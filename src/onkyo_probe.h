#pragma once

// Reachability probe for the factory's "Test connection" action. Blocking by
// nature (a TCP connect), which is why the factory runs it on its own execution
// backend rather than on the host poll thread.

#include <cstdint>

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace phicore::onkyo::ipc {

/// Candidate hosts from the action params, IP literals first, deduplicated.
QStringList resolveProbeHosts(const QJsonObject &params);

/// `iscpPort` from the action params; 0 when absent or unusable.
std::uint16_t resolveProbePort(const QJsonObject &params);

/**
 * @brief One blocking connect attempt against a single candidate.
 *
 * Costs up to ~900 ms. Callers walking several candidates must check
 * `stopRequested()` between them so a shutdown is not spent on the rest.
 */
bool probeEndpoint(const QString &host, std::uint16_t port, QString *errorMessage);

} // namespace phicore::onkyo::ipc
