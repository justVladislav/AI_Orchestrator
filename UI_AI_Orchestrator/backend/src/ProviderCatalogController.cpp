#include <ai/ui/ProviderCatalogController.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace {

// One built-in catalog entry: everything a new model row starts with. A price of 0 is "not entered"
// (never "free"), so a provider that reports no cache usage simply keeps 0 for the cache prices.
struct CatalogEntry {
    const char* apiModelId;
    const char* displayName;
    qulonglong contextWindowTokens;
    qulonglong maxOutputTokens;
    qulonglong inputPriceMicros;
    qulonglong outputPriceMicros;
    qulonglong cacheWritePriceMicros {0};
    qulonglong cacheReadPriceMicros {0};
    ai::application::LongContextPricing longContext {};
};

std::vector<CatalogEntry> builtInCatalog(const QString& providerKind)
{
    if (providerKind == QStringLiteral("openai")) {
        // Keep these ids aligned with OpenAIProvider::resolveModel() (a concrete id outside that
        // switch is still sent as given). gpt-6-sol: the official Standard rates and long-context tier.
        return {
            {"gpt-6-sol", "GPT-6 Sol", 1'050'000, 128'000, 2'000'000, 10'000'000, 2'500'000, 200'000,
                {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000}},
            {"gpt-5.6-sol", "GPT-5.6 Sol", 400000, 32768, 1'500'000, 7'500'000},
            {"gpt-5.6-terra", "GPT-5.6 Terra", 400000, 32768, 2'500'000, 15'000'000},
            {"gpt-5.6-luna", "GPT-5.6 Luna", 400000, 32768, 1'000'000, 5'000'000},
            {"gpt-5.4", "GPT-5.4", 400000, 32768, 2'000'000, 8'000'000},
            {"gpt-5", "GPT-5", 400000, 32768, 1'250'000, 10'000'000},
            {"gpt-5-mini", "GPT-5 mini", 400000, 16384, 250'000, 1'250'000},
            {"gpt-5-nano", "GPT-5 nano", 400000, 8192, 50'000, 200'000}
        };
    }
    if (providerKind == QStringLiteral("anthropic")) {
        // Keep these ids aligned with AnthropicProvider::resolveModel(). The cache prices are the
        // 5-minute prompt-cache rates this application uses: write 1.25x, read 0.1x of the input price.
        return {
            {"claude-fable-5", "Claude Fable 5", 200000, 32768, 10'000'000, 50'000'000, 12'500'000, 1'000'000},
            {"claude-opus-4-8", "Claude Opus 4.8", 200000, 32768, 5'000'000, 25'000'000, 6'250'000, 500'000},
            {"claude-sonnet-5", "Claude Sonnet 5", 200000, 32768, 2'000'000, 10'000'000, 2'500'000, 200'000},
            {"claude-haiku-4-5-20251001", "Claude Haiku 4.5", 200000, 16384, 1'000'000, 5'000'000,
                1'250'000, 100'000}
        };
    }
    return {};
}

QVariantMap catalogModel(const CatalogEntry& entry)
{
    QVariantMap model;
    model.insert(QStringLiteral("apiModelId"), QString::fromLatin1(entry.apiModelId));
    model.insert(QStringLiteral("displayName"), QString::fromLatin1(entry.displayName));
    model.insert(QStringLiteral("contextWindowTokens"), QVariant::fromValue(entry.contextWindowTokens));
    model.insert(QStringLiteral("maxOutputTokens"), QVariant::fromValue(entry.maxOutputTokens));
    model.insert(QStringLiteral("inputPriceMicros"), QVariant::fromValue(entry.inputPriceMicros));
    model.insert(QStringLiteral("outputPriceMicros"), QVariant::fromValue(entry.outputPriceMicros));
    model.insert(QStringLiteral("cacheWritePriceMicros"), QVariant::fromValue(entry.cacheWritePriceMicros));
    model.insert(QStringLiteral("cacheReadPriceMicros"), QVariant::fromValue(entry.cacheReadPriceMicros));
    model.insert(QStringLiteral("longContextThresholdInputTokens"),
        QVariant::fromValue<qulonglong>(entry.longContext.thresholdInputTokens));
    return model;
}

} // namespace

ProviderCatalogController::ProviderCatalogController(
    ai::application::ProviderCatalogService& service,
    QObject* parent)
    : QObject(parent)
    , service_(service)
    , providerModel_(this)
    , modelModel_(this)
{
    refresh();
}

QAbstractItemModel* ProviderCatalogController::providers() noexcept { return &providerModel_; }
QAbstractItemModel* ProviderCatalogController::models() noexcept { return &modelModel_; }
QString ProviderCatalogController::lastError() const { return lastError_; }

QString ProviderCatalogController::createProvider(
    const QString& name,
    const QString& kind,
    const QString& baseUrl)
{
    // Credential values never enter this API. A future credential controller
    // will store them through ICredentialStore and pass only an opaque ref.
    auto result = service_.createProvider({
        name.toStdString(), kind.toStdString(), baseUrl.toStdString(), true, std::nullopt
    });
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }
    clearError();
    refresh();
    return QString::fromStdString(result.value());
}

bool ProviderCatalogController::updateProvider(
    const QString& providerId,
    const QString& name,
    const QString& kind,
    const QString& baseUrl)
{
    // Preserve any existing opaque credential reference; editing metadata
    // must never accidentally clear a keychain link it cannot itself manage.
    const auto existing = service_.findProvider(providerId.toStdString());
    if (!existing.success()) {
        reportError(*existing.error());
        return false;
    }
    if (!existing.value().has_value()) {
        reportError({"provider.not_found", "Provider not found"});
        return false;
    }

    auto result = service_.updateProvider({
        providerId.toStdString(),
        name.toStdString(),
        kind.toStdString(),
        baseUrl.toStdString(),
        existing.value()->credentialReference
    });
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::setProviderEnabled(const QString& providerId, bool enabled)
{
    const auto result = service_.setProviderEnabled(providerId.toStdString(), enabled);
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::hasBuiltInModelCatalog(const QString& providerKind) const
{
    return providerKind == QStringLiteral("openai")
        || providerKind == QStringLiteral("anthropic");
}

QVariantList ProviderCatalogController::catalogModelsForProvider(
    const QString& providerKind) const
{
    QVariantList models;
    for (const CatalogEntry& entry : builtInCatalog(providerKind))
        models.append(catalogModel(entry));
    return models;
}

QString ProviderCatalogController::createCatalogModel(
    const QString& providerId,
    const QString& apiModelId)
{
    // The values come from the built-in catalog, never from the caller: a fresh install must start
    // with the cache prices and the long-context tier too (createModel() has no way to pass them).
    const auto provider = service_.findProvider(providerId.toStdString());
    if (!provider.success()) {
        reportError(*provider.error());
        return {};
    }
    if (!provider.value().has_value()) {
        reportError({"provider.not_found", "Provider not found"});
        return {};
    }
    const auto catalog = builtInCatalog(QString::fromStdString(provider.value()->kind));
    const auto entry = std::find_if(catalog.begin(), catalog.end(), [&](const CatalogEntry& candidate) {
        return apiModelId == QString::fromLatin1(candidate.apiModelId);
    });
    if (entry == catalog.end()) {
        reportError({"model.not_in_catalog", "The model is not in the built-in catalog of this provider"});
        return {};
    }

    auto result = service_.createModel({
        providerId.toStdString(),
        entry->apiModelId,
        entry->displayName,
        entry->contextWindowTokens,
        entry->maxOutputTokens,
        entry->inputPriceMicros,
        entry->outputPriceMicros,
        entry->cacheWritePriceMicros,
        entry->cacheReadPriceMicros,
        /*enabled=*/true,
        entry->longContext
    });
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }
    clearError();
    refresh();
    return QString::fromStdString(result.value());
}

QString ProviderCatalogController::createModel(
    const QString& providerId,
    const QString& apiModelId,
    const QString& displayName,
    qulonglong contextWindowTokens,
    qulonglong maxOutputTokens,
    qulonglong inputPriceMicros,
    qulonglong outputPriceMicros)
{
    auto result = service_.createModel({
        providerId.toStdString(),
        apiModelId.toStdString(),
        displayName.toStdString(),
        contextWindowTokens,
        maxOutputTokens,
        inputPriceMicros,
        outputPriceMicros,
        /*cacheWritePriceMicrosPerMillionTokens=*/0,
        /*cacheReadPriceMicrosPerMillionTokens=*/0,
        /*enabled=*/true
    });
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }
    clearError();
    refresh();
    return QString::fromStdString(result.value());
}

bool ProviderCatalogController::updateModelPricing(
    const QString& modelId,
    qulonglong inputPriceMicros,
    qulonglong outputPriceMicros,
    qulonglong cacheWritePriceMicros,
    qulonglong cacheReadPriceMicros)
{
    const auto existing = service_.findModel(modelId.toStdString());
    if (!existing.success()) {
        reportError(*existing.error());
        return false;
    }
    if (!existing.value().has_value()) {
        reportError({"model.not_found", "Model not found"});
        return false;
    }

    const auto& model = *existing.value();
    const auto result = service_.updateModel({
        model.id, model.apiModelId, model.displayName,
        model.contextWindowTokens, model.maxOutputTokens,
        inputPriceMicros, outputPriceMicros,
        cacheWritePriceMicros, cacheReadPriceMicros
    });
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::saveModelPricing(
    const QString& modelId,
    qulonglong inputPriceMicros,
    qulonglong outputPriceMicros,
    qulonglong cacheWritePriceMicros,
    qulonglong cacheReadPriceMicros,
    qulonglong longContextThresholdInputTokens,
    qulonglong longContextInputPriceMicros,
    qulonglong longContextOutputPriceMicros,
    qulonglong longContextCacheWritePriceMicros,
    qulonglong longContextCacheReadPriceMicros)
{
    const auto existing = service_.findModel(modelId.toStdString());
    if (!existing.success()) {
        reportError(*existing.error());
        return false;
    }
    if (!existing.value().has_value()) {
        reportError({"model.not_found", "Model not found"});
        return false;
    }

    const auto& model = *existing.value();
    // The tier is ALWAYS passed explicitly here (an engaged optional): all zeros is the explicit
    // removal, everything else is validated by the Backend against the model's context window.
    const auto result = service_.updateModel({
        model.id, model.apiModelId, model.displayName,
        model.contextWindowTokens, model.maxOutputTokens,
        inputPriceMicros, outputPriceMicros,
        cacheWritePriceMicros, cacheReadPriceMicros,
        ai::application::LongContextPricing {
            longContextThresholdInputTokens,
            longContextInputPriceMicros, longContextOutputPriceMicros,
            longContextCacheWritePriceMicros, longContextCacheReadPriceMicros,
            model.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens}
    });
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::saveModelPricingWithCacheDuration(
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
    qulonglong longContextCacheWriteOneHourPriceMicros)
{
    const auto existing = service_.findModel(modelId.toStdString());
    if (!existing.success()) {
        reportError(*existing.error());
        return false;
    }
    if (!existing.value().has_value()) {
        reportError({"model.not_found", "Model not found"});
        return false;
    }
    const auto& model = *existing.value();
    const auto result = service_.updateModel({
        model.id, model.apiModelId, model.displayName,
        model.contextWindowTokens, model.maxOutputTokens,
        inputPriceMicros, outputPriceMicros,
        cacheWritePriceMicros, cacheReadPriceMicros,
        ai::application::LongContextPricing {
            longContextThresholdInputTokens,
            longContextInputPriceMicros, longContextOutputPriceMicros,
            longContextCacheWritePriceMicros, longContextCacheReadPriceMicros,
            longContextCacheWriteOneHourPriceMicros},
        cacheWriteOneHourPriceMicros});
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::updateModel(
    const QString& modelId,
    const QString& apiModelId,
    const QString& displayName,
    qulonglong contextWindowTokens,
    qulonglong maxOutputTokens,
    qulonglong inputPriceMicros,
    qulonglong outputPriceMicros)
{
    // Cache prices are not part of this form's contract (see header doc
    // comment) - preserve whatever ModelPricingDialog last set instead of
    // silently zeroing it out on an unrelated metadata edit.
    const auto existing = service_.findModel(modelId.toStdString());
    if (!existing.success()) {
        reportError(*existing.error());
        return false;
    }
    if (!existing.value().has_value()) {
        reportError({"model.not_found", "Model not found"});
        return false;
    }
    const auto& model = *existing.value();

    const auto result = service_.updateModel({
        modelId.toStdString(), apiModelId.toStdString(), displayName.toStdString(),
        contextWindowTokens, maxOutputTokens, inputPriceMicros, outputPriceMicros,
        model.cacheWritePriceMicrosPerMillionTokens, model.cacheReadPriceMicrosPerMillionTokens
    });
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::setModelEnabled(const QString& modelId, bool enabled)
{
    const auto result = service_.setModelEnabled(modelId.toStdString(), enabled);
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::deleteModel(const QString& modelId)
{
    const auto result = service_.deleteModel(modelId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ProviderCatalogController::setDefaultModel(
    const QString& providerId,
    const QString& modelId)
{
    const auto result = service_.setDefaultModel(
        providerId.toStdString(), modelId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

void ProviderCatalogController::refresh()
{
    const auto providersResult = service_.listProviders();
    if (!providersResult.success()) {
        reportError(*providersResult.error());
        return;
    }
    const auto modelsResult = service_.listModels();
    if (!modelsResult.success()) {
        reportError(*modelsResult.error());
        return;
    }

    providerModel_.replace(providersResult.value());
    modelModel_.replace(modelsResult.value());
    clearError();
    emit catalogChanged();
}

void ProviderCatalogController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}

void ProviderCatalogController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}
