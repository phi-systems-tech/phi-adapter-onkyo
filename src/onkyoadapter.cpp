#include "onkyoadapter.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>
#include <QTcpSocket>
#include <QThread>
#include <QtGlobal>

namespace {
constexpr auto kChannelPower = "power";
constexpr auto kChannelVolume = "volume";
constexpr auto kChannelMute = "mute";
constexpr auto kChannelInput = "input";
constexpr auto kChannelConnectivity = "connectivity";
constexpr bool kUseEiscp = true;
constexpr bool kUseCrlf = false;

static QHash<QString, QString> buildInputLabelMap()
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


}

Q_LOGGING_CATEGORY(adapterLog, "phi-core.adapters.onkyo");

namespace phicore {

static QString inferModelFromIdentifier(const QString &raw)
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
    if (match.hasMatch()) {
        const QString model = match.captured(1).trimmed();
        if (!model.isEmpty() && model.contains(QRegularExpression(QStringLiteral("\\d"))))
            return model;
    }
    return {};
}

OnkyoAdapter::OnkyoAdapter(QObject *parent)
    : AdapterInterface(parent)
{
}

OnkyoAdapter::~OnkyoAdapter()
{
    qCDebug(adapterLog) << "OnkyoAdapter destroyed for" << adapter().id;
}

bool OnkyoAdapter::start(QString &errorString)
{
    constexpr int kInitialQueryDelayMs = 1500;
    m_stopping = false;
    applyConfig();

    qCInfo(adapterLog) << "Starting OnkyoAdapter for" << adapter().id
                       << "host" << adapter().ip.trimmed()
                       << "iscpPort" << m_controlPort
                       << "eISCP" << kUseEiscp
                       << "CRLF" << kUseCrlf
                       << "initialDelayMs" << kInitialQueryDelayMs
                       << "presenceTimeoutMs" << m_presenceTimeoutMs
                       << "pollIntervalMs" << m_pollIntervalMs
                       << "volumeMaxRaw" << m_volumeMaxRaw;

    if (adapter().ip.trimmed().isEmpty() || m_controlPort == 0) {
        qCWarning(adapterLog) << "OnkyoAdapter: IP not configured; staying disconnected";
    }
    errorString.clear();
    m_synced = false;
    startPresenceTimer();
    emitDeviceSnapshot();
    QTimer::singleShot(kInitialQueryDelayMs, this, [this]() { requestInitialState(); });
    if (m_pollIntervalMs > 0) {
        if (!m_pollTimer) {
            m_pollTimer = new QTimer(this);
            m_pollTimer->setInterval(m_pollIntervalMs);
            m_pollTimer->setSingleShot(false);
            connect(m_pollTimer, &QTimer::timeout, this, [this]() { requestInitialState(); });
            m_pollTimer->moveToThread(thread());
        } else {
            m_pollTimer->setInterval(m_pollIntervalMs);
        }
        updatePollInterval();
    }
    return true;
}

void OnkyoAdapter::stop()
{
    qCInfo(adapterLog) << "Stopping OnkyoAdapter for" << adapter().id;
    m_stopping = true;
    m_synced = false;
    if (m_presenceTimer)
        m_presenceTimer->stop();
    if (m_pollTimer)
        m_pollTimer->stop();
    setConnected(false);
}

void OnkyoAdapter::requestFullSync()
{
    if (m_synced)
        return;
    emitDeviceSnapshot();
    requestInitialState();
}

void OnkyoAdapter::adapterConfigUpdated()
{
    applyConfig();
    if (m_synced && !m_deviceId.isEmpty()) {
        emit channelUpdated(m_deviceId, buildInputChannel());
    } else {
        emitDeviceSnapshot();
    }
    requestInitialState();
}

void OnkyoAdapter::setConnected(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    updatePollInterval();
    emit connectionStateChanged(m_connected);
}

void OnkyoAdapter::updateChannelState(const QString &deviceExternalId,
                                      const QString &channelExternalId,
                                      const QVariant &value,
                                      CmdId cmdId)
{
    CmdResponse resp;
    resp.id = cmdId;
    resp.tsMs = QDateTime::currentMSecsSinceEpoch();

    if (deviceExternalId != m_deviceId) {
        resp.status = CmdStatus::NotSupported;
        resp.error = QStringLiteral("Unknown device");
        emit cmdResult(resp);
        return;
    }

    if (channelExternalId == QLatin1String(kChannelPower)) {
        const bool on = value.toBool();
        if (sendIscpCommand(on ? QByteArrayLiteral("PWR01") : QByteArrayLiteral("PWR00"), false, 0)) {
            resp.status = CmdStatus::Success;
            resp.finalValue = on;
        } else {
            resp.status = CmdStatus::TemporarilyOffline;
            resp.error = QStringLiteral("Receiver unavailable");
        }
        emit cmdResult(resp);
        return;
    }

    if (channelExternalId == QLatin1String(kChannelVolume)) {
        bool ok = false;
        const double requested = value.toDouble(&ok);
        if (!ok) {
            resp.status = CmdStatus::InvalidArgument;
            resp.error = QStringLiteral("Volume must be numeric");
            emit cmdResult(resp);
            return;
        }
        const double clampedPercent = qBound(0.0, requested, 100.0);
        const int rawValue = qBound(0,
                                    static_cast<int>(qRound((clampedPercent / 100.0) * m_volumeMaxRaw)),
                                    m_volumeMaxRaw);
        const QByteArray payload =
            QByteArrayLiteral("MVL") + QByteArray::number(rawValue, 16).rightJustified(2, '0').toUpper();
        if (sendIscpCommand(payload, false, 0)) {
            resp.status = CmdStatus::Success;
            resp.finalValue = clampedPercent;
        } else {
            resp.status = CmdStatus::TemporarilyOffline;
            resp.error = QStringLiteral("Receiver unavailable");
        }
        emit cmdResult(resp);
        return;
    }

    if (channelExternalId == QLatin1String(kChannelMute)) {
        const bool muted = value.toBool();
        if (sendIscpCommand(muted ? QByteArrayLiteral("AMT01") : QByteArrayLiteral("AMT00"), false, 0)) {
            resp.status = CmdStatus::Success;
            resp.finalValue = muted;
        } else {
            resp.status = CmdStatus::TemporarilyOffline;
            resp.error = QStringLiteral("Receiver unavailable");
        }
        emit cmdResult(resp);
        return;
    }

    if (channelExternalId == QLatin1String(kChannelInput)) {
        QString input = value.toString().trimmed();
        if (input.startsWith(QLatin1String("SLI")))
            input = input.mid(3);
        const QString labelMatch = input.toLower();
        for (auto it = m_inputLabelMap.constBegin(); it != m_inputLabelMap.constEnd(); ++it) {
            if (it.value().toLower() == labelMatch) {
                input = it.key();
                break;
            }
        }
        if (input.length() != 2) {
            resp.status = CmdStatus::InvalidArgument;
            resp.error = QStringLiteral("Input expects 2-digit code (e.g. 01)");
            emit cmdResult(resp);
            return;
        }
        if (sendIscpCommand(QByteArrayLiteral("SLI") + input.toLatin1(), false, 0)) {
            resp.status = CmdStatus::Success;
            resp.finalValue = input;
        } else {
            resp.status = CmdStatus::TemporarilyOffline;
            resp.error = QStringLiteral("Receiver unavailable");
        }
        emit cmdResult(resp);
        return;
    }

    resp.status = CmdStatus::NotSupported;
    resp.error = QStringLiteral("Channel not supported");
    emit cmdResult(resp);
}

void OnkyoAdapter::invokeAdapterAction(const QString &actionId,
                                       const QJsonObject &params,
                                       CmdId cmdId)
{
    if (actionId == QLatin1String("settings")) {
        AdapterInterface::invokeAdapterAction(actionId, params, cmdId);
        return;
    }
    Q_UNUSED(params);
    if (cmdId == 0)
        return;
    ActionResponse resp;
    resp.id = cmdId;
    resp.tsMs = QDateTime::currentMSecsSinceEpoch();

    if (actionId == QLatin1String("probeCurrentInput")) {
        const QString before = m_lastInputCode;
        QString resolvedCode;
        if (!sendIscpCommand(QByteArrayLiteral("SLIQSTN"), true, 1500)) {
            resp.status = CmdStatus::TemporarilyOffline;
            resp.error = QStringLiteral("Receiver unavailable");
        } else if (!m_lastInputCode.isEmpty()) {
            resp.status = CmdStatus::Success;
            resp.resultType = ActionResultType::String;
            resp.resultValue = m_lastInputCode;
            resolvedCode = m_lastInputCode;
        } else if (!before.isEmpty()) {
            resp.status = CmdStatus::Success;
            resp.resultType = ActionResultType::String;
            resp.resultValue = before;
            resolvedCode = before;
        } else {
            resp.status = CmdStatus::Failure;
            resp.error = QStringLiteral("No input reported");
        }
        if (!resolvedCode.isEmpty()) {
            QString normalized = resolvedCode.trimmed();
            if (normalized.size() == 1 && normalized[0].isDigit())
                normalized.prepend(QLatin1Char('0'));
            QSet<QString> activeCodes;
            const QJsonValue activeValue = adapter().meta.value(QStringLiteral("activeSliCodes"));
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
            const QString existingLabel = adapter().meta.value(labelKey).toString().trimmed();
            if (existingLabel.isEmpty())
                patch.insert(labelKey, QStringLiteral("SLI %1").arg(normalized));
            emit adapterMetaUpdated(patch);
        }
        if (!before.isEmpty())
            m_lastInputCode = before;
        emit actionResult(resp);
        return;
    }

    resp.status = CmdStatus::NotSupported;
    resp.error = QStringLiteral("Adapter action not supported");
    emit actionResult(resp);
}

void OnkyoAdapter::requestInitialState()
{
    if (m_stopping || QThread::currentThread()->isInterruptionRequested())
        return;
    if (adapter().ip.trimmed().isEmpty() || m_controlPort == 0)
        return;
    const QList<QByteArray> commands = {
        QByteArrayLiteral("PWRQSTN"),
        QByteArrayLiteral("AMTQSTN"),
        QByteArrayLiteral("MVLQSTN"),
        QByteArrayLiteral("SLIQSTN"),
    };
    for (const QByteArray &cmd : commands) {
        if (m_stopping || QThread::currentThread()->isInterruptionRequested())
            return;
        sendIscpCommand(cmd, true, 800);
    }
}

void OnkyoAdapter::applyConfig()
{
    const int portValue = adapter().port > 0
        ? static_cast<int>(adapter().port)
        : 0;
    if (portValue > 0) {
        m_controlPort = static_cast<quint16>(portValue);
    } else {
        m_controlPort = 60128;
    }
    m_pollIntervalMs = qBound(500,
                              adapter().meta.value(QStringLiteral("pollIntervalMs")).toInt(5000),
                              300000);
    m_retryIntervalMs = qBound(1000,
                               adapter().meta.value(QStringLiteral("retryIntervalMs")).toInt(10000),
                               300000);
    m_presenceTimeoutMs = m_pollIntervalMs + 1000;
    m_volumeMaxRaw = qBound(1,
                            adapter().meta.value(QStringLiteral("volumeMaxRaw")).toInt(160),
                            500);
    reloadInputLabelMap();
    updatePollInterval();
}

void OnkyoAdapter::updatePollInterval()
{
    if (!m_pollTimer)
        return;
    const int interval = m_connected ? m_pollIntervalMs : m_retryIntervalMs;
    if (m_pollTimer->interval() != interval)
        m_pollTimer->setInterval(interval);
    if (!m_pollTimer->isActive())
        m_pollTimer->start();
}

bool OnkyoAdapter::sendIscpCommand(const QByteArray &command, bool parseResponse, int responseTimeoutMs)
{
    const int connectTimeoutMs = 1500;
    const QString currentHost = adapter().ip.trimmed();
    if (currentHost.isEmpty() || m_controlPort == 0)
        return false;
    if (m_stopping || QThread::currentThread()->isInterruptionRequested())
        return false;
    if (!m_connected && !canAttemptConnect())
        return false;

    auto shouldAbort = [this]() {
        return m_stopping || QThread::currentThread()->isInterruptionRequested();
    };

    QTcpSocket socket;
    socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    markConnectAttempt();
    for (int i = 0; i < 1; ++i) {
        const QString &host = currentHost;
        socket.abort();
        socket.connectToHost(host, m_controlPort);
        int waitedMs = 0;
        while (!socket.waitForConnected(100)) {
            waitedMs += 100;
            if (shouldAbort() || waitedMs >= connectTimeoutMs) {
                if (socket.state() != QAbstractSocket::ConnectedState) {
                    logConnectFailure(socket.errorString(), host);
                    break;
                }
                break;
            }
        }
        if (socket.state() == QAbstractSocket::ConnectedState) {
            goto connected;
        }
    }
    if (socket.state() != QAbstractSocket::ConnectedState) {
        // Do not flip connection state on a single failed attempt; presence timer handles disconnects.
        return false;
    }
connected:
    markSeen();

    const QByteArray terminator = kUseCrlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\r");
    QByteArray payload = QByteArrayLiteral("!1") + command + terminator;
    if (!kUseEiscp) {
        socket.write(payload);
        socket.flush();
        if (parseResponse && responseTimeoutMs > 0 && socket.waitForReadyRead(responseTimeoutMs)) {
            const QByteArray data = socket.readAll();
            if (!data.isEmpty())
                processResponseData(data);
        }
        socket.disconnectFromHost();
        if (socket.state() != QAbstractSocket::UnconnectedState) {
            int disconnectWaitedMs = 0;
            while (socket.state() != QAbstractSocket::UnconnectedState && disconnectWaitedMs < 300) {
                if (shouldAbort())
                    break;
                socket.waitForDisconnected(50);
                disconnectWaitedMs += 50;
            }
        }
        return true;
    }

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
    socket.write(frame);
    socket.flush();
    if (parseResponse && responseTimeoutMs > 0) {
        QByteArray data;
        int readWaitedMs = 0;
        while (readWaitedMs < responseTimeoutMs) {
            if (shouldAbort())
                break;
            if (socket.waitForReadyRead(100)) {
                data.append(socket.readAll());
                while (socket.waitForReadyRead(50)) {
                    data.append(socket.readAll());
                }
                break;
            }
            readWaitedMs += 100;
        }
        if (!data.isEmpty())
            processResponseData(data);
    }
    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        int disconnectWaitedMs = 0;
        while (socket.state() != QAbstractSocket::UnconnectedState && disconnectWaitedMs < 300) {
            if (shouldAbort())
                break;
            socket.waitForDisconnected(50);
            disconnectWaitedMs += 50;
        }
    }
    return true;
}


bool OnkyoAdapter::canAttemptConnect() const
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    return (nowMs - m_lastConnectAttemptMs) >= m_retryIntervalMs;
}

void OnkyoAdapter::markConnectAttempt()
{
    m_lastConnectAttemptMs = QDateTime::currentMSecsSinceEpoch();
}

void OnkyoAdapter::logConnectFailure(const QString &error, const QString &host)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString msg = QStringLiteral("%1|%2").arg(error, host);
    if (msg == m_lastConnectError && (nowMs - m_lastConnectLogMs) < m_retryIntervalMs)
        return;
    m_lastConnectError = msg;
    m_lastConnectLogMs = nowMs;
    qCWarning(adapterLog)
        << "Onkyo connect failed:" << error
        << "host" << host << "port" << m_controlPort << "ip" << adapter().ip;
}


void OnkyoAdapter::processResponseData(const QByteArray &data)
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
        if (headerIndex < 0)
            return;
        if (headerIndex + 16 > data.size())
            return;
        const unsigned char *header = reinterpret_cast<const unsigned char *>(data.constData() + headerIndex);
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

void OnkyoAdapter::handleIscpPayload(const QByteArray &payload)
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
            qCInfo(adapterLog).noquote() << "Onkyo parsed PWR:" << QString::fromLatin1(value);
            if (value == "01" || value == "00") {
                const bool on = value == "01";
                emitChannelState(QString::fromLatin1(kChannelPower), on);
                markSeen();
            }
            continue;
        }
        if (line.startsWith("AMT")) {
            const QByteArray value = line.mid(3);
            if (value == "01" || value == "00") {
                const bool muted = value == "01";
                emitChannelState(QString::fromLatin1(kChannelMute), muted);
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
            const QByteArray code = line.mid(3);
            m_lastInputCode = QString::fromLatin1(code);
            emitChannelState(QString::fromLatin1(kChannelInput), QString::fromLatin1(code));
            markSeen();
            continue;
        }
    }
}

void OnkyoAdapter::emitDeviceSnapshot()
{
    if (m_synced)
        return;

    m_deviceId = resolveDeviceId();

    Device device;
    device.id = m_deviceId;
    device.deviceClass = DeviceClass::MediaPlayer;
    const QString adapterName = adapter().name.trimmed();
    const QString metaName = adapter().meta.value(QStringLiteral("deviceName")).toString().trimmed();
    device.name = !adapterName.isEmpty()
        ? adapterName
        : (!metaName.isEmpty() ? metaName : adapter().ip.trimmed());
    device.manufacturer = adapter().meta.value(QStringLiteral("manufacturer")).toString().trimmed();
    if (device.manufacturer.isEmpty())
        device.manufacturer = QStringLiteral("Onkyo & Pioneer");
    device.model = adapter().meta.value(QStringLiteral("model")).toString().trimmed();
    if (device.model.isEmpty()) {
        const QStringList candidates = {
            adapter().ip.trimmed(),
            adapter().meta.value(QStringLiteral("deviceUuid")).toString(),
            adapter().meta.value(QStringLiteral("uuid")).toString(),
            adapter().meta.value(QStringLiteral("deviceName")).toString(),
            adapter().name,
        };
        for (const QString &candidate : candidates) {
            device.model = inferModelFromIdentifier(candidate);
            if (!device.model.isEmpty())
                break;
        }
    }

    QJsonObject meta;
    const bool spotify = adapter().meta.value(QStringLiteral("supportsSpotify")).toBool();
    const bool transcoder = adapter().meta.value(QStringLiteral("supportsTranscoder")).toBool();
    if (spotify)
        meta.insert(QStringLiteral("supportsSpotify"), true);
    if (transcoder)
        meta.insert(QStringLiteral("supportsTranscoder"), true);
    device.meta = meta;

    ChannelList channels;
    Channel power;
    power.id = QString::fromLatin1(kChannelPower);
    power.name = QStringLiteral("Power");
    power.kind = ChannelKind::PowerOnOff;
    power.dataType = ChannelDataType::Bool;
    power.flags = ChannelFlagDefaultWrite;
    channels.push_back(power);

    Channel volume;
    volume.id = QString::fromLatin1(kChannelVolume);
    volume.name = QStringLiteral("Volume");
    volume.kind = ChannelKind::Volume;
    volume.dataType = ChannelDataType::Float;
    volume.flags = ChannelFlagDefaultWrite;
    volume.minValue = 0.0;
    volume.maxValue = 100.0;
    volume.stepValue = 1.0;
    channels.push_back(volume);

    Channel mute;
    mute.id = QString::fromLatin1(kChannelMute);
    mute.name = QStringLiteral("Mute");
    mute.kind = ChannelKind::Mute;
    mute.dataType = ChannelDataType::Bool;
    mute.flags = ChannelFlagDefaultWrite;
    channels.push_back(mute);

    channels.push_back(buildInputChannel());

    Channel connectivity;
    connectivity.id = QString::fromLatin1(kChannelConnectivity);
    connectivity.name = QStringLiteral("Connectivity");
    connectivity.kind = ChannelKind::ConnectivityStatus;
    connectivity.dataType = ChannelDataType::Enum;
    connectivity.flags = ChannelFlagDefaultRead;
    channels.push_back(connectivity);

    emit deviceUpdated(device, channels);
    emit fullSyncCompleted();
    m_synced = true;
}

QString OnkyoAdapter::resolveDeviceId() const
{
    const QString uuid = adapter().meta.value(QStringLiteral("deviceUuid")).toString().trimmed();
    if (!uuid.isEmpty())
        return uuid;
    const QString legacyUuid = adapter().meta.value(QStringLiteral("uuid")).toString().trimmed();
    if (!legacyUuid.isEmpty())
        return legacyUuid;
    if (!adapter().id.isEmpty())
        return adapter().id;
    const QString host = adapter().host.trimmed();
    if (!host.isEmpty())
        return host;
    const QString ip = adapter().ip.trimmed();
    if (!ip.isEmpty())
        return ip;
    return QStringLiteral("onkyo-pioneer");
}

void OnkyoAdapter::emitChannelState(const QString &channelId, const QVariant &value)
{
    const qint64 tsMs = QDateTime::currentMSecsSinceEpoch();
    emit channelStateUpdated(m_deviceId, channelId, value, tsMs);
}

void OnkyoAdapter::markSeen()
{
    m_lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_connected)
        setConnected(true);
    emitChannelState(QString::fromLatin1(kChannelConnectivity),
                     static_cast<int>(ConnectivityStatus::Connected));
}

void OnkyoAdapter::startPresenceTimer()
{
    if (!m_presenceTimer) {
        m_presenceTimer = new QTimer(this);
        m_presenceTimer->setInterval(2000);
        m_presenceTimer->setSingleShot(false);
        connect(m_presenceTimer, &QTimer::timeout, this, [this]() {
            if (m_lastSeenMs <= 0)
                return;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastSeenMs > m_presenceTimeoutMs) {
                setConnected(false);
                emitChannelState(QString::fromLatin1(kChannelConnectivity),
                                 static_cast<int>(ConnectivityStatus::Disconnected));
            }
        });
        m_presenceTimer->moveToThread(thread());
    }
    if (!m_presenceTimer->isActive())
        m_presenceTimer->start();
}

void OnkyoAdapter::reloadInputLabelMap()
{
    m_inputLabelMap = buildInputLabelMap();
    QSet<QString> activeCodeSet;
    const QJsonValue activeCodesValue = adapter().meta.value(QStringLiteral("activeSliCodes"));
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
    const QJsonObject meta = adapter().meta;
    for (auto it = meta.begin(); it != meta.end(); ++it) {
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

Channel OnkyoAdapter::buildInputChannel() const
{
    Channel input;
    input.id = QString::fromLatin1(kChannelInput);
    input.name = QStringLiteral("Input");
    input.kind = ChannelKind::HdmiInput;
    input.dataType = ChannelDataType::String;
    input.flags = ChannelFlagDefaultWrite;
    if (!m_inputLabelMap.isEmpty()) {
        QMap<QString, QString> sorted;
        for (auto it = m_inputLabelMap.constBegin(); it != m_inputLabelMap.constEnd(); ++it) {
            const QString label = it.value().trimmed();
            if (!label.isEmpty())
                sorted.insert(it.key(), label);
        }
        if (!sorted.isEmpty()) {
            for (auto it = sorted.constBegin(); it != sorted.constEnd(); ++it) {
                AdapterConfigOption entry;
                entry.label = it.value().isEmpty()
                    ? QStringLiteral("SLI %1").arg(it.key())
                    : it.value();
                entry.value = it.key();
                input.choices.push_back(entry);
            }
        }
    }
    return input;
}

} // namespace phicore
