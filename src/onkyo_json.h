#pragma once

// JSON for this adapter: nlohmann, plus the handful of accessors QJsonObject
// used to provide. The same choice phi-core made when it left Qt (core/jsonx.h),
// so a value read here reads the way it reads there.
//
// Two differences from Qt JSON that matter at call sites:
//  - A default-constructed Json is null, not an empty object. Json::object()
//    is what "{}" means.
//  - There is no "undefined". A missing key reads as null; code that needs to
//    tell "absent" from "null" asks contains() first.

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace phicore::onkyo::ipc {

using Json = nlohmann::json;

/// Parses an object. Empty, malformed or non-object text all give `{}` - the
/// caller has nothing to do differently for any of them.
Json parseObject(std::string_view text);

/// Compact UTF-8. Invalid UTF-8 is replaced rather than thrown on: payloads
/// assembled from receiver bytes must not be able to abort the serializer.
std::string dump(const Json &value);

/// Empty unless the key holds a string.
std::string jsonString(const Json &obj, std::string_view key);

/**
 * @brief Integer at `key`, or `fallback`.
 *
 * Accepts a numeric string as well as a number. Qt's toInt() did not, which
 * meant a port stored as "60128" by a form silently read as 0 and the adapter
 * fell back to the discovered port without saying so.
 */
int jsonInt(const Json &obj, std::string_view key, int fallback);

/// True only for a JSON true; anything else, missing included, is false.
bool jsonBool(const Json &obj, std::string_view key);

} // namespace phicore::onkyo::ipc
