#include "onkyo_runtime_convert.h"

#include <type_traits>
#include <variant>

#include <QJsonDocument>

namespace phicore::onkyo::ipc {

namespace v1 = phicore::adapter::v1;

std::string toJson(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QJsonObject parseJsonObject(const std::string &text)
{
    if (text.empty())
        return {};
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

std::optional<double> scalarToDouble(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<double>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*v);
    if (const auto *v = std::get_if<bool>(&value))
        return *v ? 1.0 : 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        bool ok = false;
        const double parsed = QString::fromStdString(*v).toDouble(&ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

QString scalarToDebugString(const v1::ScalarValue &value)
{
    return std::visit(
        [](const auto &entry) -> QString {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return QStringLiteral("null");
            } else if constexpr (std::is_same_v<T, bool>) {
                return entry ? QStringLiteral("true") : QStringLiteral("false");
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return QString::number(entry);
            } else if constexpr (std::is_same_v<T, double>) {
                return QString::number(entry, 'f', 6);
            } else {
                return QString::fromStdString(entry);
            }
        },
        value);
}

std::optional<bool> scalarToBool(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<bool>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return *v != 0;
    if (const auto *v = std::get_if<double>(&value))
        return *v != 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        const QString text = QString::fromStdString(*v).trimmed().toLower();
        if (text == QLatin1String("true") || text == QLatin1String("1") || text == QLatin1String("on"))
            return true;
        if (text == QLatin1String("false") || text == QLatin1String("0") || text == QLatin1String("off"))
            return false;
    }
    return std::nullopt;
}

QString scalarToQString(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<v1::Utf8String>(&value))
        return QString::fromStdString(*v);
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return QString::number(*v);
    if (const auto *v = std::get_if<double>(&value))
        return QString::number(*v, 'g', 12);
    if (const auto *v = std::get_if<bool>(&value))
        return *v ? QStringLiteral("1") : QStringLiteral("0");
    return {};
}

} // namespace phicore::onkyo::ipc
