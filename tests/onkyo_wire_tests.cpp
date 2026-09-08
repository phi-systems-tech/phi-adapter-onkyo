// The eISCP wire and the input-selector vocabulary.
//
// `onkyo_frames.h` says at the top that it is pure over bytes so the protocol
// can be changed without touching the lifecycle. This is the part that notices
// when it changes - and, since the decoder moved out of the instance, the part
// that can hand it a frame no receiver would send.

#include <phi/adapter/testing/check.h>

#include "onkyo_endpoint.h"
#include "onkyo_frames.h"
#include "onkyo_json.h"
#include "onkyo_selector.h"

#include <string>

using namespace phicore::onkyo::ipc;

namespace {

std::uint32_t u32(const std::string &s, std::size_t at)
{
    const auto *p = reinterpret_cast<const unsigned char *>(s.data()) + at;
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
        | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::string frameWith(std::uint32_t headerSize, std::uint32_t dataSize, const std::string &payload)
{
    std::string frame = "ISCP";
    const auto append = [&frame](std::uint32_t v) {
        frame.push_back(char((v >> 24) & 0xFF));
        frame.push_back(char((v >> 16) & 0xFF));
        frame.push_back(char((v >> 8) & 0xFF));
        frame.push_back(char(v & 0xFF));
    };
    append(headerSize);
    append(dataSize);
    frame.push_back(char(1));
    frame.append(3, '\0');
    frame.append(payload);
    return frame;
}

void testTheFrameIsBuiltFieldByField()
{
    // A receiver drops anything it does not recognise without a word, so the
    // header is checked field by field: magic, header size, payload size, and
    // the version byte.
    const std::string frame = encodeEiscpFrame("PWR01");
    PHI_CHECK_MSG(frame.size() == 16 + 2 + 5 + 1, "unexpected frame size %d", int(frame.size()));
    PHI_CHECK(frame.rfind("ISCP", 0) == 0);
    PHI_CHECK(u32(frame, 4) == 16u);   // the header is 16 bytes
    PHI_CHECK(u32(frame, 8) == 8u);    // "!1PWR01" plus the terminator
    PHI_CHECK(static_cast<unsigned char>(frame[12]) == 1u);
    PHI_CHECK(frame.size() >= 8 && frame.compare(frame.size() - 8, 8, "!1PWR01\r") == 0);

    // The unframed form some receivers take directly.
    PHI_CHECK(encodePlainFrame("PWR01") == "!1PWR01\r");
}

void testTheScannerAlwaysMovesForward()
{
    // The bug this file exists for. A frame whose declared sizes are zero used
    // to leave the cursor exactly where it was, and the next pass found the
    // same frame: the instance thread went into that loop and never came back,
    // the SDK's stop budget expired, and the host gave up and leaked the
    // instance. One malformed frame on port 60128 was enough.
    const FrameScan zeroed = decodeEiscpFrames(frameWith(0, 0, std::string()));
    PHI_CHECK_MSG(zeroed.corrupt, "a zero-length frame was accepted as a frame");
    PHI_CHECK(zeroed.payloads.empty());

    // The same, one size class up: a length that only looks sane after it has
    // been truncated into an int.
    const FrameScan huge = decodeEiscpFrames(frameWith(16, 0xFFFFFFF0u, std::string()));
    PHI_CHECK_MSG(huge.corrupt, "an impossible payload length was believed");

    // A header shorter than a header cannot say where its payload begins.
    PHI_CHECK(decodeEiscpFrames(frameWith(8, 4, "junk")).corrupt);
}

void testWholeFramesComeOutAndPartialOnesWait()
{
    const std::string first = encodeEiscpFrame("PWR01");
    const std::string second = encodeEiscpFrame("MVL20");

    const FrameScan both = decodeEiscpFrames(first + second);
    PHI_CHECK_MSG(both.payloads.size() == 2, "expected two payloads, got %d",
                  int(both.payloads.size()));
    PHI_CHECK(!both.corrupt);
    PHI_CHECK(both.consumed == first.size() + second.size());

    // A read that stopped in the middle of the second frame keeps it whole for
    // the next one instead of parsing half of it.
    const std::string clipped = first + second.substr(0, second.size() - 3);
    const FrameScan partial = decodeEiscpFrames(clipped);
    PHI_CHECK(partial.payloads.size() == 1);
    PHI_CHECK(!partial.corrupt);
    PHI_CHECK_MSG(partial.consumed == first.size(), "consumed %d of %d",
                  int(partial.consumed), int(clipped.size()));
}

void testTheStatusesOnePayloadReports()
{
    const auto statuses = parseStatuses("!1PWR01\x1a\r!1MVL20\x1a\r!1AMT00\x1a\r!1SLI2E\x1a\r");
    PHI_CHECK_MSG(statuses.size() == 4, "expected four statuses, got %d", int(statuses.size()));
    if (statuses.size() == 4) {
        PHI_CHECK(statuses[0].kind == StatusKind::Power && statuses[0].flag);
        PHI_CHECK(statuses[1].kind == StatusKind::Volume && statuses[1].raw == 0x20);
        PHI_CHECK(statuses[2].kind == StatusKind::Mute && !statuses[2].flag);
        PHI_CHECK(statuses[3].kind == StatusKind::Input && statuses[3].code == "2E");
    }

    // A receiver volunteers plenty this adapter has no channel for, and it
    // answers a level query with words when there is no level.
    PHI_CHECK(parseStatuses("!1NLSU0\x1a\r").empty());
    PHI_CHECK(parseStatuses("!1MVLN/A\x1a\r").empty());
    PHI_CHECK(parseStatuses("!1PWRQSTN\x1a\r").empty());
}

void testOneInputWrittenFiveWaysIsOneInput()
{
    PHI_CHECK(normalizeSliCode("SLI05") == "05");
    PHI_CHECK(normalizeSliCode("sli 5") == "05");
    PHI_CHECK(normalizeSliCode("5") == "05");
    PHI_CHECK(normalizeSliCode("05") == "05");
    PHI_CHECK(normalizeSliCode("2e") == "2E");
    PHI_CHECK(normalizeSliCode("   ").empty());

    // `TV` used to come back as `TV`, and the adapter would send SLITV: a
    // command no receiver answers, whose answer the reader would not have
    // recognised either. A code is two hex digits or it is not a code.
    PHI_CHECK_MSG(normalizeSliCode("TV").empty(), "a non-hex code got through as a code");
    PHI_CHECK(normalizeSliCode("005").empty());

    // A code with no configured label still reads as something.
    PHI_CHECK(formatSliDisplayLabel("05", "Blu-ray") == "Blu-ray");
    PHI_CHECK(formatSliDisplayLabel("05", "") == "SLI 05");
    // A configured label wins even when the code is unusable - somebody wrote
    // that label on purpose, and there is nothing better to show.
    PHI_CHECK(formatSliDisplayLabel("", "Blu-ray") == "Blu-ray");
    // With neither, there is nothing to show and nothing is shown.
    PHI_CHECK(formatSliDisplayLabel("", "").empty());
}

void testTheConfiguredListIsScalarOrArrayAndNeverRepeats()
{
    const auto fromArray = normalizeActiveSliCodes(Json::parse(R"(["SLI05", "5", "TV", "2E"])"));
    PHI_CHECK_MSG(fromArray.size() == 2, "expected two codes, got %d", int(fromArray.size()));
    // Written order, so anything derived from it comes out the same way twice.
    if (fromArray.size() == 2) {
        PHI_CHECK(fromArray[0] == "05");
        PHI_CHECK(fromArray[1] == "2E");
    }

    const auto fromScalar = normalizeActiveSliCodes(Json("sli 5"));
    PHI_CHECK(fromScalar.size() == 1 && fromScalar.front() == "05");
    PHI_CHECK(normalizeActiveSliCodes(Json()).empty());
    PHI_CHECK(normalizeActiveSliCodes(Json(5)).front() == "05");

    const auto labels = loadConfiguredSliLabels(
        parseObject(R"({"sliLabels": {"SLI05": "Blu-ray", "2": "Game", "TV": "nope"}})"));
    PHI_CHECK(labels.at("05") == "Blu-ray");
    PHI_CHECK(labels.at("02") == "Game");
    PHI_CHECK(labels.count("TV") == 0);
}

void testAddressing()
{
    PHI_CHECK(inferModelFromIdentifier("Pioneer-SC-LX701-4D2F63.local:60128") == "SC-LX701");
    PHI_CHECK(inferModelFromIdentifier("Onkyo-TX-NR609").find("TX-NR609") != std::string::npos);
    PHI_CHECK(inferModelFromIdentifier("").empty());
    // A vendor and a word is not a model.
    PHI_CHECK(inferModelFromIdentifier("Onkyo-Receiver").empty());
    PHI_CHECK(inferModelFromIdentifier("some-other-box.local").empty());

    // A port is a port or it is nothing; nothing means "use the default".
    PHI_CHECK(normalizedPort(60128) == 60128);
    PHI_CHECK(normalizedPort(0) == 0);
    PHI_CHECK(normalizedPort(70000) == 0);
    PHI_CHECK(resolvedControlPort(0) == 60128);
    PHI_CHECK(resolvedControlPort(8102) == 8102);

    PHI_CHECK(isIpLiteral("192.168.1.32"));
    PHI_CHECK(isIpLiteral("fe80::1"));
    PHI_CHECK(!isIpLiteral("receiver.local"));

    // The probe takes its candidates from the action's params, address first.
    const auto hosts = resolveProbeHosts(
        parseObject(R"({"host": "receiver.local", "ip": "192.168.1.32"})"));
    PHI_CHECK_MSG(hosts.size() == 2, "the probe has %d candidates", int(hosts.size()));
    if (!hosts.empty())
        PHI_CHECK(hosts.front() == "192.168.1.32");
    PHI_CHECK(resolveProbePort(parseObject(R"({"iscpPort": 60128})")) == 60128);
    PHI_CHECK(resolveProbePort(parseObject("{}")) == 0);

    // A form that stored the port as text used to read as zero, and the adapter
    // quietly fell back to whatever discovery had found.
    PHI_CHECK(resolveProbePort(parseObject(R"({"iscpPort": "60128"})")) == 60128);
}

} // namespace

int main()
{
    testTheFrameIsBuiltFieldByField();
    testTheScannerAlwaysMovesForward();
    testWholeFramesComeOutAndPartialOnesWait();
    testTheStatusesOnePayloadReports();
    testOneInputWrittenFiveWaysIsOneInput();
    testTheConfiguredListIsScalarOrArrayAndNeverRepeats();
    testAddressing();
    return phi::testing::report("onkyo_wire_tests");
}
