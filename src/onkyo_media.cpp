#include "onkyo_media.h"

namespace phicore::onkyo::ipc {

namespace v1 = phicore::adapter::v1;

std::optional<std::string_view> mediaPlayerCommand(v1::MediaPlayerAction action) noexcept
{
    switch (action) {
    case v1::MediaPlayerAction::Play:
        return "NTCPLAY";
    case v1::MediaPlayerAction::Pause:
        return "NTCPAUSE";
    case v1::MediaPlayerAction::Stop:
        return "NTCSTOP";
    case v1::MediaPlayerAction::Next:
        return "NTCTRUP";
    case v1::MediaPlayerAction::Previous:
        return "NTCTRDN";
    case v1::MediaPlayerAction::Unknown:
    case v1::MediaPlayerAction::SeekTo:
    case v1::MediaPlayerAction::SeekBy:
    case v1::MediaPlayerAction::PlayMedia:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace phicore::onkyo::ipc
