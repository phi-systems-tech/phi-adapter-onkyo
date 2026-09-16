// Process entry point for the onkyo/pioneer sidecar: the adapter factory and
// the SDK's own main loop. The receiver runtime lives in onkyo_instance, the
// wire format in onkyo_frames.

#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "phi/adapter/sdk/loop_execution_backend.h"
#include "phi/adapter/sdk/sidecar.h"

#include "onkyo_endpoint.h"
#include "onkyo_instance.h"
#include "onkyo_json.h"
#include "onkyo_probe.h"
#include "onkyo_schema.h"
#include "onkyo_selector.h"
#include "onkyo_trace.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace phicore::onkyo::ipc;
using namespace std::chrono_literals;

namespace {

/// Long enough for a receiver on a busy network, short enough that walking a
/// short candidate list stays inside the factory action's own timeout.
constexpr auto kProbeTimeout = 900ms;

class OnkyoIpcFactory final : public sdk::AdapterFactory
{
protected:
    /// The SLI labels from the static config core sent at bootstrap. Read
    /// where they are needed, from the payload the host thread cached:
    /// onBootstrap runs on the factory backend thread, createInstance() and
    /// configSchema() on the host thread, and core does not wait for the
    /// bootstrap reply before creating an instance. A member written in
    /// onBootstrap was empty in createInstance once (tuya, 2026-09-09).
    std::map<std::string, std::string> schemaInputLabels() const
    {
        if (!hasBootstrap())
            return {};
        return loadConfiguredSliLabels(parseObject(bootstrap().staticConfigJson));
    }

    void onFactoryConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        (void)request;
    }

    v1::Utf8String pluginType() const override { return kPluginType; }
    v1::Utf8String displayName() const override { return phicore::onkyo::ipc::displayName(); }
    v1::Utf8String description() const override { return phicore::onkyo::ipc::description(); }
    v1::Utf8String apiVersion() const override { return "1.0.0"; }
    v1::Utf8String iconSvg() const override { return phicore::onkyo::ipc::iconSvg(); }
    int timeoutMs() const override { return 20000; }
    int maxInstances() const override { return 0; }

    v1::AdapterCapabilities capabilities() const override
    {
        return phicore::onkyo::ipc::capabilities();
    }

    std::optional<v1::AdapterConfigSchema> configSchema() const override
    {
        return phicore::onkyo::ipc::configSchema(schemaInputLabels());
    }

    // handleFactoryActionInvoke() probes every candidate host with a blocking
    // connect; on the host poll thread a multi-host probe stalled IPC for
    // seconds.
    std::unique_ptr<sdk::InstanceExecutionBackend> createFactoryExecutionBackend() override
    {
        return sdk::createLoopExecutionBackend("onkyo-factory");
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        // The loop is what the instance polls and times out on; the default
        // backend has no time in it.
        return sdk::createLoopExecutionBackend("onkyo-instance");
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        // The host owns "an instance was created"; this is the adapter noting
        // which one it is about to build.
        return makeInstance(schemaInputLabels());
    }

    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        v1::ActionResponse response = handleFactoryActionInvoke(request);
        v1::Utf8String error;
        if (!sendResult(response, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal,
                "failed to send the factory.action.invoke result: %1", {error});
    }

private:
    v1::ActionResponse handleFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request)
    {
        v1::ActionResponse response;
        response.id = request.cmdId;
        response.tsMs = nowMs();

        if (request.actionId != "probe") {
            response.status = v1::CmdStatus::NotSupported;
            response.error = "Factory action not supported";
            return response;
        }

        const Json params = parseObject(request.paramsJson);
        const std::vector<std::string> hosts = resolveProbeHosts(params);
        const std::uint16_t port = resolveProbePort(params);
        if (hosts.empty() || port == 0) {
            response.status = v1::CmdStatus::InvalidArgument;
            response.error = "Probe requires host/ip and iscpPort";
            return response;
        }

        const auto cancelled = [this]() { return stopRequested(); };
        std::string lastError;
        for (const std::string &host : hosts) {
            // Each candidate costs a connect; stop early instead of walking the
            // whole list during shutdown (F-33).
            if (stopRequested()) {
                response.status = v1::CmdStatus::Failure;
                response.error = "Probe cancelled: adapter is stopping";
                response.errorContext = "factory.action";
                return response;
            }
            std::string error;
            if (probeEndpoint(host, port, kProbeTimeout, cancelled, &error)) {
                response.status = v1::CmdStatus::Success;
                response.resultType = v1::ActionResultType::String;
                response.resultValue = host + ":" + std::to_string(port);
                return response;
            }
            if (!error.empty())
                lastError = error + " (" + host + ":" + std::to_string(port) + ")";
        }

        response.status = v1::CmdStatus::Failure;
        response.error = lastError.empty() ? "Receiver unavailable" : lastError;
        response.errorContext = "factory.action";
        return response;
    }

};

} // namespace

int main(int argc, char **argv)
{
    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-onkyo-ipc.sock"));


    OnkyoIpcFactory factory;
    sdk::SidecarHost host(socketPath, factory);

    // The SDK's own loop: start, poll, stop, with the signal handling and the
    // stuck-thread accounting that used to be copied into every adapter's main.
    return sdk::runSidecarMain(host);
}
