#include "ai/application/planning/FollowUpAdmission.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace ai::application {
namespace {

using Error = common::Error;

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

template <typename T>
common::Result<T> forward(const Error& error)
{
    return common::Result<T>::failure(error);
}

common::Error stale(const char* cause, const std::string& detail)
{
    return {std::string("follow_up.stale_") + cause, detail};
}

bool startsWith(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

// The codes of checkFollowUpSourceEligibility: the Outcome gate no longer admits the source.
bool isOutcomeGateCode(const std::string& code)
{
    static const std::set<std::string> codes = {"follow_up.inconsistent_source", "follow_up.invalid_kind",
        "follow_up.invalid_outcome_state", "follow_up.invalid_run_status", "follow_up.outcome_attention_required",
        "follow_up.outcome_pending", "follow_up.outcome_superseded", "follow_up.recovery_flag_required",
        "follow_up.recovery_not_available", "follow_up.recovery_owned", "follow_up.source_cancelled",
        "follow_up.source_interrupted", "follow_up.source_not_terminal", "follow_up.unsupported_source_workflow"};
    return codes.contains(code);
}

// A stored request's read refusals that mean "the world changed", as opposed to a fault.
const char* staleCauseOfReadFailure(const std::string& code)
{
    if (code == "follow_up.project_not_found") return "project";
    if (code == "follow_up.run_not_found") return "source";
    if (code == "follow_up.outcome_missing") return "outcome";
    return nullptr;
}

const char* staleCauseOfQuoteFailure(const std::string& code)
{
    if (code == "follow_up.price_unknown" || code == "follow_up.cap_too_low") return "pricing";
    if (code == "follow_up.context_window_exceeded" || code == "follow_up.model_output_limit_too_small") return "limits";
    if (code == "follow_up.quote_provider_mismatch") return "provider";
    return nullptr;   // invalid_cap, quote_invalid_context: a broken request or assembler, not drift
}

common::Result<FollowUpAdmitted> admit(
    const FollowUpAdmissionPorts& ports, const FollowUpIntent& intent, const FollowUpPlanRequest* stored)
{
    using Result = common::Result<FollowUpAdmitted>;
    // A refusal: for a stored request a drift is a `stale_*`, for a new quote it is the plain code.
    const auto refuse = [&](const char* cause, std::string code, std::string message) {
        if (stored != nullptr) return Result::failure(stale(cause, message));
        return Result::failure({std::move(code), std::move(message)});
    };

    if (!followUpGoalIsValid(intent.goal)) return fail<FollowUpAdmitted>("follow_up.invalid_goal", "The goal is not a valid composer goal");
    const auto window = followUpAuthorisedDraftLimits(intent.splitHint, intent.maxDrafts);
    if (!window.success()) return forward<FollowUpAdmitted>(*window.error());

    // The recipient: the provider and the model must exist, be enabled and belong together; for a stored request the provider must
    // also still be exactly the one the bytes were quoted for (kind, base URL, credential reference).
    struct Catalog {
        Provider provider;
        Model model;
    };
    const auto resolveCatalog = [&]() -> common::Result<Catalog> {
        using CatalogResult = common::Result<Catalog>;
        const auto provider = ports.providers.findById(intent.providerId);
        if (!provider.success()) return CatalogResult::failure(*provider.error());
        if (!provider.value().has_value() || !provider.value()->enabled) {
            const auto refusal = refuse("provider", "follow_up.provider_unavailable", "The provider is missing or disabled");
            return CatalogResult::failure(*refusal.error());
        }
        const auto model = ports.models.findById(intent.modelId);
        if (!model.success()) return CatalogResult::failure(*model.error());
        if (!model.value().has_value() || !model.value()->enabled || model.value()->providerId != provider.value()->id) {
            const auto refusal = refuse("provider", "follow_up.model_unavailable", "The model is missing, disabled or not this provider's");
            return CatalogResult::failure(*refusal.error());
        }
        if (stored != nullptr && followUpProviderExecutionIdentity(*provider.value()) != stored->quote.providerIdentity) {
            return CatalogResult::failure(stale("provider", "The recipient of the bytes changed since the quote"));
        }
        return CatalogResult::ok(Catalog {*provider.value(), *model.value()});
    };

    // The sources: the project is Active, the Run is the project's, and for a stored request its Outcome is the pinned one.
    const auto resolveSources = [&]() -> common::Result<FollowUpSourceMaterial> {
        using SourceResult = common::Result<FollowUpSourceMaterial>;
        auto read = ports.source.read(intent.projectId, intent.sourceRunId);
        if (!read.success()) {
            const auto& error = *read.error();
            if (stored != nullptr) {
                if (const char* cause = staleCauseOfReadFailure(error.code)) return SourceResult::failure(stale(cause, error.message));
            }
            return read;
        }
        if (read.value().project.status != ProjectStatus::Active) {
            const auto refusal = refuse("project", "follow_up.project_inactive", "The project is not Active");
            return SourceResult::failure(*refusal.error());
        }
        if (read.value().runProjectId != intent.projectId) {
            const auto refusal = refuse("source", "follow_up.run_project_mismatch", "The source Run belongs to another project");
            return SourceResult::failure(*refusal.error());
        }
        if (stored != nullptr && read.value().brief.sourceStateFingerprint != stored->outcomeSourceStateFingerprint) {
            return SourceResult::failure(stale("outcome", "The Outcome of the source Run changed since the quote"));
        }
        return read;
    };

    // The ORDER is the contract. A stored request reports the FIRST cause in project -> source -> outcome -> provider -> files ->
    // context -> pricing -> limits, so the sources are checked before the catalog. A new quote asks the cheap catalog first (reading
    // the sources can hash a whole workspace), where the order of two refusals is not a promise.
    std::optional<common::Result<Catalog>> catalogResult;
    std::optional<common::Result<FollowUpSourceMaterial>> materialResult;
    if (stored != nullptr) {
        materialResult = resolveSources();
        if (!materialResult->success()) return Result::failure(*materialResult->error());
        catalogResult = resolveCatalog();
        if (!catalogResult->success()) return Result::failure(*catalogResult->error());
    } else {
        catalogResult = resolveCatalog();
        if (!catalogResult->success()) return Result::failure(*catalogResult->error());
        materialResult = resolveSources();
        if (!materialResult->success()) return Result::failure(*materialResult->error());
    }
    const Provider& provider = catalogResult->value().provider;
    const Model& model = catalogResult->value().model;
    auto& material = *materialResult;

    // The files. A list that is too long to show is refused whole - for a stored request that is a changed `files` world - and
    // it is judged HERE, after the project, source, Outcome and provider checks, so the order of causes holds.
    if (material.value().candidateRefusal.has_value()) {
        const auto& refusal = *material.value().candidateRefusal;
        return refuse("files", refusal.code, refusal.message);
    }
    // The selection is exact when given, every Eligible candidate otherwise. The file bytes are MOVED out of the material (they can be
    // 64 MiB) into the assembler's request and are gone when the context is built; the admitted material keeps no copy of them.
    std::vector<FollowUpFileCandidate> files = std::move(material.value().candidates);
    material.value().candidates.clear();
    if (intent.selectedPaths.has_value()) {
        std::set<std::string> wanted(intent.selectedPaths->begin(), intent.selectedPaths->end());
        std::set<std::string> known;
        for (const auto& file : files) known.insert(file.relativePath);
        for (const auto& path : wanted) {
            if (!known.contains(path)) {
                return refuse("files", "follow_up.unknown_file", "A selected file is not a candidate of the source Run: " + path);
            }
        }
        for (auto& file : files) file.selected = wanted.contains(file.relativePath);
    } else {
        for (auto& file : files) file.selected = file.safety == FollowUpFileSafety::Eligible;
    }

    FollowUpComposerContextRequest contextRequest;
    contextRequest.sourceRunId = intent.sourceRunId;
    contextRequest.kind = intent.kind;
    contextRequest.splitHint = intent.splitHint;
    contextRequest.maxDrafts = intent.maxDrafts;
    contextRequest.goal = intent.goal;
    contextRequest.snapshot = material.value().snapshot;
    contextRequest.brief = material.value().brief;
    contextRequest.files = std::move(files);
    auto context = assembleFollowUpComposerContext(std::move(contextRequest));
    if (!context.success()) {
        const auto& error = *context.error();
        if (stored != nullptr) {
            if (error.code == "follow_up.context_too_large") return Result::failure(stale("context", error.message));
            if (isOutcomeGateCode(error.code)) return Result::failure(stale("outcome", error.message));
        }
        return forward<FollowUpAdmitted>(error);
    }

    if (stored != nullptr) {
        if (followUpPinnedFilesOf(context.value()) != stored->files) {
            return Result::failure(stale("files", "A pinned file changed, vanished or is no longer sent"));
        }
        if (material.value().snapshot.fingerprint != stored->snapshotFingerprint) {
            return Result::failure(stale("context", "The project snapshot changed since the quote"));
        }
        if (context.value().contextFingerprint != stored->quote.contextFingerprint) {
            return Result::failure(stale("context", "The provider bytes changed since the quote"));
        }
        // Pricing is decided BEFORE the quote is priced, so that a price change is reported as pricing even when the same edit
        // also makes the quote fail on a limit.
        if (providerRequestPricingIdentity(model, followUpPromptCachePolicy(provider.kind)) != stored->quote.worstCase.pricingIdentity) {
            return Result::failure(stale("pricing", "The model or its prices changed since the quote"));
        }
    }

    auto quote = quoteFollowUpComposerRequest(context.value(), provider, model, intent.capMicros);
    if (!quote.success()) {
        const auto& error = *quote.error();
        if (stored != nullptr) {
            if (const char* cause = staleCauseOfQuoteFailure(error.code)) return Result::failure(stale(cause, error.message));
        }
        return forward<FollowUpAdmitted>(error);
    }

    if (stored != nullptr) {
        switch (compareFollowUpQuotes(stored->quote, quote.value())) {
        case FollowUpQuoteDrift::None: break;
        case FollowUpQuoteDrift::Context: return Result::failure(stale("context", "The provider bytes changed since the quote"));
        case FollowUpQuoteDrift::Provider: return Result::failure(stale("provider", "The recipient of the bytes changed since the quote"));
        case FollowUpQuoteDrift::Pricing: return Result::failure(stale("pricing", "The model or its prices changed since the quote"));
        case FollowUpQuoteDrift::Limits: return Result::failure(stale("limits", "The limits or the worst-case cost changed since the quote"));
        }
    }

    FollowUpAdmitted admitted;
    admitted.provider = provider;
    admitted.model = model;
    admitted.material = std::move(material.value());
    admitted.context = std::move(context.value());
    admitted.quote = std::move(quote.value());
    return Result::ok(std::move(admitted));
}

} // namespace

std::vector<FollowUpPinnedFile> followUpPinnedFilesOf(const FollowUpComposerContext& context)
{
    std::vector<FollowUpPinnedFile> pinned;
    for (const auto& file : context.files) {
        if (file.disposition != FollowUpExcerptDisposition::Included) continue;
        pinned.push_back({file.relativePath, file.contentSha256, file.originalBytes, file.shownBytes, file.estimatedTokens,
            file.truncated});
    }
    return pinned;
}

common::Result<FollowUpAdmitted> admitFollowUpIntent(const FollowUpAdmissionPorts& ports, const FollowUpIntent& intent)
{
    return admit(ports, intent, nullptr);
}

FollowUpIntent followUpIntentOf(const FollowUpPlanRequest& request)
{
    FollowUpIntent intent;
    intent.projectId = request.projectId;
    intent.sourceRunId = request.sourceRunId;
    intent.kind = request.kind;
    intent.goal = request.goal;
    intent.maxDrafts = request.maxDrafts;
    intent.splitHint = request.splitHint;
    intent.providerId = request.quote.providerIdentity.providerId;
    intent.modelId = request.quote.worstCase.pricingIdentity.modelId;
    intent.capMicros = request.quote.capMicros;
    std::vector<std::string> paths;
    for (const auto& file : request.files) paths.push_back(file.relativePath);
    intent.selectedPaths = std::move(paths);
    return intent;
}

common::Result<FollowUpAdmitted> admitStoredFollowUpRequest(
    const FollowUpAdmissionPorts& ports, const FollowUpPlanRequest& request)
{
    return admit(ports, followUpIntentOf(request), &request);
}

bool isFollowUpStaleCode(const std::string& code) noexcept
{
    return startsWith(code, "follow_up.stale_");
}

} // namespace ai::application
