#pragma once

// The eISCP wire, both directions.
//
// Encoding lived in onkyo_protocol; decoding lived as a private method inside
// the instance, where nothing could reach it. That is why the scanner could
// stand still on a frame whose declared size was zero and take the instance
// thread with it: there was no way to hand it a bad frame and see what it did.
// Both halves are here now, pure over bytes, with no adapter state behind them.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace phicore::onkyo::ipc {

/// Frame commands as eISCP (`ISCP` header + payload) rather than plain `!1...`.
inline constexpr bool kUseEiscp = true;
/// Terminate a command with CRLF instead of a bare CR.
inline constexpr bool kUseCrlf = false;

/// eISCP frame: `ISCP` magic, 16-byte header, then the `!1<command>` payload.
std::string encodeEiscpFrame(std::string_view command);

/// Unframed command as some receivers accept it directly on the socket.
std::string encodePlainFrame(std::string_view command);

/**
 * @brief What one pass over a receiver's bytes found.
 *
 * `consumed` is how much of `data` is finished with - noise before a header and
 * every frame returned. What is left is either nothing or the beginning of a
 * frame whose remainder has not arrived; hold it and prepend it to the next
 * read.
 */
struct FrameScan {
    std::vector<std::string> payloads;
    std::size_t consumed = 0;
    /**
     * @brief A header that cannot be believed: the scan stopped at it.
     *
     * Nothing after it can be located, because the only way past a frame is
     * over its declared length. A caller that sees this should drop the
     * connection rather than keep reading a stream it has lost its place in.
     */
    bool corrupt = false;
};

/// Largest frame taken seriously. A status frame is tens of bytes; the cap is
/// only here so a bad length cannot become an allocation.
inline constexpr std::size_t kMaxFrameSize = 64 * 1024;

/**
 * @brief Every complete eISCP payload at the front of `data`.
 *
 * The scan always moves forward: a frame is at least its 16-byte header, so a
 * declared length can never leave the cursor where it was. A length that is
 * impossible (a header shorter than the header, a frame past the cap) is
 * reported through `corrupt` instead of being stepped over, because stepping
 * over it means guessing where the next one starts.
 */
FrameScan decodeEiscpFrames(std::string_view data);

enum class StatusKind {
    Power,
    Mute,
    Volume,
    Input,
};

struct Status {
    StatusKind kind = StatusKind::Power;
    /// Power and Mute: on/muted.
    bool flag = false;
    /// Volume: the receiver's raw value, already parsed from hex.
    int raw = 0;
    /// Input: two upper-case hex digits.
    std::string code;
};

/**
 * @brief The statuses one payload reports.
 *
 * Unrecognised lines are dropped rather than reported: a receiver volunteers
 * status for everything it feels like, and this adapter speaks for four
 * channels.
 */
std::vector<Status> parseStatuses(std::string_view payload);

} // namespace phicore::onkyo::ipc
