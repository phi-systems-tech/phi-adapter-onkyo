#pragma once

// The per-receiver runtime: what phi-core asks for, turned into exchanges with
// a receiver and answers back. The class itself stays private to the .cpp -
// the factory only ever needs to create one.

#include <map>
#include <memory>
#include <string>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::onkyo::ipc {

/**
 * @brief Create one receiver instance.
 *
 * @param bootstrapInputLabels SLI code -> label map the factory read from its
 *        static config, used until the instance's own meta supplies labels.
 */
std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance(
    std::map<std::string, std::string> bootstrapInputLabels);

} // namespace phicore::onkyo::ipc
