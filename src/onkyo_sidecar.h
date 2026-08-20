#pragma once

// The per-receiver runtime: connection state, the serialized command/poll queue
// and the channel state it emits. The class itself stays private to the .cpp -
// the factory only ever needs to create one - so the receiver lifecycle is not
// part of any other translation unit.

#include <memory>

#include <QHash>
#include <QString>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::onkyo::ipc {

/**
 * @brief Create one receiver instance.
 *
 * @param bootstrapInputLabels SLI code -> label map the factory read from its
 *        static config, used until the instance's own meta supplies labels.
 */
std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance(
    QHash<QString, QString> bootstrapInputLabels);

} // namespace phicore::onkyo::ipc
