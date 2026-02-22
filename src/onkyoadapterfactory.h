#pragma once

#include "adapterfactory.h"

namespace phicore::adapter {

class OnkyoAdapterFactory : public AdapterFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PHI_ADAPTER_FACTORY_IID)
    Q_INTERFACES(phicore::adapter::AdapterFactory)

public:
    explicit OnkyoAdapterFactory(QObject *parent = nullptr)
        : AdapterFactory(parent)
    {
    }

    QString pluginType() const override { return QStringLiteral("onkyo-pioneer"); }
    QString displayName() const override { return QStringLiteral("Onkyo / Pioneer"); }
    QString apiVersion() const override { return QStringLiteral("1.0.0"); }
    QString description() const override {
        return QStringLiteral("Discover Onkyo/Pioneer AV receivers via mDNS and control them over ISCP.");
    }
    QString loggingCategory() const override { return QStringLiteral("phi-core.adapters.onkyo"); }
    QByteArray icon() const override;

    AdapterCapabilities capabilities() const override;
    discovery::DiscoveryQueryList discoveryQueries() const override;
    AdapterConfigSchema configSchema(const Adapter &info) const override;
    ActionResponse invokeFactoryAction(const QString &actionId,
                                       Adapter &infoInOut,
                                       const QJsonObject &params) const override;
    AdapterInterface *create(QObject *parent = nullptr) override;
};

} // namespace phicore::adapter
