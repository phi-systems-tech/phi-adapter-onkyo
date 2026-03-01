#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTcpSocket>
#include <QtGlobal>

#include "phi/adapter/sdk/sidecar.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

namespace {

constexpr const char kPluginType[] = "onkyo-pioneer";
constexpr const char kChannelPower[] = "power";
constexpr const char kChannelVolume[] = "volume";
constexpr const char kChannelMute[] = "mute";
constexpr const char kChannelInput[] = "input";
constexpr const char kChannelConnectivity[] = "connectivity";
constexpr const char kOnkyoIconSvg[] =
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
    "xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Receiver icon\">"
    "<rect x=\"3\" y=\"6\" width=\"18\" height=\"12\" rx=\"2.5\" "
    "stroke=\"#2E3A4F\" stroke-width=\"1.6\" fill=\"#121A26\"/>"
    "<circle cx=\"8\" cy=\"12\" r=\"2.2\" stroke=\"#7A8AA4\" stroke-width=\"1.4\" fill=\"none\"/>"
    "<rect x=\"13\" y=\"10.2\" width=\"7\" height=\"1.6\" rx=\"0.8\" fill=\"#7A8AA4\"/>"
    "<rect x=\"13\" y=\"13\" width=\"5\" height=\"1.6\" rx=\"0.8\" fill=\"#7A8AA4\"/>"
    "</svg>";

constexpr bool kUseEiscp = true;
constexpr bool kUseCrlf = false;

std::atomic_bool g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

std::int64_t nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

std::string toJson(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QJsonObject parseJsonObject(const std::string &text)
{
    if (text.empty())
        return {};
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

QHash<QString, QString> buildInputLabelMap()
{
    QHash<QString, QString> map;
    map.insert(QStringLiteral("00"), QStringLiteral("Video 1"));
    map.insert(QStringLiteral("01"), QStringLiteral("Video 2"));
    map.insert(QStringLiteral("02"), QStringLiteral("GAME"));
    map.insert(QStringLiteral("03"), QStringLiteral("AUX"));
    map.insert(QStringLiteral("04"), QStringLiteral("Video 5"));
    map.insert(QStringLiteral("05"), QStringLiteral("Video 6"));
    map.insert(QStringLiteral("06"), QStringLiteral("Video 7"));
    map.insert(QStringLiteral("10"), QStringLiteral("BD/DVD"));
    map.insert(QStringLiteral("12"), QStringLiteral("TV"));
    map.insert(QStringLiteral("20"), QStringLiteral("TV"));
    map.insert(QStringLiteral("21"), QStringLiteral("TV/CD"));
    map.insert(QStringLiteral("22"), QStringLiteral("Cable/Sat"));
    map.insert(QStringLiteral("23"), QStringLiteral("HDMI 1"));
    map.insert(QStringLiteral("24"), QStringLiteral("HDMI 2"));
    map.insert(QStringLiteral("25"), QStringLiteral("HDMI 3"));
    map.insert(QStringLiteral("26"), QStringLiteral("HDMI 4"));
    map.insert(QStringLiteral("30"), QStringLiteral("CD"));
    map.insert(QStringLiteral("31"), QStringLiteral("FM"));
    map.insert(QStringLiteral("32"), QStringLiteral("AM"));
    map.insert(QStringLiteral("40"), QStringLiteral("USB"));
    map.insert(QStringLiteral("41"), QStringLiteral("Network"));
    map.insert(QStringLiteral("44"), QStringLiteral("Bluetooth"));
    map.insert(QStringLiteral("2E"), QStringLiteral("BT Audio"));
    map.insert(QStringLiteral("80"), QStringLiteral("USB Front"));
    map.insert(QStringLiteral("81"), QStringLiteral("USB Rear"));
    return map;
}

QString inferModelFromIdentifier(const QString &raw)
{
    QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return {};

    const int portIndex = trimmed.lastIndexOf(QLatin1Char(':'));
    if (portIndex > 0)
        trimmed = trimmed.left(portIndex);

    if (trimmed.endsWith(QStringLiteral(".local"), Qt::CaseInsensitive))
        trimmed.chop(6);

    const QRegularExpression pattern(
        QStringLiteral("^(?:Pioneer|Onkyo)[-_ ]?(.+?)(?:-[0-9A-F]{4,12})?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(trimmed);
    if (!match.hasMatch())
        return {};

    const QString model = match.captured(1).trimmed();
    if (!model.isEmpty() && model.contains(QRegularExpression(QStringLiteral("\\d"))))
        return model;
    return {};
}

QByteArray buildEiscpFrame(const QByteArray &command)
{
    const QByteArray terminator = kUseCrlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\r");
    const QByteArray payload = QByteArrayLiteral("!1") + command + terminator;
    const quint32 dataSize = static_cast<quint32>(payload.size());

    QByteArray frame;
    frame.append("ISCP", 4);
    auto appendInt = [&frame](quint32 value) {
        frame.append(static_cast<char>((value >> 24) & 0xFF));
        frame.append(static_cast<char>((value >> 16) & 0xFF));
        frame.append(static_cast<char>((value >> 8) & 0xFF));
        frame.append(static_cast<char>(value & 0xFF));
    };
    appendInt(16);
    appendInt(dataSize);
    frame.append(char(1));
    frame.append(QByteArray(3, '\0'));
    frame.append(payload);
    return frame;
}

std::uint16_t normalizedPort(int value)
{
    if (value <= 0 || value > 65535)
        return 0;
    return static_cast<std::uint16_t>(value);
}

std::uint16_t resolvedControlPort(std::uint16_t adapterPortValue)
{
    const std::uint16_t adapterPort = normalizedPort(static_cast<int>(adapterPortValue));

    // _sues* discovery often resolves to port 80 (web UI), not ISCP.
    if (adapterPort == 0 || adapterPort == 80)
        return 60128;

    return adapterPort;
}

std::optional<double> scalarToDouble(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<double>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*v);
    if (const auto *v = std::get_if<bool>(&value))
        return *v ? 1.0 : 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        bool ok = false;
        const double parsed = QString::fromStdString(*v).toDouble(&ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

std::optional<bool> scalarToBool(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<bool>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return *v != 0;
    if (const auto *v = std::get_if<double>(&value))
        return *v != 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        const QString text = QString::fromStdString(*v).trimmed().toLower();
        if (text == QLatin1String("true") || text == QLatin1String("1") || text == QLatin1String("on"))
            return true;
        if (text == QLatin1String("false") || text == QLatin1String("0") || text == QLatin1String("off"))
            return false;
    }
    return std::nullopt;
}

QString scalarToQString(const v1::ScalarValue &value, const std::string &fallbackJson)
{
    if (const auto *v = std::get_if<v1::Utf8String>(&value))
        return QString::fromStdString(*v);
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return QString::number(*v);
    if (const auto *v = std::get_if<double>(&value))
        return QString::number(*v, 'g', 12);
    if (const auto *v = std::get_if<bool>(&value))
        return *v ? QStringLiteral("1") : QStringLiteral("0");

    if (!fallbackJson.empty()) {
        QJsonParseError err{};
        const QJsonDocument wrapper =
            QJsonDocument::fromJson(QByteArray::fromStdString("{\"v\":" + fallbackJson + "}"), &err);
        if (err.error == QJsonParseError::NoError && wrapper.isObject()) {
            const QJsonValue v = wrapper.object().value(QStringLiteral("v"));
            if (v.isString())
                return v.toString();
            if (v.isDouble())
                return QString::number(v.toDouble(), 'g', 12);
            if (v.isBool())
                return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
        }
    }
    return {};
}

v1::ScalarValue qVariantToScalarValue(const QVariant &value)
{
    if (!value.isValid() || value.isNull())
        return std::monostate{};

    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return static_cast<std::int64_t>(value.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float:
        return value.toDouble();
    default:
        return value.toString().toStdString();
    }
}

class OnkyoIpcSidecar final : public sdk::AdapterSidecar
{
public:
    void onBootstrap(const sdk::BootstrapRequest &request) override
    {
        AdapterSidecar::onBootstrap(request);

        m_info = request.adapter;
        m_meta = parseJsonObject(m_info.metaJson);

        applyConfig();
        m_synced = false;
        m_deviceId = resolveDeviceId();
        m_lastSeenMs = 0;
        m_lastConnectAttemptMs = 0;
        m_lastConnectLogMs = 0;
        m_lastConnectError.clear();
        m_stopping = false;
        m_started = true;

        setConnected(false);
        emitDeviceSnapshot();
        requestInitialState();
        scheduleNextPoll();
    }

    void onDisconnected() override
    {
        m_stopping = true;
        m_started = false;
        m_synced = false;
        setConnected(false);
    }

    v1::Utf8String displayName() const override
    {
        return "Onkyo / Pioneer";
    }

    v1::Utf8String description() const override
    {
        return "Onkyo/Pioneer AVR adapter (ISCP sidecar)";
    }

    v1::Utf8String apiVersion() const override
    {
        return v1::kProtocolLabel;
    }

    v1::Utf8String iconSvg() const override
    {
        return kOnkyoIconSvg;
    }

    int timeoutMs() const override
    {
        return m_presenceTimeoutMs;
    }

    int maxInstances() const override
    {
        return 0;
    }

    v1::AdapterCapabilities capabilities() const override
    {
        v1::AdapterCapabilities caps;
        caps.required = v1::AdapterRequirement::Host | v1::AdapterRequirement::Port;
        caps.optional = v1::AdapterRequirement::UsesRetryInterval;
        caps.flags = v1::AdapterFlag::None;

        v1::AdapterActionDescriptor probe;
        probe.id = "probe";
        probe.label = "Test connection";
        probe.description = "Validate receiver reachability using the current config values.";
        probe.hasForm = false;
        probe.metaJson = R"({"placement":"card","kind":"command","requiresAck":true})";
        caps.factoryActions.push_back(probe);

        v1::AdapterActionDescriptor settings;
        settings.id = "settings";
        settings.label = "Settings";
        settings.description = "Update adapter settings.";
        settings.hasForm = true;
        settings.metaJson = R"({"placement":"card","kind":"open_dialog","requiresAck":true})";
        caps.instanceActions.push_back(settings);

        v1::AdapterActionDescriptor probeCurrent;
        probeCurrent.id = "probeCurrentInput";
        probeCurrent.label = "Probe current";
        probeCurrent.description = "Probe current input and refresh labels.";
        probeCurrent.hasForm = true;
        probeCurrent.metaJson = R"({"placement":"form_field","kind":"command","requiresAck":true})";
        caps.instanceActions.push_back(probeCurrent);

        return caps;
    }

    v1::JsonText configSchemaJson() const override
    {
        return toJson(buildConfigSchemaObject());
    }

    v1::CmdResponse onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        if (request.deviceExternalId != m_deviceId) {
            resp.status = v1::CmdStatus::NotSupported;
            resp.error = "Unknown device";
            return resp;
        }

        if (request.channelExternalId == kChannelPower) {
            const auto on = scalarToBool(request.value);
            if (!on.has_value()) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Power expects boolean";
                return resp;
            }
            if (sendIscpCommand(*on ? QByteArrayLiteral("PWR01") : QByteArrayLiteral("PWR00"), false, 0, true)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = *on;
            } else {
                resp.status = v1::CmdStatus::TemporarilyOffline;
                resp.error = "Receiver unavailable";
            }
            return resp;
        }

        if (request.channelExternalId == kChannelVolume) {
            const auto requested = scalarToDouble(request.value);
            if (!requested.has_value()) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Volume must be numeric";
                return resp;
            }

            const double clampedPercent = qBound(0.0, *requested, 100.0);
            const int rawValue = qBound(0,
                static_cast<int>(qRound((clampedPercent / 100.0) * m_volumeMaxRaw)),
                m_volumeMaxRaw);
            const QByteArray payload =
                QByteArrayLiteral("MVL") + QByteArray::number(rawValue, 16).rightJustified(2, '0').toUpper();
            if (sendIscpCommand(payload, false, 0, true)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = clampedPercent;
            } else {
                resp.status = v1::CmdStatus::TemporarilyOffline;
                resp.error = "Receiver unavailable";
            }
            return resp;
        }

        if (request.channelExternalId == kChannelMute) {
            const auto muted = scalarToBool(request.value);
            if (!muted.has_value()) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Mute expects boolean";
                return resp;
            }
            if (sendIscpCommand(*muted ? QByteArrayLiteral("AMT01") : QByteArrayLiteral("AMT00"), false, 0, true)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = *muted;
            } else {
                resp.status = v1::CmdStatus::TemporarilyOffline;
                resp.error = "Receiver unavailable";
            }
            return resp;
        }

        if (request.channelExternalId == kChannelInput) {
            QString input = scalarToQString(request.value, request.valueJson).trimmed();
            if (input.startsWith(QLatin1String("SLI"), Qt::CaseInsensitive))
                input = input.mid(3).trimmed();

            const QString labelMatch = input.toLower();
            for (auto it = m_inputLabelMap.constBegin(); it != m_inputLabelMap.constEnd(); ++it) {
                if (it.value().toLower() == labelMatch) {
                    input = it.key();
                    break;
                }
            }

            if (input.length() != 2) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Input expects 2-digit code (e.g. 01)";
                return resp;
            }

            if (sendIscpCommand(QByteArrayLiteral("SLI") + input.toLatin1(), false, 0, true)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = input.toStdString();
            } else {
                resp.status = v1::CmdStatus::TemporarilyOffline;
                resp.error = "Receiver unavailable";
            }
            return resp;
        }

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Channel not supported";
        return resp;
    }

    v1::ActionResponse onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        v1::ActionResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        if (request.actionId == "probe") {
            const QJsonObject params = parseJsonObject(request.paramsJson);
            const QJsonObject factoryAdapter = resolveFactoryAdapterFromParams(params);
            const QString host = resolveProbeHost(factoryAdapter);
            const std::uint16_t port = resolveProbePort(factoryAdapter);
            if (host.isEmpty() || port == 0) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Probe requires host/ip and port";
                return resp;
            }

            QString errorMessage;
            if (probeEndpoint(host, port, &errorMessage)) {
                resp.status = v1::CmdStatus::Success;
                resp.resultType = v1::ActionResultType::String;
                resp.resultValue = QStringLiteral("%1:%2").arg(host).arg(port).toStdString();
            } else {
                resp.status = v1::CmdStatus::Failure;
                resp.error = errorMessage.isEmpty()
                    ? QStringLiteral("Receiver unavailable").toStdString()
                    : errorMessage.toStdString();
                resp.errorContext = "factory.action";
            }
            return resp;
        }

        if (request.actionId == "settings") {
            const QJsonObject params = parseJsonObject(request.paramsJson);
            if (!params.isEmpty()) {
                for (auto it = params.begin(); it != params.end(); ++it)
                    m_meta.insert(it.key(), it.value());
                m_info.metaJson = toJson(m_meta);
                applyConfig();
                QJsonObject patch = params;
                v1::Utf8String err;
                if (!sendAdapterMetaUpdated(toJson(patch), &err))
                    std::cerr << "failed to send adapterMetaUpdated: " << err << '\n';
                if (!sendAdapterDescriptorUpdated(descriptor(), &err))
                    std::cerr << "failed to send adapterDescriptorUpdated(settings): " << err << '\n';
                if (m_synced && !m_deviceId.empty()) {
                    v1::Utf8String chErr;
                    if (!sendChannelUpdated(m_deviceId, buildInputChannel(), &chErr))
                        std::cerr << "failed to send channelUpdated(input): " << chErr << '\n';
                } else {
                    emitDeviceSnapshot();
                }
                requestInitialState();
            }
            resp.status = v1::CmdStatus::Success;
            resp.resultType = v1::ActionResultType::None;
            return resp;
        }

        if (request.actionId == "probeCurrentInput") {
            const QString before = m_lastInputCode;
            QString resolvedCode;
            if (!sendIscpCommand(QByteArrayLiteral("SLIQSTN"), true, 1500, true)) {
                resp.status = v1::CmdStatus::TemporarilyOffline;
                resp.error = "Receiver unavailable";
            } else if (!m_lastInputCode.isEmpty()) {
                resp.status = v1::CmdStatus::Success;
                resp.resultType = v1::ActionResultType::String;
                resp.resultValue = m_lastInputCode.toStdString();
                resolvedCode = m_lastInputCode;
            } else if (!before.isEmpty()) {
                resp.status = v1::CmdStatus::Success;
                resp.resultType = v1::ActionResultType::String;
                resp.resultValue = before.toStdString();
                resolvedCode = before;
            } else {
                resp.status = v1::CmdStatus::Failure;
                resp.error = "No input reported";
            }

            if (!resolvedCode.isEmpty()) {
                QString normalized = resolvedCode.trimmed();
                if (normalized.size() == 1 && normalized[0].isDigit())
                    normalized.prepend(QLatin1Char('0'));

                QSet<QString> activeCodes;
                const QJsonValue activeValue = m_meta.value(QStringLiteral("activeSliCodes"));
                if (activeValue.isArray()) {
                    const QJsonArray activeArray = activeValue.toArray();
                    for (const QJsonValue &entry : activeArray) {
                        QString code;
                        if (entry.isString()) {
                            code = entry.toString().trimmed();
                        } else if (entry.isDouble()) {
                            const int numeric = entry.toInt();
                            code = QString::number(numeric);
                            if (code.size() == 1)
                                code.prepend(QLatin1Char('0'));
                        }
                        if (!code.isEmpty())
                            activeCodes.insert(code);
                    }
                }
                activeCodes.insert(normalized);

                QJsonArray nextActive;
                for (const QString &code : std::as_const(activeCodes))
                    nextActive.append(code);

                QJsonObject patch;
                patch.insert(QStringLiteral("activeSliCodes"), nextActive);
                const QString labelKey = QStringLiteral("inputLabel_%1").arg(normalized);
                const QString existingLabel = m_meta.value(labelKey).toString().trimmed();
                const QString defaultLabel = buildInputLabelMap().value(normalized).trimmed();
                const QString fallbackLabel = QStringLiteral("SLI %1").arg(normalized);
                const auto isGenericSliLabel = [&normalized](const QString &label) {
                    QString compact = label.trimmed();
                    compact.remove(QLatin1Char(' '));
                    return compact.compare(QStringLiteral("SLI%1").arg(normalized), Qt::CaseInsensitive) == 0;
                };
                if (existingLabel.isEmpty()) {
                    patch.insert(labelKey, defaultLabel.isEmpty() ? fallbackLabel : defaultLabel);
                } else if (!defaultLabel.isEmpty()
                           && isGenericSliLabel(existingLabel)) {
                    // Repair old generic fallback labels once a known default exists.
                    patch.insert(labelKey, defaultLabel);
                }

                for (auto it = patch.begin(); it != patch.end(); ++it)
                    m_meta.insert(it.key(), it.value());
                m_info.metaJson = toJson(m_meta);
                applyConfig();

                v1::Utf8String err;
                if (!sendAdapterMetaUpdated(toJson(patch), &err))
                    std::cerr << "failed to send adapterMetaUpdated(probeCurrentInput): " << err << '\n';
                if (!sendAdapterDescriptorUpdated(descriptor(), &err))
                    std::cerr << "failed to send adapterDescriptorUpdated(probeCurrentInput): " << err << '\n';
            }
            return resp;
        }

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Adapter action not supported";
        return resp;
    }

    v1::CmdResponse onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device rename not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    v1::CmdResponse onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device effect not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    v1::CmdResponse onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Scene invocation not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    void tick()
    {
        if (!m_started || m_stopping)
            return;

        if (!m_synced)
            emitDeviceSnapshot();

        const std::int64_t now = nowMs();
        if (now >= m_nextPollDueMs) {
            requestInitialState();
            scheduleNextPoll();
        }

        if (m_lastSeenMs > 0 && m_connected && (now - m_lastSeenMs > m_presenceTimeoutMs)) {
            setConnected(false);
            v1::Utf8String err;
            if (!sendChannelStateUpdated(m_deviceId,
                                         kChannelConnectivity,
                                         static_cast<std::int64_t>(v1::ConnectivityStatus::Disconnected),
                                         now,
                                         &err)) {
                std::cerr << "failed to send disconnected connectivity state: " << err << '\n';
            }
        }
    }

private:
    QJsonObject resolveFactoryAdapterFromParams(const QJsonObject &params) const
    {
        const QJsonObject factoryAdapter = params.value(QStringLiteral("factoryAdapter")).toObject();
        if (!factoryAdapter.isEmpty())
            return factoryAdapter;
        return params;
    }

    QString resolveProbeHost(const QJsonObject &factoryAdapter) const
    {
        auto hostFromObject = [](const QJsonObject &obj) -> QString {
            const QString ip = obj.value(QStringLiteral("ip")).toString().trimmed();
            if (!ip.isEmpty())
                return ip;
            return obj.value(QStringLiteral("host")).toString().trimmed();
        };

        QString host = hostFromObject(factoryAdapter);
        if (!host.isEmpty())
            return host;

        const QJsonObject meta = factoryAdapter.value(QStringLiteral("meta")).toObject();
        host = hostFromObject(meta);
        if (!host.isEmpty())
            return host;

        const QStringList hostCandidates = effectiveHosts();
        return hostCandidates.isEmpty() ? QString() : hostCandidates.front();
    }

    std::uint16_t resolveProbePort(const QJsonObject &factoryAdapter) const
    {
        auto parsePort = [](const QJsonValue &value) -> std::uint16_t {
            if (!value.isDouble())
                return 0;
            return normalizedPort(value.toInt());
        };

        std::uint16_t port = parsePort(factoryAdapter.value(QStringLiteral("port")));
        if (port == 0) {
            const QJsonObject meta = factoryAdapter.value(QStringLiteral("meta")).toObject();
            port = parsePort(meta.value(QStringLiteral("port")));
        }

        if (port != 0)
            return resolvedControlPort(port);
        return m_controlPort;
    }

    bool probeEndpoint(const QString &host, std::uint16_t port, QString *errorMessage) const
    {
        QTcpSocket socket;
        socket.connectToHost(host, port);
        if (!socket.waitForConnected(1500)) {
            if (errorMessage)
                *errorMessage = socket.errorString();
            return false;
        }
        socket.disconnectFromHost();
        return true;
    }

    QJsonArray activeSliCodesFromMeta() const
    {
        QJsonArray out;
        const QJsonValue activeValue = m_meta.value(QStringLiteral("activeSliCodes"));
        if (!activeValue.isArray())
            return out;
        const QJsonArray arr = activeValue.toArray();
        for (const QJsonValue &entry : arr) {
            if (entry.isString()) {
                const QString code = entry.toString().trimmed().toUpper();
                if (!code.isEmpty())
                    out.append(code);
                continue;
            }
            if (entry.isDouble()) {
                QString code = QString::number(entry.toInt());
                if (code.size() == 1)
                    code.prepend(QLatin1Char('0'));
                out.append(code);
            }
        }
        return out;
    }

    QJsonArray inputChoicesForSchema() const
    {
        QJsonArray choices;
        for (auto it = m_inputLabelMap.constBegin(); it != m_inputLabelMap.constEnd(); ++it) {
            QJsonObject option;
            option.insert(QStringLiteral("value"), it.key());
            option.insert(QStringLiteral("label"),
                          it.value().isEmpty()
                              ? QStringLiteral("SLI %1").arg(it.key())
                              : it.value());
            choices.append(option);
        }
        return choices;
    }

    QJsonObject buildConfigSchemaObject() const
    {
        auto field = [](QString key,
                        QString type,
                        QString label,
                        QJsonValue defaultValue = QJsonValue(),
                        QString actionId = QString(),
                        QString actionLabel = QString(),
                        QString parentActionId = QString(),
                        QJsonArray flags = QJsonArray(),
                        QJsonValue choices = QJsonValue(),
                        QJsonObject meta = QJsonObject()) {
            QJsonObject obj;
            obj.insert(QStringLiteral("key"), key);
            obj.insert(QStringLiteral("type"), type);
            obj.insert(QStringLiteral("label"), label);
            if (!defaultValue.isUndefined() && !defaultValue.isNull())
                obj.insert(QStringLiteral("default"), defaultValue);
            if (!actionId.isEmpty())
                obj.insert(QStringLiteral("actionId"), actionId);
            if (!actionLabel.isEmpty())
                obj.insert(QStringLiteral("actionLabel"), actionLabel);
            if (!parentActionId.isEmpty())
                obj.insert(QStringLiteral("parentActionId"), parentActionId);
            if (!flags.isEmpty())
                obj.insert(QStringLiteral("flags"), flags);
            if (choices.isArray())
                obj.insert(QStringLiteral("choices"), choices);
            if (!meta.isEmpty())
                obj.insert(QStringLiteral("meta"), meta);
            return obj;
        };

        QJsonArray factoryFields;
        const QString resolvedHost = !QString::fromStdString(m_info.ip).trimmed().isEmpty()
            ? QString::fromStdString(m_info.ip).trimmed()
            : QString::fromStdString(m_info.host).trimmed();
        if (!resolvedHost.isEmpty()) {
            factoryFields.append(field(QStringLiteral("host"),
                                       QStringLiteral("Hostname"),
                                       QStringLiteral("Host"),
                                       resolvedHost));
        } else {
            factoryFields.append(field(QStringLiteral("host"),
                                       QStringLiteral("Hostname"),
                                       QStringLiteral("Host")));
        }
        const int defaultPort = (m_info.port > 0 && m_info.port != 80)
            ? static_cast<int>(m_info.port)
            : 60128;
        factoryFields.append(field(QStringLiteral("port"),
                                   QStringLiteral("Port"),
                                   QStringLiteral("ISCP Port"),
                                   defaultPort));
        factoryFields.append(field(QStringLiteral("pollIntervalMs"),
                                   QStringLiteral("Integer"),
                                   QStringLiteral("Poll interval"),
                                   m_pollIntervalMs));
        factoryFields.append(field(QStringLiteral("retryIntervalMs"),
                                   QStringLiteral("Integer"),
                                   QStringLiteral("Retry interval"),
                                   m_retryIntervalMs));

        QJsonArray instanceFields;
        instanceFields.append(field(QStringLiteral("volumeMaxRaw"),
                                    QStringLiteral("Integer"),
                                    QStringLiteral("Max volume raw"),
                                    m_volumeMaxRaw,
                                    QString(),
                                    QString(),
                                    QStringLiteral("settings"),
                                    QJsonArray{QStringLiteral("InstanceOnly")}));
        instanceFields.append(field(QStringLiteral("activeSliCodes"),
                                    QStringLiteral("Select"),
                                    QStringLiteral("Active SLI codes"),
                                    activeSliCodesFromMeta(),
                                    QString(),
                                    QString(),
                                    QStringLiteral("settings"),
                                    QJsonArray{QStringLiteral("Multi"), QStringLiteral("InstanceOnly")},
                                    inputChoicesForSchema()));
        instanceFields.append(field(QStringLiteral("currentInputCode"),
                                    QStringLiteral("String"),
                                    QStringLiteral("Current input (SLI)"),
                                    QJsonValue(),
                                    QStringLiteral("probeCurrentInput"),
                                    QStringLiteral("Probe current"),
                                    QStringLiteral("settings"),
                                    QJsonArray{
                                        QStringLiteral("ReadOnly"),
                                        QStringLiteral("Transient"),
                                        QStringLiteral("InstanceOnly"),
                                    },
                                    QJsonValue(),
                                    QJsonObject{{QStringLiteral("appendTo"), QStringLiteral("activeSliCodes")}}));

        QJsonObject factorySection;
        factorySection.insert(QStringLiteral("title"), QStringLiteral("Connection"));
        factorySection.insert(QStringLiteral("fields"), factoryFields);

        QJsonObject instanceSection;
        instanceSection.insert(QStringLiteral("title"), QStringLiteral("Settings"));
        instanceSection.insert(QStringLiteral("fields"), instanceFields);

        QJsonObject schema;
        schema.insert(QStringLiteral("factory"), factorySection);
        schema.insert(QStringLiteral("instance"), instanceSection);
        return schema;
    }

    void applyConfig()
    {
        m_controlPort = resolvedControlPort(m_info.port);
        m_pollIntervalMs = qBound(500,
                                  m_meta.value(QStringLiteral("pollIntervalMs")).toInt(5000),
                                  300000);
        m_retryIntervalMs = qBound(1000,
                                   m_meta.value(QStringLiteral("retryIntervalMs")).toInt(10000),
                                   300000);
        m_presenceTimeoutMs = m_pollIntervalMs + 1000;
        m_volumeMaxRaw = qBound(1,
                                m_meta.value(QStringLiteral("volumeMaxRaw")).toInt(160),
                                500);
        reloadInputLabelMap();
    }

    QStringList effectiveHosts() const
    {
        QStringList result;
        const QString ip = QString::fromStdString(m_info.ip).trimmed();
        if (!ip.isEmpty())
            result.push_back(ip);
        return result;
    }

    void scheduleNextPoll()
    {
        const std::int64_t interval = m_connected ? m_pollIntervalMs : m_retryIntervalMs;
        m_nextPollDueMs = nowMs() + interval;
    }

    bool canAttemptConnect() const
    {
        return (nowMs() - m_lastConnectAttemptMs) >= m_retryIntervalMs;
    }

    void markConnectAttempt()
    {
        m_lastConnectAttemptMs = nowMs();
    }

    void logConnectFailure(const QString &error, const QString &host)
    {
        const std::int64_t now = nowMs();
        const QString msg = QStringLiteral("%1|%2").arg(error, host);
        if (msg == m_lastConnectError && (now - m_lastConnectLogMs) < m_retryIntervalMs)
            return;
        m_lastConnectError = msg;
        m_lastConnectLogMs = now;
        std::cerr << "Onkyo connect failed: " << error.toStdString()
                  << " host=" << host.toStdString()
                  << " port=" << m_controlPort << '\n';
    }

    bool sendIscpCommand(const QByteArray &command,
                         bool parseResponse,
                         int responseTimeoutMs,
                         bool bypassRetryGate = false)
    {
        const int connectTimeoutMs = 1500;
        const QStringList hostCandidates = effectiveHosts();
        if (hostCandidates.isEmpty() || m_controlPort == 0)
            return false;
        if (!bypassRetryGate && !m_connected && !canAttemptConnect())
            return false;

        QTcpSocket socket;
        socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        markConnectAttempt();

        for (const QString &host : hostCandidates) {
            socket.abort();
            socket.connectToHost(host, m_controlPort);

            int waitedMs = 0;
            while (!socket.waitForConnected(100)) {
                waitedMs += 100;
                if (waitedMs >= connectTimeoutMs) {
                    if (socket.state() != QAbstractSocket::ConnectedState)
                        logConnectFailure(socket.errorString(), host);
                    break;
                }
            }
            if (socket.state() == QAbstractSocket::ConnectedState)
                break;
        }

        if (socket.state() != QAbstractSocket::ConnectedState)
            return false;

        markSeen();

        if (!kUseEiscp) {
            const QByteArray terminator = kUseCrlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\r");
            socket.write(QByteArrayLiteral("!1") + command + terminator);
            socket.flush();
            if (parseResponse && responseTimeoutMs > 0 && socket.waitForReadyRead(responseTimeoutMs)) {
                const QByteArray data = socket.readAll();
                if (!data.isEmpty())
                    processResponseData(data);
            }
            socket.disconnectFromHost();
            return true;
        }

        const QByteArray frame = buildEiscpFrame(command);
        socket.write(frame);
        socket.flush();

        if (parseResponse && responseTimeoutMs > 0) {
            QByteArray data;
            int readWaitedMs = 0;
            while (readWaitedMs < responseTimeoutMs) {
                if (socket.waitForReadyRead(100)) {
                    data.append(socket.readAll());
                    while (socket.waitForReadyRead(50))
                        data.append(socket.readAll());
                    break;
                }
                readWaitedMs += 100;
            }
            if (!data.isEmpty())
                processResponseData(data);
        }

        socket.disconnectFromHost();
        return true;
    }

    void processResponseData(const QByteArray &data)
    {
        if (data.isEmpty())
            return;

        if (!kUseEiscp) {
            handleIscpPayload(data);
            return;
        }

        int offset = 0;
        while (offset + 16 <= data.size()) {
            const int headerIndex = data.indexOf("ISCP", offset);
            if (headerIndex < 0 || headerIndex + 16 > data.size())
                return;

            const unsigned char *header =
                reinterpret_cast<const unsigned char *>(data.constData() + headerIndex);
            auto readInt = [](const unsigned char *ptr) -> quint32 {
                return (static_cast<quint32>(ptr[0]) << 24)
                    | (static_cast<quint32>(ptr[1]) << 16)
                    | (static_cast<quint32>(ptr[2]) << 8)
                    | static_cast<quint32>(ptr[3]);
            };

            const quint32 headerSize = readInt(header + 4);
            const quint32 dataSize = readInt(header + 8);
            const int frameSize = static_cast<int>(headerSize + dataSize);
            if (headerIndex + frameSize > data.size())
                return;

            const QByteArray payload = data.mid(headerIndex + static_cast<int>(headerSize),
                                                static_cast<int>(dataSize));
            handleIscpPayload(payload);
            offset = headerIndex + frameSize;
        }
    }

    void handleIscpPayload(const QByteArray &payload)
    {
        auto sanitizeLine = [](QByteArray line) {
            line = line.trimmed();
            while (!line.isEmpty()) {
                const unsigned char last = static_cast<unsigned char>(line.at(line.size() - 1));
                if (last < 0x20 || last == 0x7F) {
                    line.chop(1);
                } else {
                    break;
                }
            }
            return line;
        };

        const QList<QByteArray> parts = payload.split('\r');
        for (QByteArray line : parts) {
            line = sanitizeLine(line);
            if (line.isEmpty())
                continue;
            if (line.startsWith("!1"))
                line = line.mid(2);
            line = sanitizeLine(line);

            if (line.startsWith("PWR")) {
                const QByteArray value = line.mid(3);
                if (value == "01" || value == "00") {
                    emitChannelState(QString::fromLatin1(kChannelPower), value == "01");
                    markSeen();
                }
                continue;
            }

            if (line.startsWith("AMT")) {
                const QByteArray value = line.mid(3);
                if (value == "01" || value == "00") {
                    emitChannelState(QString::fromLatin1(kChannelMute), value == "01");
                    markSeen();
                }
                continue;
            }

            if (line.startsWith("MVL")) {
                const QByteArray value = line.mid(3);
                bool ok = false;
                const int parsed = value.toInt(&ok, 16);
                if (ok) {
                    const int rawClamped = qBound(0, parsed, m_volumeMaxRaw);
                    const double normalized = (static_cast<double>(rawClamped) / m_volumeMaxRaw) * 100.0;
                    emitChannelState(QString::fromLatin1(kChannelVolume), normalized);
                    markSeen();
                }
                continue;
            }

            if (line.startsWith("SLI")) {
                QString code = QString::fromLatin1(line.mid(3)).trimmed().toUpper();
                static const QRegularExpression kCodeRe(QStringLiteral("^[0-9A-F]{2}$"));
                if (kCodeRe.match(code).hasMatch()) {
                    m_lastInputCode = code;
                    emitChannelState(QString::fromLatin1(kChannelInput), m_lastInputCode.toStdString());
                    markSeen();
                }
                continue;
            }
        }
    }

    void emitDeviceSnapshot()
    {
        if (m_synced || m_deviceId.empty())
            return;

        v1::Device device;
        device.externalId = m_deviceId;
        device.deviceClass = v1::DeviceClass::MediaPlayer;

        const QString adapterName = QString::fromStdString(m_info.name).trimmed();
        const QString metaName = m_meta.value(QStringLiteral("deviceName")).toString().trimmed();
        const QStringList hostCandidates = effectiveHosts();
        const QString host = hostCandidates.isEmpty() ? QString() : hostCandidates.front();
        if (!adapterName.isEmpty()) {
            device.name = adapterName.toStdString();
        } else if (!metaName.isEmpty()) {
            device.name = metaName.toStdString();
        } else {
            device.name = host.toStdString();
        }

        QString manufacturer = m_meta.value(QStringLiteral("manufacturer")).toString().trimmed();
        if (manufacturer.isEmpty())
            manufacturer = QStringLiteral("Onkyo & Pioneer");
        device.manufacturer = manufacturer.toStdString();

        QString model = m_meta.value(QStringLiteral("model")).toString().trimmed();
        if (model.isEmpty()) {
            const QStringList candidates = {
                host,
                m_meta.value(QStringLiteral("deviceUuid")).toString(),
                m_meta.value(QStringLiteral("uuid")).toString(),
                m_meta.value(QStringLiteral("deviceName")).toString(),
                QString::fromStdString(m_info.name),
            };
            for (const QString &candidate : candidates) {
                model = inferModelFromIdentifier(candidate);
                if (!model.isEmpty())
                    break;
            }
        }
        device.model = model.toStdString();

        QJsonObject meta;
        if (m_meta.value(QStringLiteral("supportsSpotify")).toBool())
            meta.insert(QStringLiteral("supportsSpotify"), true);
        if (m_meta.value(QStringLiteral("supportsTranscoder")).toBool())
            meta.insert(QStringLiteral("supportsTranscoder"), true);
        device.metaJson = toJson(meta);

        v1::ChannelList channels;

        v1::Channel power;
        power.externalId = kChannelPower;
        power.name = "Power";
        power.kind = v1::ChannelKind::PowerOnOff;
        power.dataType = v1::ChannelDataType::Bool;
        power.flags = v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;
        channels.push_back(power);

        v1::Channel volume;
        volume.externalId = kChannelVolume;
        volume.name = "Volume";
        volume.kind = v1::ChannelKind::Volume;
        volume.dataType = v1::ChannelDataType::Float;
        volume.flags = v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;
        volume.minValue = 0.0;
        volume.maxValue = 100.0;
        volume.stepValue = 1.0;
        channels.push_back(volume);

        v1::Channel mute;
        mute.externalId = kChannelMute;
        mute.name = "Mute";
        mute.kind = v1::ChannelKind::Mute;
        mute.dataType = v1::ChannelDataType::Bool;
        mute.flags = v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;
        channels.push_back(mute);

        v1::Channel input;
        input.externalId = kChannelInput;
        input.name = "Input";
        input.kind = v1::ChannelKind::HdmiInput;
        input.dataType = v1::ChannelDataType::String;
        input.flags = v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;
        channels.push_back(input);

        v1::Channel connectivity;
        connectivity.externalId = kChannelConnectivity;
        connectivity.name = "Connectivity";
        connectivity.kind = v1::ChannelKind::ConnectivityStatus;
        connectivity.dataType = v1::ChannelDataType::Enum;
        connectivity.flags = v1::ChannelFlag::Readable | v1::ChannelFlag::Reportable;
        channels.push_back(connectivity);

        v1::Utf8String err;
        if (!sendDeviceUpdated(device, channels, &err)) {
            std::cerr << "failed to send device snapshot: " << err << '\n';
            return;
        }
        if (!sendFullSyncCompleted(&err))
            std::cerr << "failed to send fullSyncCompleted: " << err << '\n';

        m_synced = true;
    }

    v1::Channel buildInputChannel() const
    {
        v1::Channel input;
        input.externalId = kChannelInput;
        input.name = "Input";
        input.kind = v1::ChannelKind::HdmiInput;
        input.dataType = v1::ChannelDataType::String;
        input.flags = v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;
        return input;
    }

    std::string resolveDeviceId() const
    {
        const QString uuid = m_meta.value(QStringLiteral("deviceUuid")).toString().trimmed();
        if (!uuid.isEmpty())
            return uuid.toStdString();

        const QString legacyUuid = m_meta.value(QStringLiteral("uuid")).toString().trimmed();
        if (!legacyUuid.isEmpty())
            return legacyUuid.toStdString();

        if (!m_info.externalId.empty())
            return m_info.externalId;

        const QStringList hostCandidates = effectiveHosts();
        const QString host = hostCandidates.isEmpty() ? QString() : hostCandidates.front();
        if (!host.isEmpty())
            return host.toStdString();

        return "onkyo-pioneer";
    }

    void requestInitialState()
    {
        if (!m_started || m_stopping)
            return;
        if (effectiveHosts().isEmpty() || m_controlPort == 0)
            return;

        const std::vector<QByteArray> commands = {
            QByteArrayLiteral("PWRQSTN"),
            QByteArrayLiteral("AMTQSTN"),
            QByteArrayLiteral("MVLQSTN"),
            QByteArrayLiteral("SLIQSTN"),
        };
        for (const QByteArray &cmd : commands)
            sendIscpCommand(cmd, true, 800);
    }

    void reloadInputLabelMap()
    {
        m_inputLabelMap = buildInputLabelMap();

        QSet<QString> activeCodeSet;
        const QJsonValue activeCodesValue = m_meta.value(QStringLiteral("activeSliCodes"));
        if (activeCodesValue.isArray()) {
            QHash<QString, QString> filtered;
            const QJsonArray activeCodes = activeCodesValue.toArray();
            for (const QJsonValue &entry : activeCodes) {
                QString code;
                if (entry.isString()) {
                    code = entry.toString().trimmed();
                } else if (entry.isDouble()) {
                    const int numeric = entry.toInt();
                    code = QString::number(numeric);
                    if (code.size() == 1)
                        code.prepend(QLatin1Char('0'));
                }
                if (code.isEmpty())
                    continue;
                activeCodeSet.insert(code);
                QString label = m_inputLabelMap.value(code);
                if (label.isEmpty())
                    label = QStringLiteral("SLI %1").arg(code);
                filtered.insert(code, label);
            }
            if (!filtered.isEmpty())
                m_inputLabelMap = filtered;
        }

        for (auto it = m_meta.begin(); it != m_meta.end(); ++it) {
            const QString key = it.key();
            if (!key.startsWith(QLatin1String("inputLabel_")))
                continue;
            const QString code = key.mid(11).trimmed();
            if (code.isEmpty())
                continue;
            QString label = it.value().toString().trimmed();
            if (label.isEmpty())
                label = QStringLiteral("SLI %1").arg(code);
            if (activeCodeSet.isEmpty() || activeCodeSet.contains(code))
                m_inputLabelMap.insert(code, label);
        }
    }

    void setConnected(bool connected)
    {
        if (m_connected == connected)
            return;
        m_connected = connected;
        v1::Utf8String err;
        if (!sendConnectionStateChanged(m_connected, &err))
            std::cerr << "failed to send connectionStateChanged: " << err << '\n';
    }

    void markSeen()
    {
        m_lastSeenMs = nowMs();
        if (!m_connected)
            setConnected(true);
        emitChannelState(QString::fromLatin1(kChannelConnectivity),
                         static_cast<std::int64_t>(v1::ConnectivityStatus::Connected));
    }

    void emitChannelState(const QString &channelId, const v1::ScalarValue &value)
    {
        if (m_deviceId.empty())
            return;
        v1::Utf8String err;
        if (!sendChannelStateUpdated(m_deviceId, channelId.toStdString(), value, nowMs(), &err))
            std::cerr << "failed to send channel state for " << channelId.toStdString() << ": " << err << '\n';
    }

    v1::Adapter m_info;
    QJsonObject m_meta;

    bool m_started = false;
    bool m_stopping = false;
    bool m_connected = false;
    bool m_synced = false;

    std::string m_deviceId;
    std::uint16_t m_controlPort = 0;
    int m_presenceTimeoutMs = 15000;
    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    int m_volumeMaxRaw = 160;

    std::int64_t m_nextPollDueMs = 0;
    std::int64_t m_lastSeenMs = 0;
    std::int64_t m_lastConnectAttemptMs = 0;
    std::int64_t m_lastConnectLogMs = 0;

    QString m_lastConnectError;
    QString m_lastInputCode;
    QHash<QString, QString> m_inputLabelMap;
};

class OnkyoIpcFactory final : public sdk::AdapterFactory
{
public:
    v1::Utf8String pluginType() const override
    {
        return kPluginType;
    }

    std::unique_ptr<sdk::AdapterSidecar> create() const override
    {
        return std::make_unique<OnkyoIpcSidecar>();
    }
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-onkyo-ipc.sock"));

    std::cerr << "starting phi_adapter_onkyo_ipc for pluginType=" << kPluginType
              << " socket=" << socketPath << '\n';

    OnkyoIpcFactory factory;
    sdk::SidecarHost host(socketPath, factory);

    v1::Utf8String error;
    if (!host.start(&error)) {
        std::cerr << "failed to start sidecar host: " << error << '\n';
        return 1;
    }

    while (g_running.load()) {
        if (!host.pollOnce(std::chrono::milliseconds(250), &error)) {
            std::cerr << "poll failed: " << error << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (auto *adapter = dynamic_cast<OnkyoIpcSidecar *>(host.adapter()))
            adapter->tick();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    host.stop();
    return 0;
}
