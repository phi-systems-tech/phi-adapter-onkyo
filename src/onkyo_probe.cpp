#include "onkyo_probe.h"

#include <utility>

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonValue>
#include <QTcpSocket>

#include "onkyo_protocol.h"
#include "onkyo_trace.h"

namespace phicore::onkyo::ipc {

QStringList resolveProbeHosts(const QJsonObject &params)
{
    QStringList ips;
    QStringList hosts;
    auto appendIfMissing = [&ips, &hosts](const QString &value) {
        const QString normalized = value.trimmed();
        if (normalized.isEmpty())
            return;
        QHostAddress addr;
        if (addr.setAddress(normalized)) {
            if (!ips.contains(normalized))
                ips.push_back(normalized);
            return;
        }
        if (!hosts.contains(normalized))
            hosts.push_back(normalized);
    };

    appendIfMissing(params.value(QStringLiteral("ip")).toString());
    const QJsonObject adapterObj = params.value(QStringLiteral("adapter")).toObject();
    if (!adapterObj.isEmpty())
        appendIfMissing(adapterObj.value(QStringLiteral("ip")).toString());
    appendIfMissing(params.value(QStringLiteral("host")).toString());
    if (!adapterObj.isEmpty())
        appendIfMissing(adapterObj.value(QStringLiteral("host")).toString());
    QStringList result = ips;
    for (const QString &host : std::as_const(hosts)) {
        if (!result.contains(host))
            result.push_back(host);
    }
    return result;
}

std::uint16_t resolveProbePort(const QJsonObject &params)
{
    auto parsePort = [](const QJsonValue &value) -> std::uint16_t {
        if (value.isDouble())
            return normalizedPort(value.toInt());
        if (value.isString()) {
            bool ok = false;
            const int parsed = value.toString().trimmed().toInt(&ok, 10);
            if (ok)
                return normalizedPort(parsed);
        }
        return 0;
    };

    return parsePort(params.value(QStringLiteral("iscpPort")));
}

bool probeEndpoint(const QString &host, std::uint16_t port, QString *errorMessage)
{
    QElapsedTimer timer;
    timer.start();
    timingLog(QStringLiteral("factory.probe.start host=%1 port=%2").arg(host).arg(port));
    trace(QStringLiteral("factory probe start host=%1 port=%2").arg(host).arg(port));
    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(900)) {
        timingLog(QStringLiteral("factory.probe.end status=failure host=%1 port=%2 elapsedMs=%3 error=%4")
                      .arg(host)
                      .arg(port)
                      .arg(timer.elapsed())
                      .arg(socket.errorString()));
        trace(QStringLiteral("factory probe failed host=%1 port=%2 elapsedMs=%3 error=%4")
                  .arg(host)
                  .arg(port)
                  .arg(timer.elapsed())
                  .arg(socket.errorString()));
        if (errorMessage)
            *errorMessage = socket.errorString();
        return false;
    }
    timingLog(QStringLiteral("factory.probe.end status=success host=%1 port=%2 elapsedMs=%3")
                  .arg(host)
                  .arg(port)
                  .arg(timer.elapsed()));
    trace(QStringLiteral("factory probe ok host=%1 port=%2 elapsedMs=%3")
              .arg(host)
              .arg(port)
              .arg(timer.elapsed()));
    socket.disconnectFromHost();
    return true;
}
} // namespace phicore::onkyo::ipc
