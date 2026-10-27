#pragma once

#include <ai/application/catalog/ProviderCatalogService.hpp>
#include <ai/ui/ModelListModel.hpp>
#include <ai/ui/ProviderListModel.hpp>

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtTypes>

/** Qt command/read-model adapter for provider and model metadata. */
class ProviderCatalogController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* providers READ providers CONSTANT)
    Q_PROPERTY(QAbstractItemModel* models READ models CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit ProviderCatalogController(
        ai::application::ProviderCatalogService& service,
        QObject* parent = nullptr);

    [[nodiscard]] QAbstractItemModel* providers() noexcept;
    [[nodiscard]] QAbstractItemModel* models() noexcept;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE QString createProvider(
        const QString& name,
        const QString& kind,
        const QString& baseUrl);
    Q_INVOKABLE bool updateProvider(
        const QString& providerId,
        const QString& name,
        const QString& kind,
        const QString& baseUrl);
    Q_INVOKABLE bool setProviderEnabled(const QString& providerId, bool enabled);

    // Curated models supported by the provider adapters compiled into this
    // application. QML consumes metadata only; later this implementation can
    // be replaced by a remote catalog without changing the page contract.
    Q_INVOKABLE [[nodiscard]] bool hasBuiltInModelCatalog(const QString& providerKind) const;
    Q_INVOKABLE [[nodiscard]] QVariantList catalogModelsForProvider(
        const QString& providerKind) const;

    // Creates the model of the built-in catalog with that api id for the provider, with EVERY catalog
    // value (flat and cache prices, long-context tier). Returns the new model id, or empty with lastError.
    Q_INVOKABLE QString createCatalogModel(const QString& providerId, const QString& apiModelId);

    Q_INVOKABLE QString createModel(
        const QString& providerId,
        const QString& apiModelId,
        const QString& displayName,
        qulonglong contextWindowTokens,
        qulonglong maxOutputTokens,
        qulonglong inputPriceMicros,
        qulonglong outputPriceMicros);
    // cacheWritePriceMicros/cacheReadPriceMicros are Anthropic prompt-cache
    // rates (MW-P2). Meaningless for a provider/model that never reports
    // cache usage; leave at 0 ("not entered", never "free" - see
    // ai::application::Model's cache price field doc comment).
    Q_INVOKABLE bool updateModelPricing(
        const QString& modelId,
        qulonglong inputPriceMicros,
        qulonglong outputPriceMicros,
        qulonglong cacheWritePriceMicros,
        qulonglong cacheReadPriceMicros);
    // The complete pricing form in ONE atomic Backend update: the flat prices, the cache prices
    // AND the long-context tier. A long-context threshold of 0 together with all long-context
    // prices of 0 is the EXPLICIT removal of the tier (flat pricing); any other combination is
    // validated by the Backend (ProviderCatalogService) and refused with its own typed code and
    // message - QML never re-implements those rules. Nothing is auto-filled: every value is
    // exactly what the caller passes, and a refused save changes nothing. (updateModelPricing()
    // and updateModel() never touch the tier: they leave it as stored.)
    Q_INVOKABLE bool saveModelPricing(
        const QString& modelId,
        qulonglong inputPriceMicros,
        qulonglong outputPriceMicros,
        qulonglong cacheWritePriceMicros,
        qulonglong cacheReadPriceMicros,
        qulonglong longContextThresholdInputTokens,
        qulonglong longContextInputPriceMicros,
        qulonglong longContextOutputPriceMicros,
        qulonglong longContextCacheWritePriceMicros,
        qulonglong longContextCacheReadPriceMicros);
    Q_INVOKABLE bool saveModelPricingWithCacheDuration(
        const QString& modelId,
        qulonglong inputPriceMicros,
        qulonglong outputPriceMicros,
        qulonglong cacheWritePriceMicros,
        qulonglong cacheReadPriceMicros,
        qulonglong cacheWriteOneHourPriceMicros,
        qulonglong longContextThresholdInputTokens,
        qulonglong longContextInputPriceMicros,
        qulonglong longContextOutputPriceMicros,
        qulonglong longContextCacheWritePriceMicros,
        qulonglong longContextCacheReadPriceMicros,
        qulonglong longContextCacheWriteOneHourPriceMicros);
    // Full model edit (apiModelId, display name, context/output token
    // limits, prices) - unlike updateModelPricing() this does not preserve
    // any existing field QML itself owns, so QML must pass the complete,
    // already-prefilled form state. Cache prices are the one exception:
    // ModelFormDialog has no cache-pricing fields of its own, so this always
    // preserves whatever ModelPricingDialog last set rather than silently
    // resetting it to zero on an unrelated metadata edit.
    Q_INVOKABLE bool updateModel(
        const QString& modelId,
        const QString& apiModelId,
        const QString& displayName,
        qulonglong contextWindowTokens,
        qulonglong maxOutputTokens,
        qulonglong inputPriceMicros,
        qulonglong outputPriceMicros);
    Q_INVOKABLE bool setModelEnabled(const QString& modelId, bool enabled);
    Q_INVOKABLE bool deleteModel(const QString& modelId);
    Q_INVOKABLE bool setDefaultModel(const QString& providerId, const QString& modelId);
    Q_INVOKABLE void refresh();

signals:
    // Both read models have been refreshed: consumers must not reload once per modelReset.
    void catalogChanged();
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();

private:
    void reportError(const ai::common::Error& error);
    void clearError();

    ai::application::ProviderCatalogService& service_;
    ProviderListModel providerModel_;
    ModelListModel modelModel_;
    QString lastError_;
};
