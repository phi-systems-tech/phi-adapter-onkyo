#include "onkyo_schema.h"

#include <QJsonArray>
#include <QJsonValue>

#include "onkyo_protocol.h"
#include "onkyo_runtime_convert.h"

namespace phicore::onkyo::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

constexpr const char kOnkyoIconSvg[] =
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
    "xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Receiver icon\">"
    "<rect x=\"3\" y=\"6\" width=\"18\" height=\"12\" rx=\"2.5\" "
    "stroke=\"#2E3A4F\" stroke-width=\"1.6\" fill=\"#121A26\"/>"
    "<circle cx=\"8\" cy=\"12\" r=\"2.2\" stroke=\"#7A8AA4\" stroke-width=\"1.4\" fill=\"none\"/>"
    "<rect x=\"13\" y=\"10.2\" width=\"7\" height=\"1.6\" rx=\"0.8\" fill=\"#7A8AA4\"/>"
    "<rect x=\"13\" y=\"13\" width=\"5\" height=\"1.6\" rx=\"0.8\" fill=\"#7A8AA4\"/>"
    "</svg>";

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

} // namespace

v1::Utf8String displayName()
{
    return "Onkyo / Pioneer";
}

v1::Utf8String description()
{
    return "Onkyo/Pioneer AVR adapter (ISCP sidecar)";
}

v1::Utf8String iconSvg()
{
    return kOnkyoIconSvg;
}

v1::AdapterCapabilities capabilities()
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

v1::JsonText configSchemaJson(const QHash<QString, QString> &labels)
{
    return toJson(buildConfigSchemaObject(labels));
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

} // namespace phicore::onkyo::ipc
