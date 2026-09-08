#include "onkyo_frames.h"

#include <algorithm>

#include "phi/runtime/str.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;

namespace {

constexpr std::size_t kHeaderSize = 16;
constexpr char kMagic[] = "ISCP";

std::string terminator()
{
    return kUseCrlf ? std::string("\r\n") : std::string("\r");
}

std::uint32_t readBigEndian32(const char *bytes)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes);
    return (static_cast<std::uint32_t>(p[0]) << 24)
        | (static_cast<std::uint32_t>(p[1]) << 16)
        | (static_cast<std::uint32_t>(p[2]) << 8)
        | static_cast<std::uint32_t>(p[3]);
}

void appendBigEndian32(std::string &out, std::uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

/// Trims, then drops trailing control bytes: an eISCP payload ends with the
/// unit's end-of-message character before its carriage return.
std::string sanitizeLine(std::string_view raw)
{
    std::string line = str::trimmed(raw);
    while (!line.empty()) {
        const auto last = static_cast<unsigned char>(line.back());
        if (last < 0x20 || last == 0x7F)
            line.pop_back();
        else
            break;
    }
    return line;
}

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

/// Two upper-case hex digits, which is every value a receiver puts in an SLI or
/// MVL response.
bool isHexPair(std::string_view s)
{
    return s.size() == 2 && isHexDigit(s[0]) && isHexDigit(s[1]);
}

} // namespace

std::string encodeEiscpFrame(std::string_view command)
{
    const std::string payload = "!1" + std::string(command) + terminator();

    std::string frame;
    frame.reserve(kHeaderSize + payload.size());
    frame.append(kMagic, 4);
    appendBigEndian32(frame, static_cast<std::uint32_t>(kHeaderSize));
    appendBigEndian32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.push_back(char(1));
    frame.append(3, '\0');
    frame.append(payload);
    return frame;
}

std::string encodePlainFrame(std::string_view command)
{
    return "!1" + std::string(command) + terminator();
}

FrameScan decodeEiscpFrames(std::string_view data)
{
    FrameScan scan;
    std::size_t pos = 0;

    while (pos + kHeaderSize <= data.size()) {
        const std::size_t magic = data.find(kMagic, pos, 4);
        if (magic == std::string_view::npos) {
            // Nothing recognisable left. Keep the last three bytes: they could
            // be the beginning of a magic split across two reads.
            scan.consumed = data.size() >= 3 ? data.size() - 3 : 0;
            return scan;
        }
        if (magic + kHeaderSize > data.size()) {
            scan.consumed = magic;
            return scan;
        }

        const std::uint32_t declaredHeader = readBigEndian32(data.data() + magic + 4);
        const std::uint32_t declaredData = readBigEndian32(data.data() + magic + 8);

        // Everything the header has to be for the frame after it to be findable.
        // Checked in 64 bits so a length near the top of the range is rejected
        // rather than wrapped into something that looks reasonable.
        const std::uint64_t total =
            static_cast<std::uint64_t>(declaredHeader) + static_cast<std::uint64_t>(declaredData);
        if (declaredHeader < kHeaderSize || total > kMaxFrameSize) {
            scan.consumed = magic;
            scan.corrupt = true;
            return scan;
        }
        if (magic + total > data.size()) {
            scan.consumed = magic;
            return scan;
        }

        scan.payloads.emplace_back(data.substr(magic + declaredHeader, declaredData));
        // `total` is at least kHeaderSize, so the cursor always moves.
        pos = magic + static_cast<std::size_t>(total);
        scan.consumed = pos;
    }

    return scan;
}

std::vector<Status> parseStatuses(std::string_view payload)
{
    std::vector<Status> statuses;
    for (const std::string &part : str::split(payload, '\r', false)) {
        std::string line = sanitizeLine(part);
        if (line.rfind("!1", 0) == 0)
            line = sanitizeLine(std::string_view(line).substr(2));
        if (line.size() < 3)
            continue;

        const std::string prefix = line.substr(0, 3);
        const std::string value = line.substr(3);

        if (prefix == "PWR" || prefix == "AMT") {
            if (value != "00" && value != "01")
                continue;
            Status status;
            status.kind = prefix == "PWR" ? StatusKind::Power : StatusKind::Mute;
            status.flag = value == "01";
            statuses.push_back(std::move(status));
            continue;
        }

        if (prefix == "MVL") {
            // Receivers also answer MVLUP/MVLDOWN/MVLN/A; only a level is a level.
            if (!isHexPair(value))
                continue;
            Status status;
            status.kind = StatusKind::Volume;
            status.raw = str::toInt(value, nullptr, 16);
            statuses.push_back(std::move(status));
            continue;
        }

        if (prefix == "SLI") {
            const std::string code = str::toUpper(str::trimmed(value));
            if (!isHexPair(code))
                continue;
            Status status;
            status.kind = StatusKind::Input;
            status.code = code;
            statuses.push_back(std::move(status));
        }
    }
    return statuses;
}

} // namespace phicore::onkyo::ipc
