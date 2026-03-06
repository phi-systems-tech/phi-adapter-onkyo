#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QtGlobal>

#include "phi/adapter/sdk/sidecar.h"
#include "phi/adapter/sdk/qt/instance_execution_backend_qt.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

namespace {

constexpr const char kPluginType[] = "onkyo";
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
constexpr bool kTraceEnabled = false;
constexpr bool kTimingLogsEnabled = true;
constexpr int kPollQueryTimeoutMs = 500;
constexpr int kConnectFailuresBeforeDisconnect = 3;

std::atomic_bool g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

std::int64_t nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void trace(const QString &message)
{
    if (!kTraceEnabled)
        return;
    std::cerr << "[" << nowMs() << "] onkyo-ipc: " << message.toStdString() << '\n';
}

void timingLog(const QString &message)
{
    if (!kTimingLogsEnabled)
        return;
    std::cerr << "[" << nowMs() << "] onkyo-ipc[timing]: " << message.toStdString() << '\n';
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

QString normalizeSliCode(QString raw);

QHash<QString, QString> loadConfiguredSliLabels(const QJsonObject &staticConfig)
{
    QHash<QString, QString> map;
    QJsonObject labels = staticConfig.value(QStringLiteral("sliLabels")).toObject();
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        const QString code = normalizeSliCode(it.key());
        if (code.isEmpty())
            continue;
        const QString label = it.value().toString().trimmed();
        if (label.isEmpty())
            continue;
        map.insert(code, label);
    }
    return map;
}

QString normalizeSliCode(QString raw)
{
    QString code = raw.trimmed().toUpper();
    if (code.startsWith(QLatin1String("SLI"), Qt::CaseInsensitive))
        code = code.mid(3).trimmed();
    code.remove(QLatin1Char(' '));
    if (code.isEmpty())
        return {};

    bool numeric = false;
    const int parsed = code.toInt(&numeric, 10);
    if (numeric) {
        code = QString::number(parsed);
        if (code.size() == 1)
            code.prepend(QLatin1Char('0'));
        return code;
    }

    if (code.size() == 1)
        code.prepend(QLatin1Char('0'));
    return code;
}

QString formatSliDisplayLabel(const QString &code, const QString &mappedLabel)
{
    const QString normalizedCode = normalizeSliCode(code);
    const QString label = mappedLabel.trimmed();
    if (!label.isEmpty())
        return label;
    if (normalizedCode.isEmpty())
        return QString();
    return QStringLiteral("SLI %1").arg(normalizedCode);
}

QJsonArray normalizeActiveSliCodesArray(const QJsonValue &value)
{
    QJsonArray normalized;
    QSet<QString> seen;
    auto appendCode = [&normalized, &seen](const QString &rawCode) {
        const QString code = normalizeSliCode(rawCode);
        if (code.isEmpty() || seen.contains(code))
            return;
        seen.insert(code);
        normalized.append(code);
    };

    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &entry : arr) {
            if (entry.isString()) {
                appendCode(entry.toString());
                continue;
            }
            if (entry.isDouble()) {
                appendCode(QString::number(entry.toInt()));
                continue;
            }
        }
        return normalized;
    }

    if (value.isString())
        appendCode(value.toString());
    else if (value.isDouble())
        appendCode(QString::number(value.toInt()));

    return normalized;
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
    return adapterPort == 0 ? static_cast<std::uint16_t>(60128) : adapterPort;
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

QString scalarToDebugString(const v1::ScalarValue &value)
{
    return std::visit(
        [](const auto &entry) -> QString {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return QStringLiteral("null");
            } else if constexpr (std::is_same_v<T, bool>) {
                return entry ? QStringLiteral("true") : QStringLiteral("false");
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return QString::number(entry);
            } else if constexpr (std::is_same_v<T, double>) {
                return QString::number(entry, 'f', 6);
            } else {
                return QString::fromStdString(entry);
            }
        },
        value);
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

QString scalarToQString(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<v1::Utf8String>(&value))
        return QString::fromStdString(*v);
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return QString::number(*v);
    if (const auto *v = std::get_if<double>(&value))
        return QString::number(*v, 'g', 12);
    if (const auto *v = std::get_if<bool>(&value))
        return *v ? QStringLiteral("1") : QStringLiteral("0");
    return {};
}

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

QJsonArray optionsToChoiceJson(const v1::AdapterConfigOptionList &options)
{
    QJsonArray out;
    for (const v1::AdapterConfigOption &option : options) {
        QJsonObject choice;
        choice.insert(QStringLiteral("value"), QString::fromStdString(option.value));
        choice.insert(QStringLiteral("label"), QString::fromStdString(option.label));
        out.append(choice);
    }
    return out;
}

QJsonArray schemaInputChoices(const QHash<QString, QString> &labels)
{
    QJsonArray choices;
    for (int code = 0; code <= 0xFF; ++code) {
        const QString key = QStringLiteral("%1").arg(code, 2, 16, QLatin1Char('0')).toUpper();
        const QString label = formatSliDisplayLabel(key, labels.value(key));
        QJsonObject option;
        option.insert(QStringLiteral("value"), key);
        option.insert(QStringLiteral("label"), label);
        choices.append(option);
    }
    return choices;
}

QJsonObject buildConfigSchemaObject(const QHash<QString, QString> &labels)
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
                    QJsonObject meta = QJsonObject(),
                    QJsonObject layout = QJsonObject()) {
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
        if (!layout.isEmpty())
            obj.insert(QStringLiteral("layout"), layout);
        return obj;
    };
    auto action = [](QString id,
                     QString label,
                     QString description,
                     bool hasForm,
                     QJsonObject meta = QJsonObject()) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), id);
        obj.insert(QStringLiteral("label"), label);
        obj.insert(QStringLiteral("description"), description);
        obj.insert(QStringLiteral("hasForm"), hasForm);
        if (!meta.isEmpty())
            obj.insert(QStringLiteral("meta"), meta);
        return obj;
    };

    QJsonArray factoryFields;
    factoryFields.append(field(QStringLiteral("host"), QStringLiteral("Hostname"), QStringLiteral("Host")));
    factoryFields.append(field(QStringLiteral("iscpPort"),
                               QStringLiteral("Port"),
                               QStringLiteral("ISCP Port"),
                               60128));
    factoryFields.append(field(QStringLiteral("pollIntervalMs"),
                               QStringLiteral("Integer"),
                               QStringLiteral("Poll interval"),
                               5000));
    factoryFields.append(field(QStringLiteral("retryIntervalMs"),
                               QStringLiteral("Integer"),
                               QStringLiteral("Retry interval"),
                               10000));

    const QJsonArray inputChoices = schemaInputChoices(labels);

    QJsonArray instanceFields;
    instanceFields.append(field(QStringLiteral("volumeMaxRaw"),
                                QStringLiteral("Integer"),
                                QStringLiteral("Max volume raw"),
                                160,
                                QString(),
                                QString(),
                                QStringLiteral("settings"),
                                QJsonArray{QStringLiteral("InstanceOnly")}));
    instanceFields.append(field(QStringLiteral("activeSliCodes"),
                                QStringLiteral("Select"),
                                QStringLiteral("Active SLI codes"),
                                QJsonArray(),
                                QString(),
                                QString(),
                                QStringLiteral("settings"),
                                QJsonArray{QStringLiteral("Multi"), QStringLiteral("InstanceOnly")},
                                inputChoices,
                                QJsonObject{{QStringLiteral("reloadActionLayoutOnChange"), true}}));
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
                                QJsonObject{
                                    {QStringLiteral("appendTo"), QStringLiteral("activeSliCodes")},
                                },
                                QJsonObject{{QStringLiteral("actionPosition"), QStringLiteral("inline")}}));

    for (const QJsonValue &choiceValue : inputChoices) {
        if (!choiceValue.isObject())
            continue;
        const QJsonObject choiceObj = choiceValue.toObject();
        const QString key = normalizeSliCode(choiceObj.value(QStringLiteral("value")).toString());
        if (key.isEmpty())
            continue;
        QJsonObject mappingField = field(QStringLiteral("inputLabel_%1").arg(key),
                                         QStringLiteral("String"),
                                         QStringLiteral("SLI %1 label").arg(key),
                                         labels.value(key),
                                         QString(),
                                         QString(),
                                         QStringLiteral("settings"),
                                         QJsonArray{QStringLiteral("InstanceOnly")});
        mappingField.insert(QStringLiteral("visibility"),
                            QJsonObject{
                                {QStringLiteral("fieldKey"), QStringLiteral("activeSliCodes")},
                                {QStringLiteral("op"), QStringLiteral("contains")},
                                {QStringLiteral("value"), key},
                            });
        instanceFields.append(mappingField);
    }

    QJsonObject factorySection;
    factorySection.insert(QStringLiteral("title"), QStringLiteral("Connection"));
    factorySection.insert(QStringLiteral("fields"), factoryFields);
    factorySection.insert(QStringLiteral("actions"),
                          QJsonArray{
                              action(QStringLiteral("probe"),
                                     QStringLiteral("Test connection"),
                                     QStringLiteral("Validate receiver reachability using the current config values."),
                                     false,
                                     QJsonObject{
                                         {QStringLiteral("placement"), QStringLiteral("card")},
                                         {QStringLiteral("kind"), QStringLiteral("command")},
                                         {QStringLiteral("requiresAck"), true},
                                     }),
                          });

    QJsonObject instanceSection;
    instanceSection.insert(QStringLiteral("title"), QStringLiteral("Settings"));
    instanceSection.insert(QStringLiteral("fields"), instanceFields);
    instanceSection.insert(QStringLiteral("actions"),
                           QJsonArray{
                               action(QStringLiteral("settings"),
                                      QStringLiteral("Settings"),
                                      QStringLiteral("Update adapter settings."),
                                      true,
                                      QJsonObject{
                                          {QStringLiteral("placement"), QStringLiteral("card")},
                                          {QStringLiteral("kind"), QStringLiteral("open_dialog")},
                                          {QStringLiteral("requiresAck"), true},
                                      }),
                               action(QStringLiteral("probeCurrentInput"),
                                      QStringLiteral("Probe current"),
                                      QStringLiteral("Probe current input and refresh labels."),
                                      false,
                                      QJsonObject{
                                          {QStringLiteral("placement"), QStringLiteral("form_field")},
                                          {QStringLiteral("kind"), QStringLiteral("command")},
                                          {QStringLiteral("requiresAck"), true},
                                      }),
                           });

    QJsonObject schema;
    schema.insert(QStringLiteral("factory"), factorySection);
    schema.insert(QStringLiteral("instance"), instanceSection);
    return schema;
}

class OnkyoIpcInstance final : public sdk::AdapterInstance
{
public:
    explicit OnkyoIpcInstance(QHash<QString, QString> bootstrapInputLabels)
        : m_defaultInputLabelMap(std::move(bootstrapInputLabels))
    {
    }

    ~OnkyoIpcInstance() override
    {
        stopPollingTimer();
    }

protected:
    bool start() override
    {
        constexpr int kInitialQueryDelayMs = 1500;
        m_operationQueue.clear();
        m_operationRunning = false;
        m_queuePumpScheduled = false;
        m_pollQueued = false;
        m_pollRunning = false;
        m_started = true;
        m_stopping = false;
        m_synced = false;
        m_lastConnectLogMs = 0;
        m_lastConnectError.clear();
        m_lastReportedPower.reset();
        m_lastReportedMute.reset();
        m_lastReportedVolume.reset();
        m_lastReportedInput.clear();
        m_hasLastReportedInput = false;
        m_powerState = PowerState::Unknown;
        m_consecutiveConnectFailures = 0;
        setConnected(false);
        QTimer::singleShot(kInitialQueryDelayMs, [this]() {
            enqueuePollOperation(true);
        });
        startPollingTimer();
        return true;
    }

    void stop() override
    {
        m_stopping = true;
        m_started = false;
        m_synced = false;
        m_lastReportedPower.reset();
        m_lastReportedMute.reset();
        m_lastReportedVolume.reset();
        m_lastReportedInput.clear();
        m_hasLastReportedInput = false;
        m_powerState = PowerState::Unknown;
        m_consecutiveConnectFailures = 0;
        m_pollRunning = false;
        flushPendingOperations("Instance stopped");
        setConnected(false);
        stopPollingTimer();
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        const QStringList previousHosts = effectiveHosts();
        const std::uint16_t previousPort = m_controlPort;
        const bool wasConnected = m_connected;

        m_info = request.adapter;
        m_meta = parseJsonObject(request.adapter.metaJson);

        QJsonObject normalizedPatch;
        const QJsonValue rawActiveCodes = m_meta.value(QStringLiteral("activeSliCodes"));
        const QJsonArray normalizedCodes = normalizeActiveSliCodesArray(rawActiveCodes);
        const bool hadRawActiveCodes = !rawActiveCodes.isUndefined() && !rawActiveCodes.isNull();
        if (hadRawActiveCodes) {
            if (!rawActiveCodes.isArray() || normalizedCodes != rawActiveCodes.toArray()) {
                m_meta.insert(QStringLiteral("activeSliCodes"), normalizedCodes);
                normalizedPatch.insert(QStringLiteral("activeSliCodes"), normalizedCodes);
            }
        }

        if (!normalizedPatch.isEmpty()) {
            m_info.metaJson = toJson(m_meta);
            v1::Utf8String err;
            if (!sendAdapterMetaUpdated(toJson(normalizedPatch), &err))
                std::cerr << "failed to send adapterMetaUpdated(config.normalize): " << err << '\n';
        }

        applyConfig();
        const QStringList nextHosts = effectiveHosts();
        const bool endpointChanged = (previousPort != m_controlPort) || (previousHosts != nextHosts);
        m_synced = false;
        m_deviceId = resolveDeviceId();
        m_lastConnectLogMs = 0;
        m_lastConnectError.clear();
        m_lastReportedPower.reset();
        m_lastReportedMute.reset();
        m_lastReportedVolume.reset();
        m_lastReportedInput.clear();
        m_hasLastReportedInput = false;
        m_powerState = PowerState::Unknown;
        m_consecutiveConnectFailures = 0;
        m_stopping = false;
        m_started = true;
        m_pollRunning = false;
        flushPendingOperations("Config changed");

        if (endpointChanged || !wasConnected)
            setConnected(false);
        emitDeviceSnapshot();
        enqueuePollOperation(true);
        startPollingTimer();

        std::cerr << "onkyo-ipc config.changed adapterId=" << request.adapterId
                  << " externalId=" << request.adapter.externalId
                  << " pluginType=" << request.adapter.pluginType << '\n';
    }

    void onDisconnected() override
    {
        m_stopping = true;
        m_started = false;
        m_synced = false;
        m_lastReportedPower.reset();
        m_lastReportedMute.reset();
        m_lastReportedVolume.reset();
        m_lastReportedInput.clear();
        m_hasLastReportedInput = false;
        m_powerState = PowerState::Unknown;
        m_consecutiveConnectFailures = 0;
        m_pollRunning = false;
        flushPendingOperations("Instance disconnected");
        setConnected(false);
        stopPollingTimer();
    }

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        timingLog(QStringLiteral("cmd.recv type=channel.invoke cmdId=%1 externalId=%2 device=%3 channel=%4 value=%5")
                      .arg(request.cmdId)
                      .arg(QString::fromStdString(request.externalId))
                      .arg(QString::fromStdString(request.deviceExternalId))
                      .arg(QString::fromStdString(request.channelExternalId))
                      .arg(scalarToDebugString(request.value)));
        enqueueChannelInvokeOperation(request);
    }

    void onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        timingLog(QStringLiteral("cmd.recv type=adapter.action.invoke cmdId=%1 externalId=%2 action=%3")
                      .arg(request.cmdId)
                      .arg(QString::fromStdString(request.externalId))
                      .arg(QString::fromStdString(request.actionId)));
        if (request.actionId == "probeCurrentInput") {
            enqueueProbeCurrentOperation(request);
            return;
        }
        submitActionResult(handleAdapterActionInvoke(request), "adapter.action.invoke");
    }

    void onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        submitCmdResult(handleDeviceNameUpdate(request), "device.name.update");
    }

    void onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        submitCmdResult(handleDeviceEffectInvoke(request), "device.effect.invoke");
    }

    void onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        submitCmdResult(handleSceneInvoke(request), "scene.invoke");
    }

private:
    enum class PowerState {
        Unknown,
        Off,
        On,
    };

    struct PendingOperation
    {
        enum class Kind {
            Poll,
            ChannelInvoke,
            ProbeCurrentInput,
        };

        Kind kind = Kind::Poll;
        bool pollWasRunning = false;
        std::int64_t enqueuedMs = 0;
        sdk::ChannelInvokeRequest channelRequest;
        sdk::AdapterActionInvokeRequest actionRequest;
    };

    void removeQueuedPollOperations()
    {
        for (auto it = m_operationQueue.begin(); it != m_operationQueue.end();) {
            if (it->kind == PendingOperation::Kind::Poll) {
                it = m_operationQueue.erase(it);
                continue;
            }
            ++it;
        }
        m_pollQueued = false;
    }

    void enqueueChannelInvokeOperation(const sdk::ChannelInvokeRequest &request)
    {
        // Command writes should not wait behind stale queued poll work.
        removeQueuedPollOperations();

        if (request.channelExternalId == kChannelVolume) {
            for (auto it = m_operationQueue.begin(); it != m_operationQueue.end();) {
                if (it->kind == PendingOperation::Kind::ChannelInvoke
                    && it->channelRequest.deviceExternalId == request.deviceExternalId
                    && it->channelRequest.channelExternalId == kChannelVolume) {
                    v1::CmdResponse coalesced;
                    coalesced.id = it->channelRequest.cmdId;
                    coalesced.tsMs = nowMs();
                    coalesced.status = v1::CmdStatus::Success;
                    submitCmdResult(std::move(coalesced), "channel.invoke.coalesced");
                    it = m_operationQueue.erase(it);
                    continue;
                }
                ++it;
            }
        }

        PendingOperation op;
        op.kind = PendingOperation::Kind::ChannelInvoke;
        op.enqueuedMs = nowMs();
        op.channelRequest = request;
        m_operationQueue.push_back(std::move(op));
        timingLog(QStringLiteral("cmd.queue type=channel.invoke cmdId=%1 queueSize=%2")
                      .arg(request.cmdId)
                      .arg(static_cast<int>(m_operationQueue.size())));
        scheduleQueuePump();
    }

    void enqueueProbeCurrentOperation(const sdk::AdapterActionInvokeRequest &request)
    {
        PendingOperation op;
        op.kind = PendingOperation::Kind::ProbeCurrentInput;
        op.pollWasRunning = m_pollRunning;
        op.enqueuedMs = nowMs();
        op.actionRequest = request;
        m_operationQueue.push_front(std::move(op));
        timingLog(QStringLiteral("cmd.queue type=adapter.action.invoke cmdId=%1 action=%2 queueSize=%3")
                      .arg(request.cmdId)
                      .arg(QString::fromStdString(request.actionId))
                      .arg(static_cast<int>(m_operationQueue.size())));
        scheduleQueuePump();
    }

    void enqueuePollOperation(bool prioritize)
    {
        if (!m_started || m_stopping)
            return;

        // Keep exactly one queued poll operation at any time.
        removeQueuedPollOperations();

        PendingOperation op;
        op.kind = PendingOperation::Kind::Poll;
        op.enqueuedMs = nowMs();
        if (prioritize)
            m_operationQueue.push_front(std::move(op));
        else
            m_operationQueue.push_back(std::move(op));
        m_pollQueued = true;
        scheduleQueuePump();
    }

    void scheduleQueuePump()
    {
        if (m_queuePumpScheduled)
            return;
        m_queuePumpScheduled = true;
        QTimer::singleShot(0, [this]() {
            m_queuePumpScheduled = false;
            pumpQueue();
        });
    }

    void pumpQueue()
    {
        if (m_operationRunning || m_stopping)
            return;
        if (m_operationQueue.empty())
            return;

        m_operationRunning = true;
        PendingOperation op = std::move(m_operationQueue.front());
        m_operationQueue.pop_front();
        if (op.kind == PendingOperation::Kind::Poll)
            m_pollQueued = false;
        const std::int64_t startedMs = nowMs();
        const std::int64_t waitMs = (op.enqueuedMs > 0) ? (startedMs - op.enqueuedMs) : -1;

        switch (op.kind) {
        case PendingOperation::Kind::Poll:
            timingLog(QStringLiteral("cmd.start type=poll waitMs=%1 queueSize=%2")
                          .arg(waitMs)
                          .arg(static_cast<int>(m_operationQueue.size())));
            m_pollRunning = true;
            if (m_started && !m_stopping)
                requestInitialState();
            m_pollRunning = false;
            resetPollTimerCountdown();
            timingLog(QStringLiteral("cmd.end type=poll durationMs=%1")
                          .arg(nowMs() - startedMs));
            break;
        case PendingOperation::Kind::ChannelInvoke: {
            timingLog(QStringLiteral("cmd.start type=channel.invoke cmdId=%1 channel=%2 waitMs=%3 queueSize=%4")
                          .arg(op.channelRequest.cmdId)
                          .arg(QString::fromStdString(op.channelRequest.channelExternalId))
                          .arg(waitMs)
                          .arg(static_cast<int>(m_operationQueue.size())));
            v1::CmdResponse response = handleChannelInvoke(op.channelRequest);
            const bool isPowerInvoke = (op.channelRequest.channelExternalId == kChannelPower);
            const bool cmdSuccess = (response.status == v1::CmdStatus::Success);
            submitCmdResult(std::move(response), "channel.invoke");
            if (!isPowerInvoke && cmdSuccess) {
                resetPollTimerCountdown();
                QTimer::singleShot(1000, [this]() {
                    if (m_started && !m_stopping)
                        enqueuePollOperation(true);
                });
            }
            timingLog(QStringLiteral("cmd.end type=channel.invoke cmdId=%1 durationMs=%2")
                          .arg(op.channelRequest.cmdId)
                          .arg(nowMs() - startedMs));
            break;
        }
        case PendingOperation::Kind::ProbeCurrentInput: {
            timingLog(QStringLiteral("cmd.start type=adapter.action.invoke cmdId=%1 action=%2 waitMs=%3 queueSize=%4")
                          .arg(op.actionRequest.cmdId)
                          .arg(QString::fromStdString(op.actionRequest.actionId))
                          .arg(waitMs)
                          .arg(static_cast<int>(m_operationQueue.size())));
            v1::ActionResponse response =
                handleProbeCurrentInput(op.actionRequest, op.pollWasRunning);
            submitActionResult(std::move(response), "adapter.action.invoke");
            timingLog(QStringLiteral("cmd.end type=adapter.action.invoke cmdId=%1 action=%2 durationMs=%3")
                          .arg(op.actionRequest.cmdId)
                          .arg(QString::fromStdString(op.actionRequest.actionId))
                          .arg(nowMs() - startedMs));
            break;
        }
        }

        m_operationRunning = false;
        if (!m_operationQueue.empty())
            scheduleQueuePump();
    }

    void flushPendingOperations(const v1::Utf8String &reason)
    {
        if (m_operationQueue.empty()) {
            m_pollQueued = false;
            return;
        }

        std::deque<PendingOperation> pending;
        pending.swap(m_operationQueue);
        m_pollQueued = false;

        for (PendingOperation &op : pending) {
            if (op.kind == PendingOperation::Kind::ChannelInvoke) {
                v1::CmdResponse response;
                response.id = op.channelRequest.cmdId;
                response.tsMs = nowMs();
                response.status = v1::CmdStatus::Failure;
                response.error = reason;
                submitCmdResult(std::move(response), "channel.invoke.flush");
                continue;
            }
            if (op.kind == PendingOperation::Kind::ProbeCurrentInput) {
                v1::ActionResponse response;
                response.id = op.actionRequest.cmdId;
                response.tsMs = nowMs();
                response.status = v1::CmdStatus::Failure;
                response.error = reason;
                response.resultType = v1::ActionResultType::None;
                submitActionResult(std::move(response), "adapter.action.invoke.flush");
            }
        }
    }

    void resetPollTimerCountdown()
    {
        if (!m_pollTimer || !m_started || m_stopping)
            return;
        updatePollInterval();
        if (m_pollTimer->isActive())
            m_pollTimer->stop();
        m_pollTimer->start();
    }

    v1::CmdResponse handleChannelInvoke(const sdk::ChannelInvokeRequest &request)
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

            if (m_powerState == PowerState::On && *on) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = true;
                return resp;
            }
            if (m_powerState == PowerState::Off && !*on) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = false;
                return resp;
            }

            const QByteArray command = *on ? QByteArrayLiteral("PWR01") : QByteArrayLiteral("PWR00");
            if (!sendIscpCommand(command, false, 0)) {
                resp.status = unavailableCommandStatus();
                resp.error = unavailableCommandMessage();
                return resp;
            }

            resp.status = v1::CmdStatus::Success;
            resp.finalValue = *on;
            emitPowerState(*on);
            return resp;
        }

        if (m_powerState == PowerState::Unknown)
            requestInitialState();

        if (m_powerState == PowerState::Off) {
            resp.status = v1::CmdStatus::Failure;
            resp.error = "Standby";
            return resp;
        }

        if (m_powerState != PowerState::On) {
            resp.status = v1::CmdStatus::TemporarilyOffline;
            resp.error = "Power state unknown";
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
            if (sendIscpCommand(payload, false, 0)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = static_cast<std::int64_t>(qRound(clampedPercent));
                emitVolumeState(static_cast<std::int64_t>(qRound(clampedPercent)));
            } else {
                resp.status = unavailableCommandStatus();
                resp.error = unavailableCommandMessage();
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
            if (sendIscpCommand(*muted ? QByteArrayLiteral("AMT01") : QByteArrayLiteral("AMT00"), false, 0)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = *muted;
                emitMuteState(*muted);
            } else {
                resp.status = unavailableCommandStatus();
                resp.error = unavailableCommandMessage();
            }
            return resp;
        }

        if (request.channelExternalId == kChannelInput) {
            QString input = scalarToQString(request.value).trimmed();

            const QString labelMatch = input.toLower();
            for (auto it = m_inputLabelMap.constBegin(); it != m_inputLabelMap.constEnd(); ++it) {
                if (it.value().toLower() == labelMatch) {
                    input = it.key();
                    break;
                }
            }
            input = normalizeSliCode(input);

            if (input.length() != 2) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Input expects 2-digit code (e.g. 01)";
                return resp;
            }

            if (sendIscpCommand(QByteArrayLiteral("SLI") + input.toLatin1(), false, 0)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = input.toStdString();
                m_lastInputCode = input;
                emitInputState(input);
            } else {
                resp.status = unavailableCommandStatus();
                resp.error = unavailableCommandMessage();
            }
            return resp;
        }

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Channel not supported";
        return resp;
    }

    v1::ActionResponse handleAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request)
    {
        v1::ActionResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        if (request.actionId == "settings") {
            const QJsonObject params = parseJsonObject(request.paramsJson);
            if (!params.isEmpty()) {
                QJsonObject patch;
                for (auto it = params.begin(); it != params.end(); ++it) {
                    const QString key = it.key().trimmed();
                    if (key.isEmpty())
                        continue;
                    if (key == QLatin1String("currentInputCode"))
                        continue;

                    if (key == QLatin1String("activeSliCodes")) {
                        patch.insert(QStringLiteral("activeSliCodes"),
                                     normalizeActiveSliCodesArray(it.value()));
                        continue;
                    }

                    if (key.startsWith(QLatin1String("inputLabel_"))) {
                        const QString code = normalizeSliCode(key.mid(11));
                        if (code.isEmpty())
                            continue;
                        const QString normalizedKey = QStringLiteral("inputLabel_%1").arg(code);
                        patch.insert(normalizedKey, it.value().toString().trimmed());
                        continue;
                    }

                    patch.insert(key, it.value());
                }

                for (auto it = patch.begin(); it != patch.end(); ++it)
                    m_meta.insert(it.key(), it.value());
                m_info.metaJson = toJson(m_meta);
                applyConfig();
                v1::Utf8String err;
                if (!sendAdapterMetaUpdated(toJson(patch), &err))
                    std::cerr << "failed to send adapterMetaUpdated: " << err << '\n';
                if (m_synced && !m_deviceId.empty()) {
                    v1::Utf8String chErr;
                    if (!sendChannelUpdated(m_deviceId, buildInputChannel(), &chErr))
                        std::cerr << "failed to send channelUpdated(input): " << chErr << '\n';
                } else {
                    emitDeviceSnapshot();
                }
                enqueuePollOperation(true);
            }
            resp.status = v1::CmdStatus::Success;
            resp.resultType = v1::ActionResultType::None;
            return resp;
        }

        if (request.actionId == "probeCurrentInput")
            return handleProbeCurrentInput(request, false);

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Adapter action not supported";
        return resp;
    }

    v1::ActionResponse handleProbeCurrentInput(const sdk::AdapterActionInvokeRequest &request,
                                               bool fromRunningPoll)
    {
        v1::ActionResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();
        const QString before = m_lastInputCode;
        QString resolvedCode;

        // If probe was triggered while a poll was already running, return poll result only.
        if (fromRunningPoll) {
            if (!m_lastInputCode.isEmpty()) {
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
        } else if (!sendIscpCommand(QByteArrayLiteral("SLIQSTN"),
                                    true,
                                    700,
                                    900,
                                    2)) {
            resp.status = unavailableCommandStatus();
            resp.error = unavailableCommandMessage();
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
            const QString normalized = normalizeSliCode(resolvedCode);

            QSet<QString> activeCodes;
            const QJsonValue activeValue = m_meta.value(QStringLiteral("activeSliCodes"));
            if (activeValue.isArray()) {
                const QJsonArray activeArray = activeValue.toArray();
                for (const QJsonValue &entry : activeArray) {
                    QString code;
                    if (entry.isString()) {
                        code = normalizeSliCode(entry.toString());
                    } else if (entry.isDouble()) {
                        code = normalizeSliCode(QString::number(entry.toInt()));
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
            const QString defaultLabel = m_defaultInputLabelMap.value(normalized).trimmed();
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

            resp.formValuesJson = toJson(QJsonObject{
                {QStringLiteral("activeSliCodes"), nextActive},
                {QStringLiteral("currentInputCode"), normalized},
            });
            resp.fieldChoicesJson = toJson(QJsonObject{
                {QStringLiteral("activeSliCodes"), optionsToChoiceJson(inputChoicesForChannel())},
            });
            resp.reloadLayout = true;

            v1::Utf8String err;
            if (!sendAdapterMetaUpdated(toJson(patch), &err))
                std::cerr << "failed to send adapterMetaUpdated(probeCurrentInput): " << err << '\n';
        }
        return resp;
    }

    v1::CmdResponse handleDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request)
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device rename not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    v1::CmdResponse handleDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request)
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device effect not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    v1::CmdResponse handleSceneInvoke(const sdk::SceneInvokeRequest &request)
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Scene invocation not supported";
        resp.tsMs = nowMs();
        return resp;
    }

private:
    void startPollingTimer()
    {
        if (!m_pollTimer) {
            m_pollTimer = std::make_unique<QTimer>();
            m_pollTimer->setSingleShot(false);
            QObject::connect(m_pollTimer.get(), &QTimer::timeout, [this]() {
                enqueuePollOperation(false);
            });
        }
        updatePollInterval();
        if (!m_pollTimer->isActive())
            m_pollTimer->start();
    }

    void stopPollingTimer()
    {
        if (m_pollTimer && m_pollTimer->isActive())
            m_pollTimer->stop();
    }

    v1::AdapterConfigOptionList inputChoicesForChannel() const
    {
        v1::AdapterConfigOptionList choices;
        const QJsonArray normalizedActive = normalizeActiveSliCodesArray(m_meta.value(QStringLiteral("activeSliCodes")));
        QStringList keys;
        keys.reserve(normalizedActive.size());
        for (const QJsonValue &entry : normalizedActive) {
            const QString code = normalizeSliCode(entry.toString());
            if (!code.isEmpty() && !keys.contains(code))
                keys.push_back(code);
        }
        if (keys.isEmpty()) {
            keys = m_defaultInputLabelMap.keys();
            std::sort(keys.begin(), keys.end());
        }
        if (keys.isEmpty()) {
            keys = m_inputLabelMap.keys();
            std::sort(keys.begin(), keys.end());
        }
        choices.reserve(static_cast<std::size_t>(keys.size()));
        for (const QString &key : keys) {
            v1::AdapterConfigOption option;
            option.value = key.toStdString();
            option.label = formatSliDisplayLabel(key, m_inputLabelMap.value(key)).toStdString();
            choices.push_back(std::move(option));
        }
        return choices;
    }

    void applyConfig()
    {
        const std::uint16_t configuredIscpPort =
            normalizedPort(m_meta.value(QStringLiteral("iscpPort")).toInt(0));
        const std::uint16_t discoveredPort = normalizedPort(static_cast<int>(m_info.port));
        const std::uint16_t effectivePort = configuredIscpPort > 0 ? configuredIscpPort : discoveredPort;
        m_controlPort = resolvedControlPort(effectivePort);
        m_pollIntervalMs = qBound(500,
                                  m_meta.value(QStringLiteral("pollIntervalMs")).toInt(5000),
                                  300000);
        m_retryIntervalMs = qBound(1000,
                                   m_meta.value(QStringLiteral("retryIntervalMs")).toInt(10000),
                                   300000);
        m_volumeMaxRaw = qBound(1,
                                m_meta.value(QStringLiteral("volumeMaxRaw")).toInt(160),
                                500);
        reloadInputLabelMap();
        updatePollInterval();
    }

    QStringList effectiveHosts() const
    {
        QStringList result;
        const QString ip = QString::fromStdString(m_info.ip).trimmed();
        if (!ip.isEmpty()) {
            QHostAddress addr;
            if (addr.setAddress(ip))
                result.push_back(ip);
        }
        return result;
    }

    void updatePollInterval()
    {
        if (!m_pollTimer)
            return;
        const int interval = m_connected ? m_pollIntervalMs : m_retryIntervalMs;
        if (m_pollTimer->interval() != interval)
            m_pollTimer->setInterval(interval);
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

    void markConnectSuccess()
    {
        const bool wasConnected = m_connected;
        m_consecutiveConnectFailures = 0;
        setConnected(true);
        if (!wasConnected)
            emitChannelState(QString::fromLatin1(kChannelConnectivity),
                             static_cast<std::int64_t>(v1::ConnectivityStatus::Connected));
    }

    void markConnectFailure()
    {
        if (m_consecutiveConnectFailures < std::numeric_limits<int>::max())
            ++m_consecutiveConnectFailures;
        if (m_consecutiveConnectFailures < kConnectFailuresBeforeDisconnect)
            return;
        const bool wasConnected = m_connected;
        setConnected(false);
        if (wasConnected)
            emitChannelState(QString::fromLatin1(kChannelConnectivity),
                             static_cast<std::int64_t>(v1::ConnectivityStatus::Disconnected));
        m_powerState = PowerState::Unknown;
    }

    bool hasQueuedPriorityWork() const
    {
        for (const PendingOperation &op : m_operationQueue) {
            if (op.kind != PendingOperation::Kind::Poll)
                return true;
        }
        return false;
    }

    bool sendIscpCommand(const QByteArray &command,
                         bool parseResponse,
                         int responseTimeoutMs,
                         int connectTimeoutMs = 1500,
                         int maxAttempts = 2)
    {
        QElapsedTimer totalTimer;
        totalTimer.start();
        const QStringList hostCandidates = effectiveHosts();
        if (hostCandidates.isEmpty() || m_controlPort == 0) {
            trace(QStringLiteral("iscp skip cmd=%1 reason=no-host-or-port hostCount=%2 port=%3")
                      .arg(QString::fromLatin1(command))
                      .arg(hostCandidates.size())
                      .arg(m_controlPort));
            return false;
        }
        timingLog(QStringLiteral("iscp.start cmd=%1 parseResponse=%2 responseTimeoutMs=%3 connectTimeoutMs=%4 attempts=%5 hostCount=%6 port=%7")
                      .arg(QString::fromLatin1(command))
                      .arg(parseResponse ? 1 : 0)
                      .arg(responseTimeoutMs)
                      .arg(connectTimeoutMs)
                      .arg(maxAttempts)
                      .arg(hostCandidates.size())
                      .arg(m_controlPort));
        trace(QStringLiteral("iscp start cmd=%1 parseResponse=%2 responseTimeoutMs=%3 hosts=%4 port=%5")
                  .arg(QString::fromLatin1(command))
                  .arg(parseResponse ? 1 : 0)
                  .arg(responseTimeoutMs)
                  .arg(hostCandidates.join(QLatin1Char(',')))
                  .arg(m_controlPort));

        auto connectSocket = [&](QTcpSocket &socket, QString &connectedHost) -> bool {
            for (const QString &host : hostCandidates) {
                QElapsedTimer connectTimer;
                connectTimer.start();
                socket.abort();
                socket.connectToHost(host, m_controlPort);

                int waitedMs = 0;
                while (!socket.waitForConnected(100)) {
                    waitedMs += 100;
                    if (waitedMs >= connectTimeoutMs) {
                        timingLog(QStringLiteral("iscp.connect.timeout cmd=%1 host=%2 waitedMs=%3 err=%4")
                                      .arg(QString::fromLatin1(command))
                                      .arg(host)
                                      .arg(waitedMs)
                                      .arg(socket.errorString()));
                        if (socket.state() != QAbstractSocket::ConnectedState)
                            logConnectFailure(socket.errorString(), host);
                        break;
                    }
                }
                if (socket.state() == QAbstractSocket::ConnectedState) {
                    connectedHost = host;
                    timingLog(QStringLiteral("iscp.connect.ok cmd=%1 host=%2 elapsedMs=%3")
                                  .arg(QString::fromLatin1(command))
                                  .arg(host)
                                  .arg(connectTimer.elapsed()));
                    trace(QStringLiteral("iscp connected cmd=%1 host=%2 port=%3 connectElapsedMs=%4")
                              .arg(QString::fromLatin1(command))
                              .arg(host)
                              .arg(m_controlPort)
                              .arg(connectTimer.elapsed()));
                    return true;
                }
            }
            return false;
        };

        auto executeOnce = [&](QTcpSocket &socket, const qint64 afterConnectMs) -> bool {
            if (!kUseEiscp) {
                const QByteArray terminator = kUseCrlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\r");
                trace(QStringLiteral("iscp phase cmd=%1 phase=write-begin elapsedMs=%2")
                          .arg(QString::fromLatin1(command))
                          .arg(afterConnectMs));
                if (socket.write(QByteArrayLiteral("!1") + command + terminator) < 0) {
                    trace(QStringLiteral("iscp write-failed cmd=%1 mode=plain error=%2")
                              .arg(QString::fromLatin1(command))
                              .arg(socket.errorString()));
                    return false;
                }
                trace(QStringLiteral("iscp phase cmd=%1 phase=write-end elapsedMs=%2")
                          .arg(QString::fromLatin1(command))
                          .arg(totalTimer.elapsed()));

                if (parseResponse && responseTimeoutMs > 0) {
                if (!socket.waitForReadyRead(responseTimeoutMs))
                    return false;
                const QByteArray data = socket.readAll();
                timingLog(QStringLiteral("iscp.read cmd=%1 bytes=%2 elapsedMs=%3")
                              .arg(QString::fromLatin1(command))
                              .arg(data.size())
                              .arg(totalTimer.elapsed()));
                if (data.isEmpty())
                    return false;
                processResponseData(data);
                }
                return true;
            }

            const QByteArray frame = buildEiscpFrame(command);
            trace(QStringLiteral("iscp phase cmd=%1 phase=write-begin elapsedMs=%2")
                      .arg(QString::fromLatin1(command))
                      .arg(afterConnectMs));
            if (socket.write(frame) < 0) {
                trace(QStringLiteral("iscp write-failed cmd=%1 mode=eiscp error=%2")
                          .arg(QString::fromLatin1(command))
                          .arg(socket.errorString()));
                return false;
            }
            trace(QStringLiteral("iscp phase cmd=%1 phase=write-end elapsedMs=%2")
                      .arg(QString::fromLatin1(command))
                      .arg(totalTimer.elapsed()));

            if (!parseResponse || responseTimeoutMs <= 0)
                return true;

            QByteArray data;
            int readWaitedMs = 0;
            bool firstWaitLogged = false;
            trace(QStringLiteral("iscp phase cmd=%1 phase=read-loop-begin elapsedMs=%2 timeoutMs=%3")
                      .arg(QString::fromLatin1(command))
                      .arg(totalTimer.elapsed())
                      .arg(responseTimeoutMs));
            while (readWaitedMs < responseTimeoutMs) {
                const bool ready = socket.waitForReadyRead(100);
                if (!firstWaitLogged) {
                    trace(QStringLiteral("iscp phase cmd=%1 phase=first-readyread ready=%2 elapsedMs=%3")
                              .arg(QString::fromLatin1(command))
                              .arg(ready ? 1 : 0)
                              .arg(totalTimer.elapsed()));
                    firstWaitLogged = true;
                }
                if (ready) {
                    data.append(socket.readAll());
                    QElapsedTimer coalesceTimer;
                    coalesceTimer.start();
                    while (coalesceTimer.elapsed() < 120) {
                        if (!socket.waitForReadyRead(10))
                            break;
                        const QByteArray chunk = socket.readAll();
                        if (chunk.isEmpty())
                            break;
                        data.append(chunk);
                    }
                    break;
                }
                readWaitedMs += 100;
            }

            trace(QStringLiteral("iscp read cmd=%1 readWaitedMs=%2 bytes=%3")
                      .arg(QString::fromLatin1(command))
                      .arg(readWaitedMs)
                      .arg(data.size()));
            timingLog(QStringLiteral("iscp.read cmd=%1 readWaitedMs=%2 bytes=%3 elapsedMs=%4")
                          .arg(QString::fromLatin1(command))
                          .arg(readWaitedMs)
                          .arg(data.size())
                          .arg(totalTimer.elapsed()));
            if (data.isEmpty())
                return false;

            processResponseData(data);
            return true;
        };

        if (maxAttempts < 1)
            maxAttempts = 1;
        bool hadConnectedSession = false;
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            QTcpSocket socket;
            socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
            QString connectedHost;
            if (!connectSocket(socket, connectedHost))
                continue;

            hadConnectedSession = true;
            markConnectSuccess();
            const bool ok = executeOnce(socket, totalTimer.elapsed());

            trace(QStringLiteral("iscp phase cmd=%1 phase=disconnect-begin elapsedMs=%2")
                      .arg(QString::fromLatin1(command))
                      .arg(totalTimer.elapsed()));
            socket.disconnectFromHost();
            if (socket.state() != QAbstractSocket::UnconnectedState) {
                int disconnectWaitedMs = 0;
                while (socket.state() != QAbstractSocket::UnconnectedState && disconnectWaitedMs < 300) {
                    socket.waitForDisconnected(50);
                    disconnectWaitedMs += 50;
                }
            }
            trace(QStringLiteral("iscp phase cmd=%1 phase=disconnect-end elapsedMs=%2")
                      .arg(QString::fromLatin1(command))
                      .arg(totalTimer.elapsed()));

            if (ok) {
                timingLog(QStringLiteral("iscp.done cmd=%1 status=success elapsedMs=%2 attempt=%3")
                              .arg(QString::fromLatin1(command))
                              .arg(totalTimer.elapsed())
                              .arg(attempt + 1));
                trace(QStringLiteral("iscp done cmd=%1 elapsedMs=%2 attempt=%3")
                          .arg(QString::fromLatin1(command))
                          .arg(totalTimer.elapsed())
                          .arg(attempt + 1));
                return true;
            }
        }

        if (!hadConnectedSession)
            markConnectFailure();
        timingLog(QStringLiteral("iscp.done cmd=%1 status=failure elapsedMs=%2")
                      .arg(QString::fromLatin1(command))
                      .arg(totalTimer.elapsed()));
        trace(QStringLiteral("iscp failed cmd=%1 totalElapsedMs=%2")
                  .arg(QString::fromLatin1(command))
                  .arg(totalTimer.elapsed()));
        return false;
    }

    bool sendIscpPollBatch(const std::array<QByteArray, 4> &commands,
                           int responseTimeoutMs)
    {
        const int connectTimeoutMs = 1500;
        const QStringList hostCandidates = effectiveHosts();
        if (hostCandidates.isEmpty() || m_controlPort == 0)
            return false;
        const std::int64_t pollStartMs = nowMs();
        timingLog(QStringLiteral("poll.batch.start hostCount=%1 port=%2 responseTimeoutMs=%3 queueSize=%4")
                      .arg(hostCandidates.size())
                      .arg(m_controlPort)
                      .arg(responseTimeoutMs)
                      .arg(static_cast<int>(m_operationQueue.size())));
        bool interrupted = false;
        auto shouldInterrupt = [&]() -> bool {
            if (hasQueuedPriorityWork()) {
                interrupted = true;
                return true;
            }
            return false;
        };

        auto connectSocket = [&](QTcpSocket &socket) -> bool {
            for (const QString &host : hostCandidates) {
                if (shouldInterrupt())
                    return false;
                socket.abort();
                socket.connectToHost(host, m_controlPort);

                int waitedMs = 0;
                while (!socket.waitForConnected(100)) {
                    if (shouldInterrupt()) {
                        socket.abort();
                        return false;
                    }
                    waitedMs += 100;
                    if (waitedMs >= connectTimeoutMs) {
                        if (socket.state() != QAbstractSocket::ConnectedState)
                            logConnectFailure(socket.errorString(), host);
                        break;
                    }
                }
                if (socket.state() == QAbstractSocket::ConnectedState)
                    return true;
            }
            return false;
        };

        auto sendQueryOnConnectedSocket = [&](QTcpSocket &socket, const QByteArray &command) -> bool {
            if (!kUseEiscp) {
                const QByteArray terminator = kUseCrlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\r");
                if (socket.write(QByteArrayLiteral("!1") + command + terminator) < 0)
                    return false;
                if (responseTimeoutMs <= 0)
                    return true;
                int readWaitedMs = 0;
                while (!socket.waitForReadyRead(100)) {
                    if (shouldInterrupt())
                        return false;
                    readWaitedMs += 100;
                    if (readWaitedMs >= responseTimeoutMs)
                        return false;
                }
                const QByteArray data = socket.readAll();
                if (data.isEmpty())
                    return false;
                processResponseData(data);
                return true;
            }

            const QByteArray frame = buildEiscpFrame(command);
            if (socket.write(frame) < 0)
                return false;
            if (responseTimeoutMs <= 0)
                return true;

            QByteArray data;
            int readWaitedMs = 0;
            while (readWaitedMs < responseTimeoutMs) {
                if (shouldInterrupt())
                    return false;
                if (socket.waitForReadyRead(100)) {
                    data.append(socket.readAll());
                    QElapsedTimer coalesceTimer;
                    coalesceTimer.start();
                    while (coalesceTimer.elapsed() < 120) {
                        if (shouldInterrupt())
                            return false;
                        if (!socket.waitForReadyRead(10))
                            break;
                        const QByteArray chunk = socket.readAll();
                        if (chunk.isEmpty())
                            break;
                        data.append(chunk);
                    }
                    break;
                }
                readWaitedMs += 100;
            }

            if (data.isEmpty())
                return false;
            processResponseData(data);
            return true;
        };

        QTcpSocket socket;
        socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        if (!connectSocket(socket)) {
            if (interrupted)
                return true;
            markConnectFailure();
            return false;
        }

        markConnectSuccess();
        bool allSucceeded = true;
        bool sawConnectFailure = false;

        for (const QByteArray &command : commands) {
            if (!m_started || m_stopping) {
                allSucceeded = false;
                break;
            }
            if (shouldInterrupt())
                break;

            bool commandSucceeded = sendQueryOnConnectedSocket(socket, command);
            if (!commandSucceeded) {
                if (interrupted)
                    break;
                socket.abort();
                if (connectSocket(socket)) {
                    if (interrupted)
                        break;
                    markConnectSuccess();
                    commandSucceeded = sendQueryOnConnectedSocket(socket, command);
                } else {
                    if (!interrupted)
                        sawConnectFailure = true;
                }
            }

            if (!commandSucceeded) {
                if (interrupted)
                    break;
                allSucceeded = false;
                break;
            }
            if (shouldInterrupt())
                break;
        }

        socket.disconnectFromHost();
        if (socket.state() != QAbstractSocket::UnconnectedState) {
            int disconnectWaitedMs = 0;
            while (socket.state() != QAbstractSocket::UnconnectedState && disconnectWaitedMs < 300) {
                socket.waitForDisconnected(50);
                disconnectWaitedMs += 50;
            }
        }

        if (sawConnectFailure && !interrupted)
            markConnectFailure();

        if (interrupted) {
            timingLog(QStringLiteral("poll.batch.end status=interrupted elapsedMs=%1").arg(nowMs() - pollStartMs));
            return true;
        }
        timingLog(QStringLiteral("poll.batch.end status=%1 elapsedMs=%2")
                      .arg(allSucceeded ? QStringLiteral("success") : QStringLiteral("failure"))
                      .arg(nowMs() - pollStartMs));
        return allSucceeded;
    }

    v1::CmdStatus unavailableCommandStatus() const
    {
        return v1::CmdStatus::TemporarilyOffline;
    }

    v1::Utf8String unavailableCommandMessage() const
    {
        return v1::Utf8String("Receiver unavailable");
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
                    emitPowerState(value == "01");
                    }
                continue;
            }

            if (line.startsWith("AMT")) {
                const QByteArray value = line.mid(3);
                if (value == "01" || value == "00") {
                    emitMuteState(value == "01");
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
                    emitVolumeState(static_cast<std::int64_t>(qRound(normalized)));
                }
                continue;
            }

            if (line.startsWith("SLI")) {
                QString code = QString::fromLatin1(line.mid(3)).trimmed().toUpper();
                static const QRegularExpression kCodeRe(QStringLiteral("^[0-9A-F]{2}$"));
                if (kCodeRe.match(code).hasMatch()) {
                    m_lastInputCode = code;
                    emitInputState(m_lastInputCode);
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
        volume.dataType = v1::ChannelDataType::Int;
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
        input.choices = inputChoicesForChannel();
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
        input.choices = inputChoicesForChannel();
        return input;
    }

    std::string resolveDeviceId() const
    {
        const QString uuid = m_meta.value(QStringLiteral("deviceUuid")).toString().trimmed();
        if (!uuid.isEmpty())
            return uuid.toStdString();

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

        static const std::array<QByteArray, 4> commands = {
            QByteArrayLiteral("PWRQSTN"),
            QByteArrayLiteral("MVLQSTN"),
            QByteArrayLiteral("AMTQSTN"),
            QByteArrayLiteral("SLIQSTN"),
        };
        sendIscpPollBatch(commands, kPollQueryTimeoutMs);
    }

    void reloadInputLabelMap()
    {
        m_inputLabelMap.clear();

        const QJsonArray activeCodes = normalizeActiveSliCodesArray(m_meta.value(QStringLiteral("activeSliCodes")));
        for (const QJsonValue &entry : activeCodes) {
            const QString code = normalizeSliCode(entry.toString());
            if (code.isEmpty())
                continue;
            const QString customLabel = m_meta.value(QStringLiteral("inputLabel_%1").arg(code)).toString().trimmed();
            if (!customLabel.isEmpty()) {
                m_inputLabelMap.insert(code, customLabel);
                continue;
            }
            const QString defaultLabel = m_defaultInputLabelMap.value(code).trimmed();
            m_inputLabelMap.insert(code, defaultLabel.isEmpty() ? QStringLiteral("SLI %1").arg(code) : defaultLabel);
        }
    }

    void setConnected(bool connected)
    {
        if (m_connected == connected)
            return;
        m_connected = connected;
        updatePollInterval();
        v1::Utf8String err;
        if (!sendConnectionStateChanged(m_connected, &err))
            std::cerr << "failed to send connectionStateChanged: " << err << '\n';
    }

    void emitChannelState(const QString &channelId, const v1::ScalarValue &value)
    {
        if (m_deviceId.empty())
            return;
        v1::Utf8String err;
        if (!sendChannelStateUpdated(m_deviceId, channelId.toStdString(), value, nowMs(), &err))
            std::cerr << "failed to send channel state for " << channelId.toStdString() << ": " << err << '\n';
    }

    void emitPowerState(bool value)
    {
        m_powerState = value ? PowerState::On : PowerState::Off;
        if (m_lastReportedPower.has_value() && m_lastReportedPower.value() == value)
            return;
        m_lastReportedPower = value;
        emitChannelState(QString::fromLatin1(kChannelPower), value);
    }

    void emitMuteState(bool value)
    {
        if (m_lastReportedMute.has_value() && m_lastReportedMute.value() == value)
            return;
        m_lastReportedMute = value;
        emitChannelState(QString::fromLatin1(kChannelMute), value);
    }

    void emitVolumeState(std::int64_t value)
    {
        if (m_lastReportedVolume.has_value() && m_lastReportedVolume.value() == value)
            return;
        m_lastReportedVolume = value;
        emitChannelState(QString::fromLatin1(kChannelVolume), value);
    }

    void emitInputState(const QString &value)
    {
        if (m_hasLastReportedInput && m_lastReportedInput == value)
            return;
        m_lastReportedInput = value;
        m_hasLastReportedInput = true;
        emitChannelState(QString::fromLatin1(kChannelInput), value.toStdString());
    }

    void submitCmdResult(v1::CmdResponse response, const char *context)
    {
        timingLog(QStringLiteral("cmd.result.send context=%1 cmdId=%2 status=%3 error=%4")
                      .arg(QString::fromLatin1(context))
                      .arg(response.id)
                      .arg(static_cast<int>(response.status))
                      .arg(QString::fromStdString(response.error)));
        trace(QStringLiteral("result cmd context=%1 id=%2 status=%3 err=%4")
                  .arg(QString::fromLatin1(context))
                  .arg(response.id)
                  .arg(static_cast<int>(response.status))
                  .arg(QString::fromStdString(response.error)));
        v1::Utf8String err;
        if (!sendResult(response, &err))
            std::cerr << "failed to send " << context << " result: " << err << '\n';
    }

    void submitActionResult(v1::ActionResponse response, const char *context)
    {
        timingLog(QStringLiteral("action.result.send context=%1 cmdId=%2 status=%3 error=%4")
                      .arg(QString::fromLatin1(context))
                      .arg(response.id)
                      .arg(static_cast<int>(response.status))
                      .arg(QString::fromStdString(response.error)));
        trace(QStringLiteral("result action context=%1 id=%2 status=%3 err=%4")
                  .arg(QString::fromLatin1(context))
                  .arg(response.id)
                  .arg(static_cast<int>(response.status))
                  .arg(QString::fromStdString(response.error)));
        v1::Utf8String err;
        if (!sendResult(response, &err))
            std::cerr << "failed to send " << context << " result: " << err << '\n';
    }

    v1::Adapter m_info;
    QJsonObject m_meta;

    bool m_started = false;
    bool m_stopping = false;
    bool m_connected = false;
    bool m_synced = false;

    std::string m_deviceId;
    std::uint16_t m_controlPort = 0;
    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    int m_volumeMaxRaw = 160;

    std::int64_t m_lastConnectLogMs = 0;

    QString m_lastConnectError;
    QString m_lastInputCode;
    std::optional<bool> m_lastReportedPower;
    std::optional<bool> m_lastReportedMute;
    std::optional<std::int64_t> m_lastReportedVolume;
    QString m_lastReportedInput;
    bool m_hasLastReportedInput = false;
    int m_consecutiveConnectFailures = 0;
    PowerState m_powerState = PowerState::Unknown;
    QHash<QString, QString> m_defaultInputLabelMap;
    QHash<QString, QString> m_inputLabelMap;
    std::deque<PendingOperation> m_operationQueue;
    bool m_operationRunning = false;
    bool m_queuePumpScheduled = false;
    bool m_pollQueued = false;
    bool m_pollRunning = false;

    std::unique_ptr<QTimer> m_pollTimer;
};

class OnkyoIpcFactory final : public sdk::AdapterFactory
{
protected:
    void onBootstrap(const sdk::BootstrapRequest &request) override
    {
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
        return "Onkyo / Pioneer";
    }

    v1::Utf8String description() const override
    {
        return "Onkyo/Pioneer AVR adapter (ISCP sidecar)";
    }

    v1::Utf8String apiVersion() const override
    {
        return "1.0.0";
    }

    v1::Utf8String iconSvg() const override
    {
        return kOnkyoIconSvg;
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
        probeCurrent.hasForm = false;
        probeCurrent.metaJson = R"({"placement":"form_field","kind":"command","requiresAck":true})";
        caps.instanceActions.push_back(probeCurrent);

        return caps;
    }

    v1::JsonText configSchemaJson() const override
    {
        return toJson(buildConfigSchemaObject(m_schemaInputLabels));
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
        return std::make_unique<OnkyoIpcInstance>(m_schemaInputLabels);
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
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    host.stop();
    return 0;
}
