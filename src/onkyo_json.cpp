#include "onkyo_json.h"

#include "phi/runtime/str.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;

Json parseObject(std::string_view text)
{
    if (text.empty())
        return Json::object();
    const Json parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return Json::object();
    return parsed;
}

std::string dump(const Json &value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string jsonString(const Json &obj, std::string_view key)
{
    if (!obj.is_object())
        return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

int jsonInt(const Json &obj, std::string_view key, int fallback)
{
    if (!obj.is_object())
        return fallback;
    const auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number_integer())
        return it->get<int>();
    if (it->is_number_float())
        return static_cast<int>(it->get<double>());
    if (it->is_string()) {
        bool ok = false;
        const int parsed = str::toInt(str::trimmed(it->get<std::string>()), &ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

bool jsonBool(const Json &obj, std::string_view key)
{
    if (!obj.is_object())
        return false;
    const auto it = obj.find(key);
    return it != obj.end() && it->is_boolean() && it->get<bool>();
}

} // namespace phicore::onkyo::ipc
