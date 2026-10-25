#include "onkyo_schema.h"

#include <string>
#include <utility>
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
 * inputs.
 */
v1::AdapterConfigOptionList schemaInputChoices(const std::map<std::string, std::string> &labels)
{
    v1::AdapterConfigOptionList choices;
    for (int code = 0; code <= 0xFF; ++code) {
        char buffer[3];
        std::snprintf(buffer, sizeof(buffer), "%02X", code);
        const std::string key(buffer);
        const auto known = labels.find(key);
        choices.emplace_back(key, formatSliDisplayLabel(key, known == labels.end() ? std::string() : known->second));
    }
    return choices;
}

v1::AdapterConfigField field(const std::string &key, v1::AdapterConfigFieldType type, const std::string &label,
                             v1::ScalarValue defaultValue = {})
{
    v1::AdapterConfigField out(key, type, label);
    out.setDefaultValue(std::move(defaultValue));
    return out;
}

v1::AdapterConfigSchema buildConfigSchema(const std::map<std::string, std::string> &labels)
{
    using Type = v1::AdapterConfigFieldType;
    using Flag = v1::AdapterConfigFieldFlag;
    v1::AdapterConfigSchema schema;

    v1::AdapterConfigSection &factory = schema.factory();
    factory.setTitle("Connection");
    factory.layout().setColumns(2);
    v1::AdapterConfigField port = field("iscpPort", Type::Port, "ISCP Port", std::int64_t{60128});
    port.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField poll = field("pollIntervalMs", Type::Integer, "Poll interval", std::int64_t{5000});
    poll.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField retry = field("retryIntervalMs", Type::Integer, "Retry interval", std::int64_t{10000});
    retry.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    factory.setFields({field("host", Type::Hostname, "Host"), port, poll, retry});

    // The settings dialog is two columns: the selection and the probe across
    // both, the label of every selected input one cell each.
    v1::AdapterConfigSection &instance = schema.instance();
    instance.setTitle("Settings");
    auto settingsField = [](v1::AdapterConfigField value, v1::AdapterConfigFieldFlags flags) {
        value.setParentActionId("settings").setFlags(flags);
        return value;
    };

    v1::AdapterConfigField volume =
        settingsField(field("volumeMaxRaw", Type::Integer, "Max volume raw", std::int64_t{160}), Flag::InstanceOnly);
    volume.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField active =
        settingsField(field("activeSliCodes", Type::Select, "Active SLI codes"), Flag::Multi | Flag::InstanceOnly);
    active.setOptions(schemaInputChoices(labels)).setReloadsForm(true);
    active.layout().setCells(2).setNewRow(true);
    v1::AdapterConfigField current = settingsField(field("currentInputCode", Type::String, "Current input (SLI)"),
                                                   Flag::ReadOnly | Flag::Transient | Flag::InstanceOnly);
    current.addAction("probeCurrentInput", "Probe current").setAppendTo("activeSliCodes");
    current.layout().setCells(2);
    instance.setFields({volume, active, current});

    for (const v1::AdapterConfigOption &choice : active.options()) {
        const std::string key = normalizeSliCode(choice.value());
        if (key.empty())
            continue;
        const auto known = labels.find(key);
        v1::AdapterConfigField mapping = settingsField(
            field("inputLabel_" + key, Type::String, "SLI " + key + " label",
                  known == labels.end() ? v1::ScalarValue() : v1::ScalarValue(known->second)),
            Flag::InstanceOnly);
        mapping.setVisibility({"activeSliCodes", key, v1::AdapterConfigVisibilityOp::Contains});
        instance.addField(std::move(mapping));
    }
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
    caps.setRequired(v1::AdapterRequirement::Host | v1::AdapterRequirement::Port)
        .setOptional(v1::AdapterRequirement::UsesRetryInterval)
        .setFlags(v1::AdapterCapability::None);

    v1::AdapterActionDescriptor probe("probe", "Test connection",
                                      "Validate receiver reachability using the current config values.");
    probe.setHasForm(false);
    caps.addFactoryAction(probe);

    v1::AdapterActionDescriptor settings("settings", "Settings", "Update adapter settings.");
    settings.setHasForm(true);
    settings.formLayout().setColumns(2);
    caps.addInstanceAction(settings);

    v1::AdapterActionDescriptor probeCurrent("probeCurrentInput", "Probe current",
                                             "Probe current input and refresh labels.");
    probeCurrent.setHasForm(false).setPlacement(v1::AdapterActionPlacement::Field);
    caps.addInstanceAction(probeCurrent);

    return caps;
}

v1::AdapterConfigSchema configSchema(const std::map<std::string, std::string> &labels)
{
    return buildConfigSchema(labels);
}

Json optionsToChoiceJson(const v1::AdapterConfigOptionList &options)
{
    Json out = Json::array();
    for (const v1::AdapterConfigOption &option : options)
        out.push_back(Json{{"value", option.value()}, {"label", option.label()}});
    return out;
}

} // namespace phicore::onkyo::ipc
