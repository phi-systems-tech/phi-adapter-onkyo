#include "onkyo_protocol.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QRegularExpression>
#include <QSet>
#include <Qt>

namespace phicore::onkyo::ipc {

QHash<QString, QString> loadConfiguredSliLabels(const QJsonObject &staticConfig)
{
    QHash<QString, QString> map;
    QJsonObject labels = staticConfig.value(QStringLiteral("sliLabels")).toObject();
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        const QString code = normalizeSliCode(it.key());
        if (code.isEmpty())
            continue;
        const QString label = it.value().toString().trimmed();
        if (label.isEmpty())
            continue;
        map.insert(code, label);
    }
    return map;
}

QString normalizeSliCode(QString raw)
{
    QString code = raw.trimmed().toUpper();
    if (code.startsWith(QLatin1String("SLI"), Qt::CaseInsensitive))
        code = code.mid(3).trimmed();
    code.remove(QLatin1Char(' '));
    if (code.isEmpty())
        return {};

    bool numeric = false;
    const int parsed = code.toInt(&numeric, 10);
    if (numeric) {
        code = QString::number(parsed);
        if (code.size() == 1)
            code.prepend(QLatin1Char('0'));
        return code;
    }

    if (code.size() == 1)
        code.prepend(QLatin1Char('0'));
    return code;
}

QString formatSliDisplayLabel(const QString &code, const QString &mappedLabel)
{
    const QString normalizedCode = normalizeSliCode(code);
    const QString label = mappedLabel.trimmed();
    if (!label.isEmpty())
        return label;
    if (normalizedCode.isEmpty())
        return QString();
    return QStringLiteral("SLI %1").arg(normalizedCode);
}

QJsonArray normalizeActiveSliCodesArray(const QJsonValue &value)
{
    QJsonArray normalized;
    QSet<QString> seen;
    auto appendCode = [&normalized, &seen](const QString &rawCode) {
        const QString code = normalizeSliCode(rawCode);
        if (code.isEmpty() || seen.contains(code))
            return;
        seen.insert(code);
        normalized.append(code);
    };

    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &entry : arr) {
            if (entry.isString()) {
                appendCode(entry.toString());
                continue;
            }
            if (entry.isDouble()) {
                appendCode(QString::number(entry.toInt()));
                continue;
            }
        }
        return normalized;
    }

    if (value.isString())
        appendCode(value.toString());
    else if (value.isDouble())
        appendCode(QString::number(value.toInt()));

    return normalized;
}

QString inferModelFromIdentifier(const QString &raw)
{
    QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return {};

    const int portIndex = trimmed.lastIndexOf(QLatin1Char(':'));
    if (portIndex > 0)
        trimmed = trimmed.left(portIndex);

    if (trimmed.endsWith(QStringLiteral(".local"), Qt::CaseInsensitive))
        trimmed.chop(6);

    const QRegularExpression pattern(
        QStringLiteral("^(?:Pioneer|Onkyo)[-_ ]?(.+?)(?:-[0-9A-F]{4,12})?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(trimmed);
    if (!match.hasMatch())
        return {};

    const QString model = match.captured(1).trimmed();
    if (!model.isEmpty() && model.contains(QRegularExpression(QStringLiteral("\\d"))))
        return model;
    return {};
}

QByteArray buildEiscpFrame(const QByteArray &command)
{
    const QByteArray terminator = kUseCrlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\r");
    const QByteArray payload = QByteArrayLiteral("!1") + command + terminator;
    const quint32 dataSize = static_cast<quint32>(payload.size());

    QByteArray frame;
    frame.append("ISCP", 4);
    auto appendInt = [&frame](quint32 value) {
        frame.append(static_cast<char>((value >> 24) & 0xFF));
        frame.append(static_cast<char>((value >> 16) & 0xFF));
        frame.append(static_cast<char>((value >> 8) & 0xFF));
        frame.append(static_cast<char>(value & 0xFF));
    };
    appendInt(16);
    appendInt(dataSize);
    frame.append(char(1));
    frame.append(QByteArray(3, '\0'));
    frame.append(payload);
    return frame;
}

std::uint16_t normalizedPort(int value)
{
    if (value <= 0 || value > 65535)
        return 0;
    return static_cast<std::uint16_t>(value);
}

std::uint16_t resolvedControlPort(std::uint16_t adapterPortValue)
{
    const std::uint16_t adapterPort = normalizedPort(static_cast<int>(adapterPortValue));
    return adapterPort == 0 ? static_cast<std::uint16_t>(60128) : adapterPort;
}

QByteArray buildPlainFrame(const QByteArray &command)
{
    const QByteArray terminator = kUseCrlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\r");
    return QByteArrayLiteral("!1") + command + terminator;
}

} // namespace phicore::onkyo::ipc
