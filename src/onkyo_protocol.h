#pragma once

// eISCP wire format and the SLI (input-selector) code vocabulary. Pure
// functions over bytes and strings - no sockets, no adapter state - so the
// protocol can be reasoned about and changed without touching the lifecycle.

#include <cstdint>

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace phicore::onkyo::ipc {

/// Frame commands as eISCP (`ISCP` header + payload) rather than plain `!1...`.
inline constexpr bool kUseEiscp = true;
/// Terminate a command with CRLF instead of a bare CR.
inline constexpr bool kUseCrlf = false;

/// eISCP frame: `ISCP` magic, 16-byte header, then the `!1<command>` payload.
QByteArray buildEiscpFrame(const QByteArray &command);

/// Unframed command as some receivers accept it directly on the socket.
QByteArray buildPlainFrame(const QByteArray &command);

/**
 * @brief Canonical form of an SLI code.
 *
 * Accepts `SLI05`, `sli 5`, `5`, `05`, `TV` and returns `05` / `TV`. Empty when
 * nothing usable remains, which callers treat as "skip this entry".
 */
QString normalizeSliCode(QString raw);

/// Configured label, or a `SLI <code>` fallback; empty for an unusable code.
QString formatSliDisplayLabel(const QString &code, const QString &mappedLabel);

/// Normalizes a scalar or array of SLI codes into a deduplicated array.
QJsonArray normalizeActiveSliCodesArray(const QJsonValue &value);

/// Code -> label map from the factory's `sliLabels` static config object.
QHash<QString, QString> loadConfiguredSliLabels(const QJsonObject &staticConfig);

/// Model name inferred from an mDNS-style identifier, or empty when unclear.
QString inferModelFromIdentifier(const QString &raw);

/// 0 for anything outside 1..65535, which callers read as "not configured".
std::uint16_t normalizedPort(int value);

/// Configured control port, or the ISCP default (60128).
std::uint16_t resolvedControlPort(std::uint16_t adapterPortValue);

} // namespace phicore::onkyo::ipc
