#include "onkyoadapterfactory.h"
#include "onkyoadapter.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTcpSocket>

namespace phicore::adapter {

namespace {

void addFieldByLegacyScope(AdapterConfigSchema &schema, const AdapterConfigField &field)
{
    const bool instanceOnly =
        (static_cast<int>(field.flags) & static_cast<int>(AdapterConfigFieldFlag::InstanceOnly)) != 0;
    if (!instanceOnly)
        schema.factory.fields.push_back(field);
    schema.instance.fields.push_back(field);
}

} // namespace

static const QByteArray kOnkyoIconSvg = QByteArrayLiteral(
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
    "xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Receiver icon\">\n"
    "  <rect x=\"3\" y=\"6\" width=\"18\" height=\"12\" rx=\"2.5\" "
    "stroke=\"#2E3A4F\" stroke-width=\"1.6\" fill=\"#121A26\"/>\n"
    "  <circle cx=\"8\" cy=\"12\" r=\"2.2\" stroke=\"#7A8AA4\" stroke-width=\"1.4\" fill=\"none\"/>\n"
    "  <rect x=\"13\" y=\"10.2\" width=\"7\" height=\"1.6\" rx=\"0.8\" fill=\"#7A8AA4\"/>\n"
    "  <rect x=\"13\" y=\"13\" width=\"5\" height=\"1.6\" rx=\"0.8\" fill=\"#7A8AA4\"/>\n"
    "</svg>\n"
);

QByteArray OnkyoAdapterFactory::icon() const
{
    return kOnkyoIconSvg;
}

static QByteArray buildEiscpFrame(const QByteArray &command)
{
    const QByteArray payload = QByteArrayLiteral("!1") + command + QByteArrayLiteral("\r");
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

AdapterCapabilities OnkyoAdapterFactory::capabilities() const
{
    AdapterCapabilities caps;
    caps.required = AdapterRequirement::Host | AdapterRequirement::UsesRetryInterval;
    caps.optional = AdapterRequirement::None;
    caps.flags |= AdapterFlag::AdapterFlagSupportsDiscovery;
    caps.flags |= AdapterFlag::AdapterFlagSupportsProbe;
    caps.flags |= AdapterFlag::AdapterFlagRequiresPolling;
    caps.defaults.insert(QStringLiteral("pollIntervalMs"), 5000);
    caps.defaults.insert(QStringLiteral("retryIntervalMs"), 10000);
    AdapterActionDescriptor settings;
    settings.id = QStringLiteral("settings");
    settings.label = QStringLiteral("Settings");
    settings.description = QStringLiteral("Edit receiver connection settings.");
    settings.hasForm = true;
    settings.meta.insert(QStringLiteral("placement"), QStringLiteral("card"));
    settings.meta.insert(QStringLiteral("kind"), QStringLiteral("open_dialog"));
    settings.meta.insert(QStringLiteral("requiresAck"), true);
    caps.instanceActions.push_back(settings);
    AdapterActionDescriptor probeCurrentAction;
    probeCurrentAction.id = QStringLiteral("probeCurrentInput");
    probeCurrentAction.label = QStringLiteral("Probe current");
    probeCurrentAction.description = QStringLiteral("Read current input (SLI) from the receiver.");
    probeCurrentAction.meta.insert(QStringLiteral("placement"), QStringLiteral("form_field"));
    probeCurrentAction.meta.insert(QStringLiteral("kind"), QStringLiteral("command"));
    probeCurrentAction.meta.insert(QStringLiteral("requiresAck"), true);
    probeCurrentAction.meta.insert(QStringLiteral("resultField"), QStringLiteral("currentInputCode"));
    caps.instanceActions.push_back(probeCurrentAction);
    AdapterActionDescriptor probeAction;
    probeAction.id = QStringLiteral("probe");
    probeAction.label = QStringLiteral("Test connection");
    probeAction.description = QStringLiteral("Reachability & command check");
    probeAction.meta.insert(QStringLiteral("placement"), QStringLiteral("card"));
    probeAction.meta.insert(QStringLiteral("kind"), QStringLiteral("command"));
    probeAction.meta.insert(QStringLiteral("requiresAck"), true);
    caps.factoryActions.push_back(probeAction);
    return caps;
}

discovery::DiscoveryQueryList OnkyoAdapterFactory::discoveryQueries() const
{
    discovery::DiscoveryQuery iscpQuery;
    iscpQuery.pluginType = pluginType();
    iscpQuery.kind = discovery::DiscoveryKind::Mdns;
    iscpQuery.mdnsServiceType = QStringLiteral("_iscp._tcp");
    iscpQuery.defaultPort = 60128;

    return { iscpQuery };
}

AdapterConfigSchema OnkyoAdapterFactory::configSchema(const Adapter &info) const
{
    AdapterConfigSchema schema;
    schema.factory.title = QStringLiteral("Onkyo / Pioneer Receiver");
    schema.factory.description = QStringLiteral("Configure connection to an Onkyo/Pioneer receiver (ISCP).");
    schema.instance.title = schema.factory.title;
    schema.instance.description = schema.factory.description;

    const QJsonObject txt = info.meta.value(QStringLiteral("txt")).toObject();
    const QString discoveredName =
        txt.value(QStringLiteral("name")).toString().trimmed();
    const QString discoveredManufacturer =
        txt.value(QStringLiteral("manufacturer")).toString().trimmed();
    const QString discoveredUuid =
        txt.value(QStringLiteral("uuid")).toString().trimmed();
    const QString discoveredModel =
        txt.value(QStringLiteral("model")).toString().trimmed();
    const bool supportsSpotify =
        txt.value(QStringLiteral("spotify")).toString().trimmed() == QLatin1String("true");
    const bool supportsTranscoder =
        txt.value(QStringLiteral("transcoder")).toString().trimmed() == QLatin1String("true");

    const QString resolvedHost = !info.host.isEmpty()
        ? info.host
        : (!info.ip.isEmpty()
            ? info.ip
            : info.meta.value(QStringLiteral("host")).toString().trimmed());
    AdapterConfigField hostField;
    hostField.key = QStringLiteral("host");
    hostField.label = QStringLiteral("Host");
    hostField.type = AdapterConfigFieldType::Hostname;
    hostField.flags = AdapterConfigFieldFlag::Required;
    if (!resolvedHost.isEmpty())
        hostField.defaultValue = resolvedHost;
    hostField.parentActionId = QStringLiteral("settings");
    addFieldByLegacyScope(schema, hostField);

    AdapterConfigField portField;
    portField.key = QStringLiteral("port");
    portField.label = QStringLiteral("ISCP Port");
    portField.type = AdapterConfigFieldType::Port;
    if (info.port > 0 && info.port != 80) {
        portField.defaultValue = static_cast<int>(info.port);
    } else {
        portField.defaultValue = 60128;
    }
    portField.parentActionId = QStringLiteral("settings");
    addFieldByLegacyScope(schema, portField);

    AdapterConfigField pollField;
    pollField.key = QStringLiteral("pollIntervalMs");
    pollField.label = QStringLiteral("Poll interval");
    pollField.type = AdapterConfigFieldType::Integer;
    pollField.defaultValue = 5000;
    pollField.parentActionId = QStringLiteral("settings");
    addFieldByLegacyScope(schema, pollField);

    AdapterConfigField retryField;
    retryField.key = QStringLiteral("retryIntervalMs");
    retryField.label = QStringLiteral("Retry interval");
    retryField.description = QStringLiteral("Reconnect interval while the receiver is offline.");
    retryField.type = AdapterConfigFieldType::Integer;
    retryField.defaultValue = 10000;
    retryField.parentActionId = QStringLiteral("settings");
    addFieldByLegacyScope(schema, retryField);

    AdapterConfigField volumeMaxField;
    volumeMaxField.key = QStringLiteral("volumeMaxRaw");
    volumeMaxField.label = QStringLiteral("Max volume raw");
    volumeMaxField.type = AdapterConfigFieldType::Integer;
    volumeMaxField.flags = AdapterConfigFieldFlag::InstanceOnly;
    volumeMaxField.defaultValue = 160;
    volumeMaxField.parentActionId = QStringLiteral("settings");
    addFieldByLegacyScope(schema, volumeMaxField);


    AdapterConfigField activeInputsField;
    activeInputsField.key = QStringLiteral("activeSliCodes");
    activeInputsField.label = QStringLiteral("Active SLI codes");
    activeInputsField.type = AdapterConfigFieldType::Select;
    activeInputsField.flags = AdapterConfigFieldFlag::Multi | AdapterConfigFieldFlag::InstanceOnly;
    activeInputsField.defaultValue = QStringList();
    activeInputsField.parentActionId = QStringLiteral("settings");

    struct InputLabelField {
        const char *code;
        const char *label;
    };
    const InputLabelField inputFields[] = {
        { "02", "GAME" },
        { "03", "AUX" },
        { "10", "BD/DVD" },
        { "12", "TV" },
        { "20", "TV" },
        { "21", "TV/CD" },
        { "22", "Cable/Sat" },
        { "23", "HDMI 1" },
        { "24", "HDMI 2" },
        { "25", "HDMI 3" },
        { "26", "HDMI 4" },
        { "30", "CD" },
        { "31", "FM" },
        { "32", "AM" },
        { "40", "USB" },
        { "41", "Network" },
        { "44", "Bluetooth" },
        { "2E", "BT Audio" },
        { "80", "USB Front" },
        { "81", "USB Rear" },
    };
    QMap<QString, QString> inputLabelMap;
    QSet<QString> activeCodes;
    for (const InputLabelField &entry : inputFields) {
        inputLabelMap.insert(QString::fromLatin1(entry.code), QString::fromLatin1(entry.label));
    }
    const QJsonValue activeValue = info.meta.value(QStringLiteral("activeSliCodes"));
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
    for (auto it = info.meta.constBegin(); it != info.meta.constEnd(); ++it) {
        const QString key = it.key();
        if (!key.startsWith(QLatin1String("inputLabel_")))
            continue;
        const QString code = key.mid(11).trimmed();
        if (code.isEmpty())
            continue;
        const QString label = it.value().toString().trimmed();
        if (!label.isEmpty())
            inputLabelMap.insert(code, label);
        activeCodes.insert(code);
    }
    for (const QString &code : std::as_const(activeCodes)) {
        if (!inputLabelMap.contains(code))
            inputLabelMap.insert(code, QStringLiteral("SLI %1").arg(code));
    }
    for (auto it = inputLabelMap.constBegin(); it != inputLabelMap.constEnd(); ++it) {
        AdapterConfigOption opt;
        opt.value = it.key();
        opt.label = it.value().isEmpty()
            ? QStringLiteral("SLI %1").arg(it.key())
            : it.value();
        activeInputsField.options.push_back(opt);
    }
    addFieldByLegacyScope(schema, activeInputsField);
    for (auto it = inputLabelMap.constBegin(); it != inputLabelMap.constEnd(); ++it) {
        const QString code = it.key();
        AdapterConfigField mapField;
        mapField.key = QStringLiteral("inputLabel_%1").arg(code);
        mapField.label = QStringLiteral("SLI %1").arg(code);
        mapField.type = AdapterConfigFieldType::String;
        mapField.flags = AdapterConfigFieldFlag::InstanceOnly;
        mapField.defaultValue = it.value().isEmpty()
            ? QStringLiteral("SLI %1").arg(code)
            : it.value();
        mapField.visibility.fieldKey = QStringLiteral("activeSliCodes");
        mapField.visibility.op = AdapterConfigVisibilityOp::Contains;
        mapField.visibility.value = code;
        mapField.parentActionId = QStringLiteral("settings");
        addFieldByLegacyScope(schema, mapField);
    }

    AdapterConfigField currentInputField;
    currentInputField.key = QStringLiteral("currentInputCode");
    currentInputField.label = QStringLiteral("Current input (SLI)");
    currentInputField.type = AdapterConfigFieldType::String;
    currentInputField.flags = AdapterConfigFieldFlag::ReadOnly
        | AdapterConfigFieldFlag::Transient
        | AdapterConfigFieldFlag::InstanceOnly;
    currentInputField.actionId = QStringLiteral("probeCurrentInput");
    currentInputField.actionLabel = QStringLiteral("Probe current");
    currentInputField.meta.insert(QStringLiteral("appendTo"), QStringLiteral("activeSliCodes"));
    currentInputField.parentActionId = QStringLiteral("settings");
    addFieldByLegacyScope(schema, currentInputField);

    if (!discoveredName.isEmpty()) {
        AdapterConfigField nameField;
        nameField.key = QStringLiteral("deviceName");
        nameField.label = QStringLiteral("Device name");
        nameField.type = AdapterConfigFieldType::String;
        nameField.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        nameField.defaultValue = discoveredName;
        nameField.parentActionId = QStringLiteral("settings");
        addFieldByLegacyScope(schema, nameField);
    }

    if (!discoveredManufacturer.isEmpty()) {
        AdapterConfigField manufacturerField;
        manufacturerField.key = QStringLiteral("manufacturer");
        manufacturerField.label = QStringLiteral("Manufacturer");
        manufacturerField.type = AdapterConfigFieldType::String;
        manufacturerField.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        manufacturerField.defaultValue = discoveredManufacturer;
        manufacturerField.parentActionId = QStringLiteral("settings");
        addFieldByLegacyScope(schema, manufacturerField);
    }

    if (!discoveredModel.isEmpty()) {
        AdapterConfigField modelField;
        modelField.key = QStringLiteral("model");
        modelField.label = QStringLiteral("Model");
        modelField.type = AdapterConfigFieldType::String;
        modelField.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        modelField.defaultValue = discoveredModel;
        modelField.parentActionId = QStringLiteral("settings");
        addFieldByLegacyScope(schema, modelField);
    }

    if (!discoveredUuid.isEmpty()) {
        AdapterConfigField uuidField;
        uuidField.key = QStringLiteral("deviceUuid");
        uuidField.label = QStringLiteral("UUID");
        uuidField.type = AdapterConfigFieldType::String;
        uuidField.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        uuidField.defaultValue = discoveredUuid;
        uuidField.parentActionId = QStringLiteral("settings");
        addFieldByLegacyScope(schema, uuidField);
    }

    if (supportsSpotify) {
        AdapterConfigField spotifyField;
        spotifyField.key = QStringLiteral("supportsSpotify");
        spotifyField.label = QStringLiteral("Spotify Connect");
        spotifyField.type = AdapterConfigFieldType::Boolean;
        spotifyField.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        spotifyField.defaultValue = true;
        spotifyField.parentActionId = QStringLiteral("settings");
        addFieldByLegacyScope(schema, spotifyField);
    }

    if (supportsTranscoder) {
        AdapterConfigField transcoderField;
        transcoderField.key = QStringLiteral("supportsTranscoder");
        transcoderField.label = QStringLiteral("Transcoder");
        transcoderField.type = AdapterConfigFieldType::Boolean;
        transcoderField.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        transcoderField.defaultValue = true;
        transcoderField.parentActionId = QStringLiteral("settings");
        addFieldByLegacyScope(schema, transcoderField);
    }

    return schema;
}

ActionResponse OnkyoAdapterFactory::invokeFactoryAction(const QString &actionId,
                                                        Adapter &infoInOut,
                                                        const QJsonObject &params) const
{
    ActionResponse resp;
    QString errorString;
    Q_UNUSED(params);
    if (actionId != QLatin1String("probe")) {
        resp.status = CmdStatus::NotImplemented;
        resp.error = QStringLiteral("Unsupported action");
        return resp;
    }

    QString host = infoInOut.host.trimmed();
    if (host.isEmpty())
        host = infoInOut.ip.trimmed();
    if (host.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("Host is required");
        return resp;
    }

    const int portValue = infoInOut.port > 0
        ? static_cast<int>(infoInOut.port)
        : 0;
    const quint16 port = portValue > 0 ? static_cast<quint16>(portValue) : 60128;

    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(1500)) {
        errorString = QStringLiteral("Connection failed: %1").arg(socket.errorString());
        resp.status = CmdStatus::Failure;
        resp.error = errorString;
        return resp;
    }

    const QByteArray frame = buildEiscpFrame(QByteArrayLiteral("PWRQSTN"));
    socket.write(frame);
    socket.flush();
    if (!socket.waitForReadyRead(1500)) {
        errorString = QStringLiteral("No response from receiver");
        socket.disconnectFromHost();
        resp.status = CmdStatus::Failure;
        resp.error = errorString;
        return resp;
    }
    QByteArray data = socket.readAll();
    while (socket.waitForReadyRead(50)) {
        data.append(socket.readAll());
    }
    socket.disconnectFromHost();
    if (data.isEmpty()) {
        errorString = QStringLiteral("Empty response from receiver");
        resp.status = CmdStatus::Failure;
        resp.error = errorString;
        return resp;
    }
    if (!data.contains("PWR")) {
        errorString = QStringLiteral("Unexpected response from receiver");
        resp.status = CmdStatus::Failure;
        resp.error = errorString;
        return resp;
    }
    resp.status = CmdStatus::Success;
    return resp;
}

AdapterInterface *OnkyoAdapterFactory::create(QObject *parent)
{
    return new OnkyoAdapter(parent);
}

} // namespace phicore::adapter
