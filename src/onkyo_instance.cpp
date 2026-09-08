#include "onkyo_instance.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "phi/runtime/loop.h"
#include "phi/runtime/oneshots.h"
#include "phi/runtime/str.h"

#include "onkyo_devicemodel.h"
#include "onkyo_endpoint.h"
#include "onkyo_frames.h"
#include "onkyo_inputs.h"
#include "onkyo_iscp_client.h"
#include "onkyo_json.h"
#include "onkyo_queue.h"
#include "onkyo_schema.h"
#include "onkyo_selector.h"
#include "onkyo_state.h"
#include "onkyo_trace.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace std::chrono_literals;

namespace {

constexpr auto kPollReplyTimeout = 500ms;
constexpr auto kConnectTimeout = 1500ms;
constexpr auto kProbeReplyTimeout = 700ms;
constexpr auto kProbeConnectTimeout = 900ms;

/// A write is confirmed by the next poll, not by the write itself; this is how
/// long the receiver is given to have acted on it.
constexpr auto kRefreshAfterWrite = 1000ms;

/// Three failed polls before connectivity is called lost - a receiver that
/// misses one answer is not a receiver that has gone away.
constexpr int kConnectFailuresBeforeDisconnect = 3;

const std::vector<std::string> &pollCommands()
{
    static const std::vector<std::string> commands = {"PWRQSTN", "MVLQSTN", "AMTQSTN", "SLIQSTN"};
    return commands;
}

class OnkyoInstance final : public sdk::AdapterInstance
{
public:
    explicit OnkyoInstance(std::map<std::string, std::string> bootstrapInputLabels)
    {
        m_inputs.setDefaults(std::move(bootstrapInputLabels));
    }

    ~OnkyoInstance() override
    {
        // Runs on the host thread, not the loop's. Everything with an affinity
        // was released in stop(), which the host runs on the loop thread before
        // the backend goes down; touching a timer or a watch from here aborts
        // on the runtime's affinity check, and used to earn a killTimer warning
        // from Qt and a leaked timer.
    }

protected:
    bool start() override
    {
        m_loop = phi::runtime::Loop::current();
        if (m_loop == nullptr) {
            std::cerr << "onkyo instance started off a loop; no timers are possible\n";
            return false;
        }
        m_client.emplace(*m_loop);
        m_shots.emplace(*m_loop);
        m_lifecycle = Lifecycle::Running;
        m_state.forget();
        m_consecutiveConnectFailures = 0;
        setConnected(false);
        // Nothing is polled yet: start() runs before the first config.changed,
        // so there is no host to poll. The first poll is armed there, which is
        // also the moment the endpoint is known.
        return true;
    }

    void stop() override
    {
        // Terminal, and the last chance to let go of anything that belongs to
        // this loop.
        m_lifecycle = Lifecycle::Stopped;
        answerEverythingPending("Instance stopped");
        setConnected(false);
        releaseLoopResources();
    }

    void onDisconnected() override
    {
        // phi-core went away. Not terminal - it comes back and re-sends the
        // configuration, which is what starts this instance polling again - so
        // this pauses rather than stops. The old code set the same flag for
        // both and only the host's destruction order kept them apart.
        m_lifecycle = Lifecycle::Paused;
        m_pollTimer.reset();
        if (m_client)
            m_client->cancel();
        m_running.reset();
        answerEverythingPending("Instance disconnected");
        m_state.forget();
        m_consecutiveConnectFailures = 0;
        setConnected(false);
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        if (m_lifecycle == Lifecycle::Stopped)
            return;

        const std::vector<std::string> previousHosts = hosts();
        const std::uint16_t previousPort = m_controlPort;

        m_info = request.adapter;
        m_meta = parseObject(request.adapter.metaJson);
        normalizeActiveCodesInMeta();
        applyConfig();

        const bool endpointChanged =
            previousPort != m_controlPort || previousHosts != hosts();

        m_lifecycle = Lifecycle::Running;
        m_deviceId = resolveDeviceId(m_info, m_meta, firstHost());
        m_synced = false;
        m_consecutiveConnectFailures = 0;
        m_state.forget();
        if (m_client)
            m_client->cancel();
        m_running.reset();
        answerEverythingPending("Config changed");

        if (endpointChanged || !m_connected)
            setConnected(false);

        emitDeviceSnapshot();
        armPollTimer();
        m_queue.pushPoll(nowMs(), true);
        pump();

        std::cerr << "onkyo-ipc config.changed adapterId=" << request.adapterId
                  << " externalId=" << request.adapter.externalId
                  << " pluginType=" << request.adapter.pluginType << '\n';
    }

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        timingLog(str::arg("cmd.recv type=channel.invoke cmdId=%1 channel=%2",
                           request.cmdId, request.channelExternalId));
        if (m_lifecycle != Lifecycle::Running) {
            answerCommand(request.cmdId, v1::CmdStatus::TemporarilyOffline,
                          "Instance is not running", "channel.invoke");
            return;
        }
        for (const v1::CmdId superseded : m_queue.pushChannelInvoke(request, nowMs())) {
            // Never sent, and phi-core is waiting on every one of them. They are
            // answered as superseded rather than as successes: nothing reached
            // the receiver, and a success with no value is a claim this adapter
            // cannot back up.
            answerCommand(superseded, v1::CmdStatus::Success, {}, "channel.invoke.superseded");
        }
        pump();
    }

    void onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        timingLog(str::arg("cmd.recv type=adapter.action.invoke cmdId=%1 action=%2",
                           request.cmdId, request.actionId));
        if (request.actionId == "probeCurrentInput") {
            if (m_lifecycle != Lifecycle::Running) {
                answerAction(request.cmdId, v1::CmdStatus::TemporarilyOffline,
                             "Instance is not running");
                return;
            }
            m_queue.pushProbe(request, nowMs());
            pump();
            return;
        }
        if (request.actionId == "settings") {
            handleSettings(request);
            return;
        }
        answerAction(request.cmdId, v1::CmdStatus::NotSupported, "Adapter action not supported");
    }

    void onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        answerCommand(request.cmdId, v1::CmdStatus::NotImplemented,
                      "Device rename not supported", "device.name.update");
    }

    void onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        answerCommand(request.cmdId, v1::CmdStatus::NotImplemented,
                      "Device effect not supported", "device.effect.invoke");
    }

    void onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        answerCommand(request.cmdId, v1::CmdStatus::NotImplemented,
                      "Scene invocation not supported", "scene.invoke");
    }

private:
    enum class Lifecycle {
        Idle,
        Running,
        Paused,
        Stopped,
    };

    // --- configuration ----------------------------------------------------

    void applyConfig()
    {
        const std::uint16_t configured = normalizedPort(jsonInt(m_meta, "iscpPort", 0));
        const std::uint16_t discovered = normalizedPort(static_cast<int>(m_info.port));
        m_controlPort = resolvedControlPort(configured > 0 ? configured : discovered);
        m_pollIntervalMs = std::clamp(jsonInt(m_meta, "pollIntervalMs", 5000), 500, 300000);
        m_retryIntervalMs = std::clamp(jsonInt(m_meta, "retryIntervalMs", 10000), 1000, 300000);
        m_state.setVolumeMaxRaw(jsonInt(m_meta, "volumeMaxRaw", 160));
        m_inputs.applyMeta(m_meta);
    }

    void normalizeActiveCodesInMeta()
    {
        const auto raw = m_meta.find("activeSliCodes");
        if (raw == m_meta.end() || raw->is_null())
            return;
        const std::vector<std::string> normalized = normalizeActiveSliCodes(*raw);
        Json asArray = Json::array();
        for (const std::string &code : normalized)
            asArray.push_back(code);
        if (*raw == asArray)
            return;
        m_meta["activeSliCodes"] = asArray;
        m_info.metaJson = dump(m_meta);
        sendMetaPatch(Json{{"activeSliCodes", asArray}}, "config.normalize");
    }

    /**
     * @brief Where to look for the receiver, best first.
     *
     * phi-core resolves a configured hostname in the background and hands the
     * address down in `ip`; until that lands, `ip` is the name itself. This
     * used to drop anything that was not already an address, so between an
     * instance appearing and that lookup finishing every command failed with
     * "no host" instead of being tried.
     */
    std::vector<std::string> hosts() const
    {
        std::vector<std::string> candidates;
        const std::string ip = str::trimmed(m_info.ip);
        if (!ip.empty())
            candidates.push_back(ip);
        const std::string configured = str::trimmed(jsonString(m_meta, "host"));
        if (!configured.empty()
            && std::find(candidates.begin(), candidates.end(), configured) == candidates.end())
            candidates.push_back(configured);
        return candidates;
    }

    std::string firstHost() const
    {
        const std::vector<std::string> candidates = hosts();
        return candidates.empty() ? std::string() : candidates.front();
    }

    bool addressable() const { return !hosts().empty() && m_controlPort != 0; }

    // --- the loop ---------------------------------------------------------

    void armPollTimer()
    {
        if (!m_loop || m_lifecycle != Lifecycle::Running)
            return;
        const int interval = m_connected ? m_pollIntervalMs : m_retryIntervalMs;
        if (m_pollTimer && interval == m_pollTimerInterval)
            return;
        m_pollTimerInterval = interval;
        m_pollTimer = m_loop->timerEvery(std::chrono::milliseconds(interval), [this]() {
            m_queue.pushPoll(nowMs(), false);
            pump();
        });
    }

    void releaseLoopResources()
    {
        m_pollTimer.reset();
        m_pollTimerInterval = 0;
        m_shots.reset();
        if (m_client)
            m_client->cancel();
        m_client.reset();
        m_running.reset();
        m_loop = nullptr;
    }

    void pump()
    {
        if (m_lifecycle != Lifecycle::Running || !m_client)
            return;
        if (m_running.has_value() || m_client->busy())
            return;

        std::optional<Operation> next = m_queue.take();
        if (!next)
            return;

        m_running = std::move(next);
        m_operationStartedMs = nowMs();
        const std::int64_t waited = m_running->enqueuedMs > 0
            ? m_operationStartedMs - m_running->enqueuedMs
            : -1;

        switch (m_running->kind) {
        case Operation::Kind::Poll:
            timingLog(str::arg("cmd.start type=poll waitMs=%1 queueSize=%2", waited,
                               static_cast<int>(m_queue.size())));
            beginPoll();
            break;
        case Operation::Kind::ChannelInvoke:
            timingLog(str::arg("cmd.start type=channel.invoke cmdId=%1 channel=%2 waitMs=%3",
                               m_running->channelRequest.cmdId,
                               m_running->channelRequest.channelExternalId, waited));
            beginChannelInvoke();
            break;
        case Operation::Kind::ProbeCurrentInput:
            timingLog(str::arg("cmd.start type=adapter.action.invoke cmdId=%1 waitMs=%2",
                               m_running->actionRequest.cmdId, waited));
            beginProbe();
            break;
        }
    }

    void finishOperation()
    {
        if (m_running) {
            timingLog(str::arg("cmd.end type=%1 durationMs=%2",
                               static_cast<int>(m_running->kind), nowMs() - m_operationStartedMs));
        }
        m_running.reset();
        if (m_shots)
            m_shots->runOnce(0ms, [this]() { pump(); });
    }

    IscpRequest requestFor(std::vector<std::string> commands, bool awaitReplies)
    {
        IscpRequest request;
        request.hosts = hosts();
        request.port = m_controlPort;
        request.commands = std::move(commands);
        request.awaitReplies = awaitReplies;
        request.connectTimeout = kConnectTimeout;
        request.replyTimeout = kPollReplyTimeout;
        return request;
    }

    // --- the poll ---------------------------------------------------------

    void beginPoll()
    {
        if (!addressable()) {
            finishOperation();
            return;
        }
        m_client->start(requestFor(pollCommands(), true), [this](IscpResult result) {
            applyResult(result);
            armPollTimer();
            // A poll that lost one query still brought back the others, and
            // saying "failure" about it would hide three good answers. What is
            // worth knowing is how many of the four came back.
            timingLog(str::arg("poll.end status=%1 answered=%2 of %3 host=%4",
                               result.answered > 0 ? "success" : "failure",
                               result.answered,
                               static_cast<int>(pollCommands().size()),
                               result.host));
            finishOperation();
        });
    }

    void applyResult(const IscpResult &result)
    {
        if (result.connected)
            markConnectSuccess();
        else
            markConnectFailure(result.error);
        for (const ChannelReport &report : m_state.apply(result.statuses))
            emitReport(report);
    }

    // --- channel writes ---------------------------------------------------

    void beginChannelInvoke()
    {
        const sdk::ChannelInvokeRequest request = m_running->channelRequest;

        if (request.deviceExternalId != m_deviceId) {
            completeCommand(v1::CmdStatus::NotSupported, "Unknown device");
            return;
        }
        if (!addressable()) {
            completeCommand(v1::CmdStatus::TemporarilyOffline, "Receiver unavailable");
            return;
        }

        if (request.channelExternalId == kChannelPower) {
            writePower(request);
            return;
        }

        if (m_state.power() == PowerState::Unknown) {
            // The receiver has to be awake for any of the rest, and nothing has
            // said whether it is. Ask first, then carry on with the write - the
            // old code ran the same query, but inline, which is what a blocking
            // socket made it look like it could afford.
            m_client->start(requestFor(pollCommands(), true), [this](IscpResult result) {
                applyResult(result);
                continueChannelInvoke();
            });
            return;
        }
        continueChannelInvoke();
    }

    void continueChannelInvoke()
    {
        if (!m_running || m_running->kind != Operation::Kind::ChannelInvoke)
            return;
        const sdk::ChannelInvokeRequest request = m_running->channelRequest;

        if (m_state.power() == PowerState::Off) {
            completeCommand(v1::CmdStatus::Failure, "Standby");
            return;
        }
        if (m_state.power() != PowerState::On) {
            completeCommand(v1::CmdStatus::TemporarilyOffline, "Power state unknown");
            return;
        }

        if (request.channelExternalId == kChannelVolume) {
            writeVolume(request);
            return;
        }
        if (request.channelExternalId == kChannelMute) {
            writeMute(request);
            return;
        }
        if (request.channelExternalId == kChannelInput) {
            writeInput(request);
            return;
        }
        completeCommand(v1::CmdStatus::NotSupported, "Channel not supported");
    }

    void writePower(const sdk::ChannelInvokeRequest &request)
    {
        const std::optional<bool> on = scalarToBool(request.value);
        if (!on.has_value()) {
            completeCommand(v1::CmdStatus::InvalidArgument, "Power expects boolean");
            return;
        }
        const PowerState wanted = *on ? PowerState::On : PowerState::Off;
        if (m_state.power() == wanted) {
            v1::CmdResponse response = responseFor(request.cmdId, v1::CmdStatus::Success, {});
            response.finalValue = *on;
            completeCommand(std::move(response));
            return;
        }

        m_client->start(requestFor({*on ? "PWR01" : "PWR00"}, false),
                        [this, on = *on, cmdId = request.cmdId](IscpResult result) {
                            if (result.connected)
                                markConnectSuccess();
                            else
                                markConnectFailure(result.error);
                            if (!result.complete) {
                                completeCommand(v1::CmdStatus::TemporarilyOffline,
                                                "Receiver unavailable");
                                return;
                            }
                            v1::CmdResponse response =
                                responseFor(cmdId, v1::CmdStatus::Success, {});
                            response.finalValue = on;
                            if (const auto report = m_state.applyPower(on))
                                emitReport(*report);
                            completeCommand(std::move(response));
                        });
    }

    void writeVolume(const sdk::ChannelInvokeRequest &request)
    {
        const std::optional<double> wanted = scalarToDouble(request.value);
        if (!wanted.has_value()) {
            completeCommand(v1::CmdStatus::InvalidArgument, "Volume must be numeric");
            return;
        }
        const double percent = std::clamp(*wanted, 0.0, 100.0);
        const int raw = m_state.rawForPercent(percent);
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02X", raw & 0xFF);
        const auto rounded = static_cast<std::int64_t>(percent + 0.5);

        writeAndConfirm(request.cmdId, std::string("MVL") + hex, v1::ScalarValue(rounded),
                        [this, rounded]() {
                            if (const auto report = m_state.applyVolumePercent(rounded))
                                emitReport(*report);
                        });
    }

    void writeMute(const sdk::ChannelInvokeRequest &request)
    {
        const std::optional<bool> muted = scalarToBool(request.value);
        if (!muted.has_value()) {
            completeCommand(v1::CmdStatus::InvalidArgument, "Mute expects boolean");
            return;
        }
        writeAndConfirm(request.cmdId, *muted ? "AMT01" : "AMT00", v1::ScalarValue(*muted),
                        [this, muted = *muted]() {
                            if (const auto report = m_state.applyMute(muted))
                                emitReport(*report);
                        });
    }

    void writeInput(const sdk::ChannelInvokeRequest &request)
    {
        std::string wanted = str::trimmed(scalarToString(request.value));
        // A user may name an input rather than number it.
        const std::string byLabel = m_inputs.codeForLabel(wanted);
        if (!byLabel.empty())
            wanted = byLabel;
        const std::string code = normalizeSliCode(wanted);
        if (code.empty()) {
            // Anything that is not two hex digits is not a code a receiver
            // answers to. This used to go out as SLI<whatever> and be reported
            // as a success that nothing had confirmed.
            completeCommand(v1::CmdStatus::InvalidArgument,
                            "Input expects a two-digit SLI code (for example 01)");
            return;
        }
        writeAndConfirm(request.cmdId, "SLI" + code, v1::ScalarValue(code),
                        [this, code]() {
                            if (const auto report = m_state.applyInput(code))
                                emitReport(*report);
                        });
    }

    /// The shape every non-power write shares: send it, report it, and ask the
    /// receiver again shortly so what it actually did is what ends up reported.
    void writeAndConfirm(v1::CmdId cmdId,
                         std::string command,
                         v1::ScalarValue finalValue,
                         std::function<void()> onSuccess)
    {
        m_client->start(requestFor({std::move(command)}, false),
                        [this, cmdId, finalValue = std::move(finalValue),
                         onSuccess = std::move(onSuccess)](IscpResult result) {
                            if (result.connected)
                                markConnectSuccess();
                            else
                                markConnectFailure(result.error);
                            if (!result.complete) {
                                completeCommand(v1::CmdStatus::TemporarilyOffline,
                                                "Receiver unavailable");
                                return;
                            }
                            onSuccess();
                            v1::CmdResponse response =
                                responseFor(cmdId, v1::CmdStatus::Success, {});
                            response.finalValue = finalValue;
                            scheduleConfirmationPoll();
                            completeCommand(std::move(response));
                        });
    }

    void scheduleConfirmationPoll()
    {
        if (!m_shots)
            return;
        // The bag dies with this instance, so the shot cannot outlive what it
        // was going to touch - which is what a bare QTimer::singleShot(this)
        // could not promise.
        m_shots->runOnce(kRefreshAfterWrite, [this]() {
            if (m_lifecycle != Lifecycle::Running)
                return;
            m_queue.pushPoll(nowMs(), true);
            pump();
        });
    }

    // --- the probe --------------------------------------------------------

    void beginProbe()
    {
        if (!addressable()) {
            completeAction(v1::CmdStatus::TemporarilyOffline, "Receiver unavailable");
            return;
        }
        IscpRequest request = requestFor({"SLIQSTN"}, true);
        request.replyTimeout = kProbeReplyTimeout;
        request.connectTimeout = kProbeConnectTimeout;
        request.maxAttempts = 2;

        m_client->start(std::move(request), [this](IscpResult result) {
            applyResult(result);
            const std::string code = m_state.lastInputCode();
            if (result.answered == 0 && code.empty()) {
                completeAction(v1::CmdStatus::TemporarilyOffline, "Receiver unavailable");
                return;
            }
            if (code.empty()) {
                completeAction(v1::CmdStatus::Failure, "No input reported");
                return;
            }
            completeAction(buildProbeResponse(code));
        });
    }

    v1::ActionResponse buildProbeResponse(const std::string &code)
    {
        std::vector<std::string> active = m_inputs.activeCodes();
        if (std::find(active.begin(), active.end(), code) == active.end())
            active.push_back(code);

        Json activeArray = Json::array();
        for (const std::string &entry : active)
            activeArray.push_back(entry);

        Json patch = Json::object();
        patch["activeSliCodes"] = activeArray;

        const std::string labelKey = "inputLabel_" + code;
        const std::string existing = str::trimmed(jsonString(m_meta, labelKey));
        const std::string preferred = m_inputs.defaultLabel(code);
        const std::string generic = "SLI " + code;
        if (existing.empty()) {
            patch[labelKey] = preferred.empty() ? generic : preferred;
        } else if (!preferred.empty() && str::equalsIgnoreCase(existing, generic)) {
            // Repair a placeholder written before the factory knew this code.
            patch[labelKey] = preferred;
        }

        for (const auto &entry : patch.items())
            m_meta[entry.key()] = entry.value();
        m_info.metaJson = dump(m_meta);
        m_inputs.applyMeta(m_meta);

        v1::ActionResponse response;
        response.id = m_running ? m_running->actionRequest.cmdId : 0;
        response.tsMs = nowMs();
        response.status = v1::CmdStatus::Success;
        response.resultType = v1::ActionResultType::String;
        response.resultValue = code;
        response.formValuesJson = dump(Json{
            {"activeSliCodes", activeArray},
            {"currentInputCode", code},
        });
        response.fieldChoicesJson =
            dump(Json{{"activeSliCodes", optionsToChoiceJson(m_inputs.choices())}});
        response.reloadLayout = true;

        sendMetaPatch(patch, "probeCurrentInput");
        publishInputChannel();
        return response;
    }

    // --- the settings dialog ----------------------------------------------

    void handleSettings(const sdk::AdapterActionInvokeRequest &request)
    {
        const Json params = parseObject(request.paramsJson);
        if (params.empty()) {
            answerAction(request.cmdId, v1::CmdStatus::Success, {});
            return;
        }

        Json patch = Json::object();
        for (const auto &entry : params.items()) {
            const std::string key = str::trimmed(entry.key());
            if (key.empty() || key == "currentInputCode")
                continue;
            if (key == "activeSliCodes") {
                Json codes = Json::array();
                for (const std::string &code : normalizeActiveSliCodes(entry.value()))
                    codes.push_back(code);
                patch["activeSliCodes"] = codes;
                continue;
            }
            if (key.rfind("inputLabel_", 0) == 0) {
                const std::string code = normalizeSliCode(key.substr(11));
                if (code.empty())
                    continue;
                patch["inputLabel_" + code] =
                    entry.value().is_string() ? str::trimmed(entry.value().get<std::string>())
                                              : std::string();
                continue;
            }
            patch[key] = entry.value();
        }

        for (const auto &entry : patch.items())
            m_meta[entry.key()] = entry.value();
        m_info.metaJson = dump(m_meta);
        applyConfig();
        sendMetaPatch(patch, "settings");
        publishInputChannel();

        if (m_lifecycle == Lifecycle::Running) {
            armPollTimer();
            m_queue.pushPoll(nowMs(), true);
            pump();
        }
        answerAction(request.cmdId, v1::CmdStatus::Success, {});
    }

    // --- talking to phi-core ----------------------------------------------

    void emitDeviceSnapshot()
    {
        if (m_synced || m_deviceId.empty())
            return;
        const v1::Device device = buildDevice(m_deviceId, m_info, m_meta, firstHost());
        const v1::ChannelList channels = buildChannels(m_inputs.choices());
        v1::Utf8String error;
        if (!sendDeviceUpdated(device, channels, &error)) {
            std::cerr << "failed to send device snapshot: " << error << '\n';
            return;
        }
        m_synced = true;
    }

    void publishInputChannel()
    {
        if (m_deviceId.empty())
            return;
        if (!m_synced) {
            emitDeviceSnapshot();
            return;
        }
        v1::Utf8String error;
        if (!sendChannelUpdated(m_deviceId, buildInputChannel(m_inputs.choices()), &error))
            std::cerr << "failed to send channelUpdated(input): " << error << '\n';
    }

    void sendMetaPatch(const Json &patch, const char *context)
    {
        if (patch.empty())
            return;
        v1::Utf8String error;
        if (!sendAdapterMetaUpdated(dump(patch), &error))
            std::cerr << "failed to send adapterMetaUpdated(" << context << "): " << error << '\n';
    }

    void emitReport(const ChannelReport &report)
    {
        switch (report.kind) {
        case StatusKind::Power:
            emitChannelState(kChannelPower, report.flag);
            break;
        case StatusKind::Mute:
            emitChannelState(kChannelMute, report.flag);
            break;
        case StatusKind::Volume:
            emitChannelState(kChannelVolume, report.volumePercent);
            break;
        case StatusKind::Input:
            emitChannelState(kChannelInput, report.inputCode);
            break;
        }
    }

    void emitChannelState(const char *channelId, const v1::ScalarValue &value)
    {
        if (m_deviceId.empty())
            return;
        v1::Utf8String error;
        if (!sendChannelStateUpdated(m_deviceId, channelId, value, nowMs(), &error))
            std::cerr << "failed to send channel state for " << channelId << ": " << error << '\n';
    }

    void setConnected(bool connected)
    {
        if (m_connected == connected)
            return;
        m_connected = connected;
        armPollTimer();
        v1::Utf8String error;
        if (!sendConnectionStateChanged(m_connected, &error))
            std::cerr << "failed to send connectionStateChanged: " << error << '\n';
        emitChannelState(kChannelConnectivity,
                         static_cast<std::int64_t>(m_connected
                                                       ? v1::ConnectivityStatus::Connected
                                                       : v1::ConnectivityStatus::Disconnected));
    }

    void markConnectSuccess()
    {
        m_consecutiveConnectFailures = 0;
        setConnected(true);
    }

    void markConnectFailure(const std::string &error)
    {
        logConnectFailure(error);
        if (m_consecutiveConnectFailures < kConnectFailuresBeforeDisconnect)
            ++m_consecutiveConnectFailures;
        if (m_consecutiveConnectFailures < kConnectFailuresBeforeDisconnect)
            return;
        setConnected(false);
        m_state.forget();
    }

    void logConnectFailure(const std::string &error)
    {
        const std::int64_t now = nowMs();
        if (error == m_lastConnectError && (now - m_lastConnectLogMs) < m_retryIntervalMs)
            return;
        m_lastConnectError = error;
        m_lastConnectLogMs = now;
        std::cerr << "Onkyo connect failed: " << error << " port=" << m_controlPort << '\n';
    }

    // --- answering --------------------------------------------------------

    static v1::CmdResponse responseFor(v1::CmdId cmdId, v1::CmdStatus status,
                                       const std::string &error)
    {
        v1::CmdResponse response;
        response.id = cmdId;
        response.tsMs = nowMs();
        response.status = status;
        response.error = error;
        return response;
    }

    void answerCommand(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error,
                       const char *context)
    {
        v1::Utf8String sendError;
        if (!sendResult(responseFor(cmdId, status, error), &sendError))
            std::cerr << "failed to send " << context << " result: " << sendError << '\n';
    }

    void answerAction(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::ActionResponse response;
        response.id = cmdId;
        response.tsMs = nowMs();
        response.status = status;
        response.error = error;
        response.resultType = v1::ActionResultType::None;
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            std::cerr << "failed to send adapter.action.invoke result: " << sendError << '\n';
    }

    /// Answers the running channel invoke and moves on.
    void completeCommand(v1::CmdStatus status, const std::string &error)
    {
        completeCommand(responseFor(m_running ? m_running->channelRequest.cmdId : 0, status,
                                    error));
    }

    void completeCommand(v1::CmdResponse response)
    {
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            std::cerr << "failed to send channel.invoke result: " << sendError << '\n';
        finishOperation();
    }

    void completeAction(v1::CmdStatus status, const std::string &error)
    {
        v1::ActionResponse response;
        response.id = m_running ? m_running->actionRequest.cmdId : 0;
        response.tsMs = nowMs();
        response.status = status;
        response.error = error;
        response.resultType = v1::ActionResultType::None;
        completeAction(std::move(response));
    }

    void completeAction(v1::ActionResponse response)
    {
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            std::cerr << "failed to send adapter.action.invoke result: " << sendError << '\n';
        finishOperation();
    }

    /// Everything queued, and anything running, gets an answer. phi-core has a
    /// pending command for each and will wait out its timeout otherwise.
    void answerEverythingPending(const std::string &reason)
    {
        std::deque<Operation> pending = m_queue.drain();
        if (m_running)
            pending.push_front(std::move(*m_running));
        m_running.reset();

        for (const Operation &op : pending) {
            if (op.kind == Operation::Kind::ChannelInvoke) {
                answerCommand(op.channelRequest.cmdId, v1::CmdStatus::Failure, reason,
                              "channel.invoke.flush");
            } else if (op.kind == Operation::Kind::ProbeCurrentInput) {
                answerAction(op.actionRequest.cmdId, v1::CmdStatus::Failure, reason);
            }
        }
    }

    // --- scalars ----------------------------------------------------------

    static std::optional<double> scalarToDouble(const v1::ScalarValue &value)
    {
        if (const auto *v = std::get_if<double>(&value))
            return *v;
        if (const auto *v = std::get_if<std::int64_t>(&value))
            return static_cast<double>(*v);
        if (const auto *v = std::get_if<bool>(&value))
            return *v ? 1.0 : 0.0;
        if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
            bool ok = false;
            const double parsed = str::toDouble(str::trimmed(*v), &ok);
            if (ok)
                return parsed;
        }
        return std::nullopt;
    }

    static std::optional<bool> scalarToBool(const v1::ScalarValue &value)
    {
        if (const auto *v = std::get_if<bool>(&value))
            return *v;
        if (const auto *v = std::get_if<std::int64_t>(&value))
            return *v != 0;
        if (const auto *v = std::get_if<double>(&value))
            return *v != 0.0;
        if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
            const std::string text = str::toLower(str::trimmed(*v));
            if (text == "true" || text == "1" || text == "on")
                return true;
            if (text == "false" || text == "0" || text == "off")
                return false;
        }
        return std::nullopt;
    }

    static std::string scalarToString(const v1::ScalarValue &value)
    {
        if (const auto *v = std::get_if<v1::Utf8String>(&value))
            return *v;
        if (const auto *v = std::get_if<std::int64_t>(&value))
            return str::number(*v);
        if (const auto *v = std::get_if<double>(&value))
            return str::number(*v);
        if (const auto *v = std::get_if<bool>(&value))
            return *v ? "1" : "0";
        return {};
    }

    // --- members ----------------------------------------------------------

    phi::runtime::Loop *m_loop = nullptr;
    std::optional<IscpClient> m_client;
    std::optional<phi::runtime::OneShots> m_shots;
    phi::runtime::Timer m_pollTimer;
    int m_pollTimerInterval = 0;

    v1::Adapter m_info;
    Json m_meta = Json::object();
    std::string m_deviceId;
    std::uint16_t m_controlPort = 0;
    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;

    ReceiverState m_state;
    InputCatalog m_inputs;
    OperationQueue m_queue;
    std::optional<Operation> m_running;
    std::int64_t m_operationStartedMs = 0;

    Lifecycle m_lifecycle = Lifecycle::Idle;
    bool m_connected = false;
    bool m_synced = false;
    int m_consecutiveConnectFailures = 0;
    std::string m_lastConnectError;
    std::int64_t m_lastConnectLogMs = 0;
};

} // namespace

std::unique_ptr<sdk::AdapterInstance> makeInstance(
    std::map<std::string, std::string> bootstrapInputLabels)
{
    return std::make_unique<OnkyoInstance>(std::move(bootstrapInputLabels));
}

} // namespace phicore::onkyo::ipc
