// Process entry point for the onkyo/pioneer sidecar: the adapter factory and
// the Qt main loop that drives the SDK host. The receiver runtime itself lives
// in onkyo_sidecar, the wire format in onkyo_protocol.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <QCoreApplication>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "phi/adapter/sdk/qt/instance_execution_backend_qt.h"
#include "phi/adapter/sdk/qt/sidecar_driver_qt.h"
#include "phi/adapter/sdk/sidecar.h"

#include "onkyo_probe.h"
#include "onkyo_protocol.h"
#include "onkyo_runtime_convert.h"
#include "onkyo_schema.h"
#include "onkyo_sidecar.h"
#include "onkyo_trace.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace phicore::onkyo::ipc;

namespace {

std::atomic_bool g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

class OnkyoIpcFactory final : public sdk::AdapterFactory
{
protected:
    void onBootstrap(const sdk::BootstrapRequest &request) override
    {
        // Runs on the factory backend thread; the SDK sends the bootstrap
        // descriptor from the same task, so configSchemaJson() and
        // createInstance() (host thread, both reachable only after that reply)
        // see the labels written here.
        m_schemaInputLabels = loadConfiguredSliLabels(parseJsonObject(request.staticConfigJson));
    }

    void onFactoryConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        (void)request;
    }

    v1::Utf8String pluginType() const override
    {
        return kPluginType;
    }

    v1::Utf8String displayName() const override
    {
        return phicore::onkyo::ipc::displayName();
    }

    v1::Utf8String description() const override
    {
        return phicore::onkyo::ipc::description();
    }

    v1::Utf8String apiVersion() const override
    {
        return "1.0.0";
    }

    v1::Utf8String iconSvg() const override
    {
        return phicore::onkyo::ipc::iconSvg();
    }

    int timeoutMs() const override
    {
        return 20000;
    }

    int maxInstances() const override
    {
        return 0;
    }

    v1::AdapterCapabilities capabilities() const override
    {
        return phicore::onkyo::ipc::capabilities();
    }

    v1::JsonText configSchemaJson() const override
    {
        return phicore::onkyo::ipc::configSchemaJson(m_schemaInputLabels);
    }


    // handleFactoryActionInvoke() probes every candidate host with a blocking
    // QTcpSocket::waitForConnected(900); on the host poll thread a multi-host
    // probe stalled IPC for seconds.
    std::unique_ptr<sdk::InstanceExecutionBackend> createFactoryExecutionBackend() override
    {
        return sdk::qt::createFactoryExecutionBackend();
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return sdk::qt::createInstanceExecutionBackend();
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        std::cerr << "create onkyo instance externalId=" << externalId << '\n';
        return makeInstance(m_schemaInputLabels);
    }

    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        submitFactoryActionResult(handleFactoryActionInvoke(request), "factory.action.invoke");
    }

private:
    v1::ActionResponse handleFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request)
    {
        trace(QStringLiteral("factory action invoke id=%1 action=%2")
                  .arg(request.cmdId)
                  .arg(QString::fromStdString(request.actionId)));
        v1::ActionResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        if (request.actionId != "probe") {
            resp.status = v1::CmdStatus::NotSupported;
            resp.error = "Factory action not supported";
            return resp;
        }

        const QJsonObject params = parseJsonObject(request.paramsJson);
        const QStringList hosts = resolveProbeHosts(params);
        const std::uint16_t port = resolveProbePort(params);
        if (hosts.isEmpty() || port == 0) {
            resp.status = v1::CmdStatus::InvalidArgument;
            resp.error = "Probe requires host/ip and iscpPort";
            return resp;
        }

        QString errorMessage;
        QString successfulHost;
        for (const QString &host : hosts) {
            // Each candidate costs a blocking connect; stop early instead of
            // walking the whole list during shutdown (F-33).
            if (stopRequested()) {
                resp.status = v1::CmdStatus::Failure;
                resp.error = "Probe cancelled: adapter is stopping";
                resp.errorContext = "factory.action";
                return resp;
            }
            QString hostError;
            if (probeEndpoint(host, port, &hostError)) {
                successfulHost = host;
                break;
            }
            if (!hostError.isEmpty())
                errorMessage = QStringLiteral("%1 (%2:%3)").arg(hostError, host).arg(port);
        }

        if (!successfulHost.isEmpty()) {
            resp.status = v1::CmdStatus::Success;
            resp.resultType = v1::ActionResultType::String;
            resp.resultValue = QStringLiteral("%1:%2").arg(successfulHost).arg(port).toStdString();
        } else {
            resp.status = v1::CmdStatus::Failure;
            resp.error = errorMessage.isEmpty()
                ? QStringLiteral("Receiver unavailable").toStdString()
                : errorMessage.toStdString();
            resp.errorContext = "factory.action";
        }
        return resp;
    }

    void submitFactoryActionResult(v1::ActionResponse response, const char *context)
    {
        v1::Utf8String err;
        if (!sendResult(response, &err))
            std::cerr << "failed to send " << context << " result: " << err << '\n';
    }

    QHash<QString, QString> m_schemaInputLabels;
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

    // The driver watches the host's poll descriptor from the Qt event loop:
    // no polling interval, no idle wakeups, and the Qt event loop is no longer
    // starved by a blocking poll (adapter timers now run on time).
    phicore::adapter::sdk::qt::SidecarDriver driver(host);

    v1::Utf8String error;
    if (!driver.start(&error)) {
        std::cerr << "failed to start sidecar host: " << error << '\n';
        return 1;
    }

    // Signal handlers only flip a flag; a slow timer turns it into a clean
    // Qt shutdown.
    QTimer shutdownTimer;
    QObject::connect(&shutdownTimer, &QTimer::timeout, [&]() {
        if (!g_running.load(std::memory_order_relaxed))
            app.quit();
    });
    shutdownTimer.start(250);

    const int execResult = app.exec();
    driver.stop();

    return execResult;
}
