// The eISCP wire and the input-selector vocabulary.
//
// `onkyo_protocol.h` says at the top that it is pure functions over bytes and
// strings so the protocol can be changed without touching the lifecycle. This
// is the part that was missing: something that notices when it changes.

#include <phi/adapter/testing/check.h>

#include "onkyo_protocol.h"
#include "onkyo_probe.h"

#include <QCoreApplication>
#include <QJsonDocument>

using namespace phicore::onkyo::ipc;

namespace {

QJsonObject objectOf(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- the frame ---------------------------------------------------------

    // A receiver drops anything it does not recognise without a word, so the
    // header is checked field by field: magic, header size, payload size, and
    // the version byte.
    const QByteArray frame = buildEiscpFrame(QByteArrayLiteral("PWR01"));
    PHI_CHECK_MSG(frame.size() == 16 + 2 + 5 + 1, "unexpected frame size %d", int(frame.size()));
    PHI_CHECK(frame.startsWith(QByteArrayLiteral("ISCP")));
    const auto u32 = [&frame](int at) {
        return (static_cast<quint32>(static_cast<unsigned char>(frame[at])) << 24)
            | (static_cast<quint32>(static_cast<unsigned char>(frame[at + 1])) << 16)
            | (static_cast<quint32>(static_cast<unsigned char>(frame[at + 2])) << 8)
            | static_cast<quint32>(static_cast<unsigned char>(frame[at + 3]));
    };
    PHI_CHECK(u32(4) == 16u);                       // the header is 16 bytes
    PHI_CHECK(u32(8) == 8u);                        // "!1PWR01" plus the terminator
    PHI_CHECK(static_cast<unsigned char>(frame[12]) == 1u);
    PHI_CHECK(frame.endsWith(QByteArrayLiteral("!1PWR01\r")));

    // The unframed form some receivers take directly.
    PHI_CHECK(buildPlainFrame(QByteArrayLiteral("PWR01")) == QByteArrayLiteral("!1PWR01\r"));

    // --- the input selector ------------------------------------------------

    // One input arrives written five ways; all of them are the same input, and
    // a selector that does not agree with itself shows duplicates.
    PHI_CHECK(normalizeSliCode(QStringLiteral("SLI05")) == QStringLiteral("05"));
    PHI_CHECK(normalizeSliCode(QStringLiteral("sli 5")) == QStringLiteral("05"));
    PHI_CHECK(normalizeSliCode(QStringLiteral("5")) == QStringLiteral("05"));
    PHI_CHECK(normalizeSliCode(QStringLiteral("05")) == QStringLiteral("05"));
    PHI_CHECK(normalizeSliCode(QStringLiteral("TV")) == QStringLiteral("TV"));
    PHI_CHECK(normalizeSliCode(QStringLiteral("   ")).isEmpty());

    // A code with no configured label still reads as something.
    PHI_CHECK(formatSliDisplayLabel(QStringLiteral("05"), QStringLiteral("Blu-ray"))
              == QStringLiteral("Blu-ray"));
    PHI_CHECK(formatSliDisplayLabel(QStringLiteral("05"), QString()) == QStringLiteral("SLI 05"));
    // A configured label wins even when the code is unusable - somebody wrote
    // that label on purpose, and there is nothing better to show.
    PHI_CHECK(formatSliDisplayLabel(QString(), QStringLiteral("Blu-ray")) == QStringLiteral("Blu-ray"));
    // With neither, there is nothing to show and nothing is shown.
    PHI_CHECK(formatSliDisplayLabel(QString(), QString()).isEmpty());

    // The configured list is a scalar or an array, and the same input twice is
    // one input.
    const QJsonArray fromArray = normalizeActiveSliCodesArray(
        QJsonDocument::fromJson(R"(["SLI05", "5", "TV"])").array());
    PHI_CHECK_MSG(fromArray.size() == 2, "expected two codes, got %d", int(fromArray.size()));
    const QJsonArray fromScalar = normalizeActiveSliCodesArray(QJsonValue(QStringLiteral("sli 5")));
    PHI_CHECK(fromScalar.size() == 1 && fromScalar.at(0).toString() == QStringLiteral("05"));
    PHI_CHECK(normalizeActiveSliCodesArray(QJsonValue()).isEmpty());

    const QHash<QString, QString> labels = loadConfiguredSliLabels(objectOf(R"({
        "sliLabels": {"SLI05": "Blu-ray", "2": "Game"}
    })"));
    PHI_CHECK(labels.value(QStringLiteral("05")) == QStringLiteral("Blu-ray"));
    PHI_CHECK(labels.value(QStringLiteral("02")) == QStringLiteral("Game"));

    // --- addressing --------------------------------------------------------

    PHI_CHECK(inferModelFromIdentifier(QStringLiteral("Pioneer-SC-LX701-4D2F63.local:60128"))
              .contains(QStringLiteral("SC-LX701")));
    PHI_CHECK(inferModelFromIdentifier(QString()).isEmpty());

    // A port is a port or it is nothing; nothing means "use the default".
    PHI_CHECK(normalizedPort(60128) == 60128);
    PHI_CHECK(normalizedPort(0) == 0);
    PHI_CHECK(normalizedPort(70000) == 0);
    PHI_CHECK(resolvedControlPort(0) == 60128);
    PHI_CHECK(resolvedControlPort(8102) == 8102);

    // The probe takes its candidates from the action's params, address first.
    const QStringList hosts = resolveProbeHosts(objectOf(R"({
        "host": "receiver.local", "ip": "192.168.1.32"
    })"));
    PHI_CHECK_MSG(!hosts.isEmpty(), "the probe has nowhere to connect");
    if (!hosts.isEmpty())
        PHI_CHECK(hosts.first() == QStringLiteral("192.168.1.32"));
    PHI_CHECK(resolveProbePort(objectOf(R"({"iscpPort": 60128})")) == 60128);
    PHI_CHECK(resolveProbePort(objectOf("{}")) == 0);

    return phi::testing::report("onkyo_protocol_tests");
}
