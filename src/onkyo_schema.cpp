#include "onkyo_schema.h"

#include <string>
#include <vector>

#include <cstdio>

#include "phi/runtime/str.h"

#include "onkyo_selector.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;
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

/**
 * @brief One choice per selector code the receiver could possibly report.
 *
 * All 256 of them, and a label field per code below, whether or not anything
 * is known about it. That is deliberate - a user with an input this adapter
 * has never heard of can still select it and name it - but it does mean the
 * bootstrap schema is a few hundred fields wide for a receiver with six
 * inputs. `configSchemaSize()` exists so the cost is a number rather than an
 * impression.
 */
Json schemaInputChoices(const std::map<std::string, std::string> &labels)
{
    Json choices = Json::array();
    for (int code = 0; code <= 0xFF; ++code) {
        char buffer[3];
        std::snprintf(buffer, sizeof(buffer), "%02X", code);
        const std::string key(buffer);
        const auto known = labels.find(key);
        choices.push_back(Json{
            {"value", key},
            {"label", formatSliDisplayLabel(key, known == labels.end() ? std::string()
                                                                       : known->second)},
        });
    }
    return choices;
}

struct FieldSpec {
    std::string key;
    std::string type;
    std::string label;
    Json defaultValue = Json();
    std::string actionId;
    std::string actionLabel;
    std::string parentActionId;
    Json flags = Json::array();
    Json choices = Json();
    Json meta = Json::object();
    Json layout = Json::object();
};

Json field(const FieldSpec &spec)
{
    Json obj = Json::object();
    obj["key"] = spec.key;
    obj["type"] = spec.type;
    obj["label"] = spec.label;
    if (!spec.defaultValue.is_null())
        obj["default"] = spec.defaultValue;
    if (!spec.actionId.empty())
        obj["actionId"] = spec.actionId;
    if (!spec.actionLabel.empty())
        obj["actionLabel"] = spec.actionLabel;
    if (!spec.parentActionId.empty())
        obj["parentActionId"] = spec.parentActionId;
    if (!spec.flags.empty())
        obj["flags"] = spec.flags;
    if (spec.choices.is_array())
        obj["choices"] = spec.choices;
    if (!spec.meta.empty())
        obj["meta"] = spec.meta;
    if (!spec.layout.empty())
        obj["layout"] = spec.layout;
    return obj;
}

Json action(const std::string &id, const std::string &label, const std::string &description,
            bool hasForm, Json meta = Json::object())
{
    Json obj = Json::object();
    obj["id"] = id;
    obj["label"] = label;
    obj["description"] = description;
    obj["hasForm"] = hasForm;
    if (!meta.empty())
        obj["meta"] = meta;
    return obj;
}

Json buildConfigSchemaObject(const std::map<std::string, std::string> &labels)
{
    Json factoryFields = Json::array();
    factoryFields.push_back(field({.key = "host", .type = "Hostname", .label = "Host"}));
    factoryFields.push_back(field({.key = "iscpPort", .type = "Port", .label = "ISCP Port",
                                   .defaultValue = 60128}));
    factoryFields.push_back(field({.key = "pollIntervalMs", .type = "Integer",
                                   .label = "Poll interval", .defaultValue = 5000}));
    factoryFields.push_back(field({.key = "retryIntervalMs", .type = "Integer",
                                   .label = "Retry interval", .defaultValue = 10000}));

    const Json inputChoices = schemaInputChoices(labels);

    Json instanceFields = Json::array();
    instanceFields.push_back(field({.key = "volumeMaxRaw", .type = "Integer",
                                    .label = "Max volume raw", .defaultValue = 160,
                                    .parentActionId = "settings",
                                    .flags = Json::array({"InstanceOnly"})}));
    instanceFields.push_back(field({.key = "activeSliCodes", .type = "Select",
                                    .label = "Active SLI codes",
                                    .defaultValue = Json::array(),
                                    .parentActionId = "settings",
                                    .flags = Json::array({"Multi", "InstanceOnly"}),
                                    .choices = inputChoices,
                                    .meta = Json{{"reloadActionLayoutOnChange", true}}}));
    instanceFields.push_back(field({.key = "currentInputCode", .type = "String",
                                    .label = "Current input (SLI)",
                                    .actionId = "probeCurrentInput",
                                    .actionLabel = "Probe current",
                                    .parentActionId = "settings",
                                    .flags = Json::array({"ReadOnly", "Transient",
                                                          "InstanceOnly"}),
                                    .meta = Json{{"appendTo", "activeSliCodes"}},
                                    .layout = Json{{"actionPosition", "inline"}}}));

    for (const Json &choice : inputChoices) {
        if (!choice.is_object())
            continue;
        const std::string key = normalizeSliCode(jsonString(choice, "value"));
        if (key.empty())
            continue;
        const auto known = labels.find(key);
        Json mappingField = field({.key = "inputLabel_" + key,
                                   .type = "String",
                                   .label = "SLI " + key + " label",
                                   .defaultValue = known == labels.end() ? Json()
                                                                         : Json(known->second),
                                   .parentActionId = "settings",
                                   .flags = Json::array({"InstanceOnly"})});
        mappingField["visibility"] = Json{
            {"fieldKey", "activeSliCodes"},
            {"op", "contains"},
            {"value", key},
        };
        instanceFields.push_back(std::move(mappingField));
    }

    Json factorySection = Json::object();
    factorySection["title"] = "Connection";
    factorySection["fields"] = factoryFields;
    factorySection["actions"] = Json::array({
        action("probe", "Test connection",
               "Validate receiver reachability using the current config values.", false,
               Json{{"placement", "card"}, {"kind", "command"}, {"requiresAck", true}}),
    });

    Json instanceSection = Json::object();
    instanceSection["title"] = "Settings";
    instanceSection["fields"] = instanceFields;
    instanceSection["actions"] = Json::array({
        action("settings", "Settings", "Update adapter settings.", true,
               Json{{"placement", "card"}, {"kind", "open_dialog"}, {"requiresAck", true}}),
        action("probeCurrentInput", "Probe current", "Probe current input and refresh labels.",
               false,
               Json{{"placement", "form_field"}, {"kind", "command"}, {"requiresAck", true}}),
    });

    Json schema = Json::object();
    schema["factory"] = factorySection;
    schema["instance"] = instanceSection;
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

v1::JsonText configSchemaJson(const std::map<std::string, std::string> &labels)
{
    return dump(buildConfigSchemaObject(labels));
}

Json optionsToChoiceJson(const v1::AdapterConfigOptionList &options)
{
    Json out = Json::array();
    for (const v1::AdapterConfigOption &option : options)
        out.push_back(Json{{"value", option.value}, {"label", option.label}});
    return out;
}

} // namespace phicore::onkyo::ipc
