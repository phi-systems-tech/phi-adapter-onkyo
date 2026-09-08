#pragma once

// What phi-core is told this receiver is: one media player, five channels.
// Data, not behaviour - the builders here take a configuration and answer with
// a descriptor, and nothing in this file talks to anything.

#include <string>

#include "phi/adapter/v1/channel.h"
#include "phi/adapter/v1/device.h"
#include "phi/adapter/v1/schema.h"

#include "onkyo_json.h"

namespace phicore::onkyo::ipc {

inline constexpr const char kChannelPower[] = "power";
inline constexpr const char kChannelVolume[] = "volume";
inline constexpr const char kChannelMute[] = "mute";
inline constexpr const char kChannelInput[] = "input";
inline constexpr const char kChannelConnectivity[] = "connectivity";

/// The input channel on its own, for the case where only its choices changed.
phicore::adapter::v1::Channel buildInputChannel(
    const phicore::adapter::v1::AdapterConfigOptionList &choices);

/// All five, in the order phi-core lists them.
phicore::adapter::v1::ChannelList buildChannels(
    const phicore::adapter::v1::AdapterConfigOptionList &inputChoices);

/**
 * @brief The device descriptor.
 *
 * @param deviceId  Adapter-domain id, already resolved.
 * @param info      What phi-core knows about the instance.
 * @param meta      The instance's own settings.
 * @param host      First candidate address, used as a name and as a last
 *                  resort for guessing a model.
 */
phicore::adapter::v1::Device buildDevice(const std::string &deviceId,
                                         const phicore::adapter::v1::Adapter &info,
                                         const Json &meta,
                                         const std::string &host);

/// The device id: the configured uuid, the instance's external id, the host,
/// and finally a constant - in that order, because that is the order of how
/// much each one can be trusted to stay the same.
std::string resolveDeviceId(const phicore::adapter::v1::Adapter &info,
                            const Json &meta,
                            const std::string &host);

} // namespace phicore::onkyo::ipc
