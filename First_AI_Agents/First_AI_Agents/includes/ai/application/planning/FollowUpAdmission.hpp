#pragma once

#include "ai/application/planning/FollowUpComposerContext.hpp"
#include "ai/application/planning/FollowUpComposerQuote.hpp"
#include "ai/application/planning/FollowUpPlanRequest.hpp"
#include "ai/application/planning/IFollowUpSourceReader.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// PC-5C: the ONE admission procedure of the follow-up composer. The quote, the authorisation and the claim all run it, so what is
// checked cannot differ between them. It reads the sources, assembles the provider bytes ITSELF (the quote contract of 5A-3: the
// service, not a caller, is the origin of the bytes) and prices them. Nothing is written and no provider is called.
struct FollowUpAdmissionPorts {
    const IFollowUpSourceReader& source;
    const IProviderRepository& providers;
    const IModelRepository& models;
};

// What the dialog edits (and what a stored request carries).
struct FollowUpIntent {
    ProjectId projectId;
    RunId sourceRunId;
    FollowUpPlanKind kind {FollowUpPlanKind::NextRun};
    std::string goal;
    std::uint32_t maxDrafts {1};
    FollowUpSplitHint splitHint {FollowUpSplitHint::Auto};
    ProviderId providerId;
    ModelId modelId;
    // The user's cap; empty = the system ceiling.
    std::optional<std::uint64_t> capMicros;
    // The files to send, by relative path. Empty optional = every Eligible candidate (the assembler keeps the first eight in path
    // order and reports the rest as FileLimit). An explicit list must name candidates; it is exact, never extended.
    std::optional<std::vector<std::string>> selectedPaths;
};

struct FollowUpAdmitted {
    Provider provider;
    Model model;
    FollowUpSourceMaterial material;
    FollowUpComposerContext context;
    FollowUpComposerQuote quote;
};

// The files an assembled context actually sends, as a request pins them.
[[nodiscard]] std::vector<FollowUpPinnedFile> followUpPinnedFilesOf(const FollowUpComposerContext& context);

// A new quote. Refusals are the plain typed errors, all FREE (nothing was sent):
//   follow_up.invalid_goal, follow_up.invalid_draft_limits, follow_up.provider_unavailable, follow_up.model_unavailable,
//   follow_up.project_not_found, follow_up.project_inactive, follow_up.run_not_found, follow_up.run_project_mismatch,
//   follow_up.outcome_missing, follow_up.unknown_file (a selected path is no candidate), the whole-gate codes of the assembler
//   (follow_up.not_outcome_run, follow_up.invalid_outcome_state, ...), follow_up.context_* and the quote's follow_up.* codes
//   (price_unknown, cap_too_low, invalid_cap, context_window_exceeded, model_output_limit_too_small, ...).
// A persistence or filesystem fault is passed on unchanged.
[[nodiscard]] common::Result<FollowUpAdmitted> admitFollowUpIntent(
    const FollowUpAdmissionPorts& ports, const FollowUpIntent& intent);

// The intent a stored request was quoted for: its pinned files are the selection, its stored cap the cap.
[[nodiscard]] FollowUpIntent followUpIntentOf(const FollowUpPlanRequest& request);

// A STORED request, checked again against what is true now (at the authorisation and at the claim). Any drift before the call is a
// FREE `stale`, reported with the first cause found, in this order:
//   follow_up.stale_project   the project is gone or no longer Active
//   follow_up.stale_source    the source Run is gone or belongs to another project
//   follow_up.stale_outcome   the Outcome head is missing, its source state is not the pinned one, or the whole gate no longer holds
//   follow_up.stale_provider  the provider or the model is gone, disabled or no longer the model's provider, or the recipient changed
//   follow_up.stale_files     a pinned file changed, vanished or is no longer sent
//   follow_up.stale_context   anything else in the exact provider bytes or the project snapshot
//   follow_up.stale_pricing   the pricing identity (model, prices, window, cache policy) or the price is no longer known
//   follow_up.stale_limits    the token counts, the effective output, the cost or the cap
// A fault (follow_up.context_* of a broken loader, persistence errors) is NOT a stale: it is returned as it is and the request is left
// alone.
[[nodiscard]] common::Result<FollowUpAdmitted> admitStoredFollowUpRequest(
    const FollowUpAdmissionPorts& ports, const FollowUpPlanRequest& request);

[[nodiscard]] bool isFollowUpStaleCode(const std::string& code) noexcept;

} // namespace ai::application
