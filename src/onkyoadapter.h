#pragma once

#include "adapterinterface.h"

#include <QByteArray>
#include <QHash>
#include <QTimer>
class QTcpSocket;

namespace phicore {

class OnkyoAdapter : public AdapterInterface
{
    Q_OBJECT

public:
    explicit OnkyoAdapter(QObject *parent = nullptr);
    ~OnkyoAdapter() override;

protected:
    bool start(QString &errorString) override;
    void stop() override;
    void requestFullSync() override;
    void adapterConfigUpdated() override;
    void updateChannelState(const QString &deviceExternalId,
                            const QString &channelExternalId,
                            const QVariant &value,
                            CmdId cmdId) override;
    void invokeAdapterAction(const QString &actionId,
                             const QJsonObject &params,
                             CmdId cmdId) override;

private:
    void reloadInputLabelMap();
    Channel buildInputChannel() const;
    void setConnected(bool connected);
    void applyConfig();
    void updatePollInterval();
    bool canAttemptConnect() const;
    void markConnectAttempt();
    void logConnectFailure(const QString &error, const QString &host);
    void requestInitialState();
    bool sendIscpCommand(const QByteArray &command, bool parseResponse, int responseTimeoutMs);
    void processResponseData(const QByteArray &data);
    void handleIscpPayload(const QByteArray &payload);
    void emitDeviceSnapshot();
    QString resolveDeviceId() const;
    void emitChannelState(const QString &channelId, const QVariant &value);
    void markSeen();
    void startPresenceTimer();

    bool m_connected = false;
    bool m_synced = false;
    QString m_deviceId;
    quint16 m_controlPort = 0;
    int m_presenceTimeoutMs = 15000;
    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    qint64 m_lastConnectAttemptMs = 0;
    qint64 m_lastConnectLogMs = 0;
    QString m_lastConnectError;
    int m_volumeMaxRaw = 160;
    QHash<QString, QString> m_inputLabelMap;
    QString m_lastInputCode;
    bool m_stopping = false;
    qint64 m_lastSeenMs = 0;
    QTimer *m_presenceTimer = nullptr;
    QTimer *m_pollTimer = nullptr;
};

} // namespace phicore
