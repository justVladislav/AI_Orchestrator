#pragma once

#include "ai/application/planning/FollowUpPlan.hpp"
#include "ai/application/planning/FollowUpPlanRequest.hpp"
#include "ai/application/planning/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/planning/IOutcomeBriefRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/planning/IRunDraftRepository.hpp"
#include "ai/application/execution/IRunSourceLookup.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// PC-5D: everything the follow-up dialog and the Outcome block READ before the user commits to anything, decided by the Backend so
// that no QML code classifies a Run, a model or a request. It reads through const views only (the brief repository, the request
// repository, the catalog, the Draft store and the Run lookup): it cannot write, quote, authorise, claim or call a provider, and it
// has no way to start a Run.
//
// A GUEST runtime (another process owns the database) is READ-NOTHING: with a `refusal` every entry fails closed with it
// (follow_up.not_the_owner) before it reads anything, exactly like FollowUpPlanService.

struct FollowUpKindEligibility {
    // The kind is OFFERED for this Run at all (a Failed Run is offered `recovery`, not `next_run`, and the other way round).
    bool offered {false};
    // The 5A-1 gate passed AND no request holds the source Run's slot.
    bool allowed {false};
    // Why not, when offered but not allowed: a stable `follow_up.*` code of the gate (or `follow_up.request_open`) and its message.
    std::string reasonCode;
    std::string reasonMessage;
};

struct FollowUpRunEligibility {
    enum class Availability {
        NotApplicable, // no current Outcome Brief for this Run (not an outcome Run, still going, unknown): the block shows nothing
        Evaluated,     // the Run has a current brief; `nextRun` / `recovery` say what may be planned
    };
    Availability availability {Availability::NotApplicable};
    RunId runId;
    ProjectId projectId;
    FollowUpKindEligibility nextRun;
    FollowUpKindEligibility recovery;
    // The request that holds the Run's slot (quoted | authorized | composing | plan_ready), if any.
    std::optional<FollowUpPlanRequest> openRequest;
    // The newest request of this Run in ANY state, if any: what the block's status line shows (a failed, stale, expired, cancelled or
    // interrupted request stays visible until a newer one replaces it; a discarded one is not shown).
    std::optional<FollowUpPlanRequest> latestRequest;
};

// A model the dialog's picker offers. `usable` is an ADVISORY pre-filter on facts the catalog states plainly (the provider and the
// model are enabled, both prices were entered, the output limit can hold the minimum answer); the quote is still the authority and
// refuses the rest with its own typed, free code. `reasonCode` says why a model is shown disabled.
struct FollowUpModelChoice {
    ProviderId providerId;
    std::string providerName;
    std::string providerKind;
    ModelId modelId;
    std::string displayName;
    bool usable {false};
    // provider_disabled | model_disabled | price_unknown | output_limit_too_small, empty when usable.
    std::string reasonCode;
    std::uint64_t contextWindowTokens {0};
    std::uint64_t maxOutputTokens {0};
    std::uint64_t inputPriceMicrosPerMillionTokens {0};
    std::uint64_t outputPriceMicrosPerMillionTokens {0};
    bool hasCredentialReference {false};
};

class FollowUpPlanReadModel final {
public:
    // The newest Runs offered as further advisory source Runs (the same bound as the Prompt Workshop's picker).
    static constexpr std::size_t kMaxSourceRunChoices = 50;

    FollowUpPlanReadModel(
        const IOutcomeBriefRepository& briefs,
        const IFollowUpPlanRequestRepository& requests,
        const IProviderRepository& providers,
        const IModelRepository& models,
        const IRunDraftRepository& drafts,
        const IRunSourceLookup& runs,
        std::optional<common::Error> refusal = std::nullopt);

    // The gate for both kinds over the Run's CURRENT Outcome Brief, plus the open and the latest request. Failure: the refusal, or a
    // persistence fault (a stale or corrupt head is `persistence.invalid_data`, never an empty answer).
    [[nodiscard]] common::Result<FollowUpRunEligibility> eligibility(const RunId& runId) const;

    // Every model of every provider, in the catalog's order, with its advisory usability.
    [[nodiscard]] common::Result<std::vector<FollowUpModelChoice>> modelChoices() const;

    // Saved Drafts of the project that can lend their roster (custom roster, Manifest workflow, a non-empty roster).
    [[nodiscard]] common::Result<std::vector<RunDraft>> rosterTemplates(const ProjectId& projectId) const;

    // The newest Runs of the project (at most kMaxSourceRunChoices) a user may ADD as advisory source Runs. When `requiredRunId` is
    // present, that Run is guaranteed to be in the bounded result even when it is older than the newest window; it is the immutable
    // source Run that the follow-up review must show selected and locked.
    [[nodiscard]] common::Result<std::vector<RunSourceChoice>> sourceRunChoices(
        const ProjectId& projectId, const RunId& requiredRunId = {}) const;

private:
    [[nodiscard]] common::Result<void> checkOwner() const;

    const IOutcomeBriefRepository& briefs_;
    const IFollowUpPlanRequestRepository& requests_;
    const IProviderRepository& providers_;
    const IModelRepository& models_;
    const IRunDraftRepository& drafts_;
    const IRunSourceLookup& runs_;
    std::optional<common::Error> refusal_;
};

} // namespace ai::application
