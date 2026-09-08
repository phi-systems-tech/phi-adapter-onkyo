#pragma once

// Reachability for the factory's "Test connection" action.
//
// Blocking by nature - it is one TCP connect and nothing else - which is why
// the factory runs it on its own execution backend rather than on the host
// poll thread. It stays blocking rather than borrowing the instance's loop:
// the factory has no loop of its own, and one connect with a deadline needs
// none.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace phicore::onkyo::ipc {

/**
 * @brief One connect attempt against a single candidate.
 *
 * @param cancelled Polled while waiting; a probe walking several candidates
 *        must be able to give the rest up when the sidecar is shutting down.
 * @return whether the receiver accepted a connection.
 */
bool probeEndpoint(const std::string &host,
                   std::uint16_t port,
                   std::chrono::milliseconds timeout,
                   const std::function<bool()> &cancelled,
                   std::string *errorMessage);

} // namespace phicore::onkyo::ipc
