#include "onkyo_devicemodel.h"

#include <array>

#include "phi/runtime/str.h"

#include "onkyo_endpoint.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

namespace {

constexpr auto kReadWriteReport =
    v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;

} // namespace

v1::Channel buildInputChannel(const v1::AdapterConfigOptionList &choices)
{
    v1::Channel input;
    input.setExternalId(kChannelInput);
    input.setName("Input");
    input.setKind(v1::ChannelKind::HdmiInput);
    input.setDataType(v1::ChannelDataType::String);
    input.setFlags(kReadWriteReport);
    input.setChoices(choices);
    return input;
}

v1::ChannelList buildChannels(const v1::AdapterConfigOptionList &inputChoices)
{
    v1::ChannelList channels;

    v1::Channel power;
    power.setExternalId(kChannelPower);
    power.setName("Power");
    power.setKind(v1::ChannelKind::PowerOnOff);
    power.setDataType(v1::ChannelDataType::Bool);
    power.setFlags(kReadWriteReport);
    channels.push_back(std::move(power));

    v1::Channel volume;
    volume.setExternalId(kChannelVolume);
    volume.setName("Volume");
    volume.setKind(v1::ChannelKind::Volume);
    volume.setDataType(v1::ChannelDataType::Int);
    volume.setFlags(kReadWriteReport);
    volume.setRange(0.0, 100.0, 1.0);
    channels.push_back(std::move(volume));

    v1::Channel mute;
    mute.setExternalId(kChannelMute);
    mute.setName("Mute");
    mute.setKind(v1::ChannelKind::Mute);
    mute.setDataType(v1::ChannelDataType::Bool);
    mute.setFlags(kReadWriteReport);
    channels.push_back(std::move(mute));

    channels.push_back(buildInputChannel(inputChoices));

    v1::Channel connectivity;
    connectivity.setExternalId(kChannelConnectivity);
    connectivity.setName("Connectivity");
    connectivity.setKind(v1::ChannelKind::ConnectivityStatus);
    connectivity.setDataType(v1::ChannelDataType::Enum);
    connectivity.setFlags(v1::ChannelFlag::Readable | v1::ChannelFlag::Reportable);
    channels.push_back(std::move(connectivity));

    return channels;
}

std::string resolveDeviceId(const v1::Adapter &info, const Json &meta, const std::string &host)
{
    const std::string uuid = str::trimmed(jsonString(meta, "deviceUuid"));
    if (!uuid.empty())
        return uuid;
    if (!info.externalId().empty())
        return info.externalId();
    if (!host.empty())
        return host;
    return "onkyo-pioneer";
}

v1::Device buildDevice(const std::string &deviceId,
                       const v1::Adapter &info,
                       const Json &meta,
                       const std::string &host)
{
    v1::Device device;
    device.setExternalId(deviceId);
    device.setDeviceClass(v1::DeviceClass::MediaPlayer);
    device.setMediaPlayerFeatures(v1::MediaPlayerFeature::Play
        | v1::MediaPlayerFeature::Pause
        | v1::MediaPlayerFeature::Stop
        | v1::MediaPlayerFeature::Next
        | v1::MediaPlayerFeature::Previous);

    const std::string adapterName = str::trimmed(info.name());
    const std::string metaName = str::trimmed(jsonString(meta, "deviceName"));
    if (!adapterName.empty())
        device.setName(adapterName);
    else if (!metaName.empty())
        device.setName(metaName);
    else
        device.setName(host);

    const std::string manufacturer = str::trimmed(jsonString(meta, "manufacturer"));
    device.setManufacturer(manufacturer.empty() ? "Onkyo & Pioneer" : manufacturer);

    std::string model = str::trimmed(jsonString(meta, "model"));
    if (model.empty()) {
        const std::array<std::string, 4> candidates = {
            host,
            jsonString(meta, "deviceUuid"),
            metaName,
            info.name(),
        };
        for (const std::string &candidate : candidates) {
            model = inferModelFromIdentifier(candidate);
            if (!model.empty())
                break;
        }
    }
    device.setModel(model);

    Json deviceMeta = Json::object();
    if (jsonBool(meta, "supportsSpotify"))
        deviceMeta["supportsSpotify"] = true;
    if (jsonBool(meta, "supportsTranscoder"))
        deviceMeta["supportsTranscoder"] = true;
    device.setMetaJson(dump(deviceMeta));

    return device;
}

} // namespace phicore::onkyo::ipc
