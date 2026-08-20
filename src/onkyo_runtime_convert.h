#pragma once

// Conversions between the Qt-free SDK value types and the Qt types the rest of
// this adapter works in. Everything here is pure: no adapter state, no I/O.

#include <optional>
#include <string>

#include <QJsonObject>
#include <QString>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::onkyo::ipc {

std::string toJson(const QJsonObject &obj);

/// Returns an empty object for empty, malformed or non-object text.
QJsonObject parseJsonObject(const std::string &text);

std::optional<double> scalarToDouble(const phicore::adapter::v1::ScalarValue &value);
std::optional<bool> scalarToBool(const phicore::adapter::v1::ScalarValue &value);
QString scalarToQString(const phicore::adapter::v1::ScalarValue &value);

/// Human-readable rendering for traces; never used to build wire values.
QString scalarToDebugString(const phicore::adapter::v1::ScalarValue &value);

} // namespace phicore::onkyo::ipc
