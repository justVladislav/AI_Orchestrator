#pragma once

#include "ai/application/planning/FollowUpAdmission.hpp"
#include "ai/application/planning/FollowUpPlanAdvisories.hpp"
#include "ai/application/planning/FollowUpPlanRequest.hpp"
#include "ai/application/planning/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/planning/IFollowUpSourceReader.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/planning/IRunDraftRepository.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// PC-5C: the Backend of the opt-in paid next-Run composer, everything the dialog does EXCEPT the paid call itself (that is the
// FollowUpComposerProcessor). The user's commands are quote -> authorize -> (wait) -> preview -> confirm, plus cancel and discard.
//
// By construction it can neither start nor change anything it is not meant to: its constructor names only the request repository,
// the atomic Confirm port, the read-only source reader, the provider / model catalog (read) and the Draft store (read, for a roster
// template). It has no RunManager, no workflow runner, no provider client, no proposal, Apply, Work Ledger or Outcome Brief
// writer, and no Run write. The only things it ever writes are the request rows and, through the atomic port, ONE Draft group.
//
// A GUEST runtime (another process owns the database) is READ-NOTHING and WRITE-NOTHING: with a `refusal` every entry fails closed
// with it (follow_up.not_the_owner) before it reads or writes anything.

struct FollowUpFileView {
    std::string relativePath;
    FollowUpFileSafety safety {FollowUpFileSafety::ReadFailed};
    FollowUpExcerptDisposition disposition {FollowUpExcerptDisposition::Ineligible};
    std::string contentSha256;
    std::uint64_t originalBytes {0};
    std::uint64_t shownBytes {0};
    std::uint64_t estimatedTokens {0};
    bool truncated {false};
};

struct FollowUpQuoteOutcome {
    FollowUpPlanRequest request;   // quoted
    // The `quoted` request of the same source Run that this quote replaced (a free discard, in the same transaction).
    std::optional<FollowUpPlanRequestId> replacedQuoteId;
    // EVERY candidate with what happens to it in THIS context (the dialog's list), bytewise by path.
    std::vector<FollowUpFileView> files;
};

struct FollowUpPlanPreview {
    FollowUpPlanRequest request;
    // plan_ready and drafts_saved only: the plan the STORED reply parses to, with the advisories stored with it.
    std::optional<NextRunPlan> plan;
    std::vector<FollowUpPlanAdvisory> advisories;
};

struct FollowUpConfirmOptions {
    // An explicitly chosen draft of the SAME project whose roster, parallelism, provider limits and repair policy are copied.
    std::optional<RunDraftId> rosterTemplate;
    // Further source Runs the user names; the request's own Run is always one of the group's and cannot be removed.
    std::vector<RunId> extraSourceRunIds;
    // 6a (a): a Draft whose plan carried a valid `staffing` hint gets `maxParallelism = staffing.parallelism` (applied AFTER the roster
    // template's value, so it wins when both are chosen). Nothing else of the hint is copied: no roster is created, trimmed or cloned.
    // A Draft without a hint keeps the template's value (or the default). Ignored by a repeated Confirm, like the template.
    bool applySuggestedParallelism {false};
};

class FollowUpPlanService final {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    FollowUpPlanService(
        IFollowUpPlanRequestRepository& requests,
        IFollowUpPlanConfirmPort& confirmPort,
        const IFollowUpSourceReader& source,
        const IProviderRepository& providers,
        const IModelRepository& models,
        const IRunDraftRepository& drafts,
        Clock clock = {},
        std::optional<common::Error> refusal = std::nullopt);

    // Wiring the processor in (both optional; without them an authorised request just waits for the next startup pass, and a
    // composing request is cancelled directly).
    //   nudge:  called with the request id after a successful authorisation.
    //   cancel: asks the call that is running for this id to stop; true when there was one. The request then becomes cancelled
    //           when the call returns (possibly charged).
    void setAuthorizedNudge(std::function<void(const FollowUpPlanRequestId&)> nudge);
    void setComposingCancel(std::function<bool(const FollowUpPlanRequestId&)> cancel);

    // The candidate files of a source Run with what would happen to each by default (the first eight Eligible are sent). The same
    // admission gate as a quote, minus the model: a Run that cannot be planned from is refused with the gate's code.
    [[nodiscard]] common::Result<std::vector<FollowUpFileView>> describeFiles(
        const ProjectId& projectId, const RunId& sourceRunId, FollowUpPlanKind kind) const;

    // ONE immutable quote: reads, assembles and prices (nothing is sent) and stores a `quoted` request, replacing the source Run's
    // earlier `quoted` one in the same transaction. `follow_up.request_open` when an authorised / composing / ready request holds
    // the slot. See admitFollowUpIntent for the free refusals.
    [[nodiscard]] common::Result<FollowUpQuoteOutcome> quote(const FollowUpIntent& intent);

    // quoted -> authorized after the admission checks ran AGAIN (admitStoredFollowUpRequest). A drift is a free `stale`
    // (follow_up.stale_*, the request becomes stale); a request past its 15 minutes becomes `expired` (follow_up.quote_expired).
    // Never calls the provider: the processor does, once, after the claim.
    [[nodiscard]] common::Result<FollowUpPlanRequest> authorize(const FollowUpPlanRequestId& id);

    // quoted | authorized -> cancelled (free). composing: asks the running call to stop (see setComposingCancel); the request becomes
    // cancelled, possibly charged, when it returns. Anything else: follow_up.invalid_transition.
    [[nodiscard]] common::Result<FollowUpPlanRequest> cancel(const FollowUpPlanRequestId& id);

    // quoted -> discarded (free), plan_ready -> discarded (the paid plan is dropped; possibly charged).
    [[nodiscard]] common::Result<FollowUpPlanRequest> discard(const FollowUpPlanRequestId& id);

    [[nodiscard]] common::Result<std::optional<FollowUpPlanRequest>> find(const FollowUpPlanRequestId& id) const;
    [[nodiscard]] common::Result<std::optional<FollowUpPlanRequest>> findOpenForSourceRun(const RunId& runId) const;
    [[nodiscard]] common::Result<std::vector<FollowUpPlanRequest>> listForProject(const ProjectId& projectId, std::size_t limit) const;
    [[nodiscard]] common::Result<FollowUpPlanPreview> preview(const FollowUpPlanRequestId& id) const;

    // The explicit Confirm: ONE atomic operation (IFollowUpPlanConfirmPort) that saves the Draft group of the stored plan.
    // `expectedPlanFingerprint` is the plan the user was shown (FollowUpPlanPreview). A repeat returns the same group and writes
    // nothing. Failure: the port's (follow_up.not_plan_ready, plan_changed, drafts_not_the_plan, source_run_*, drafts_removed, ...)
    // and follow_up.template_* / follow_up.duplicate_source_run / follow_up.too_many_source_runs for the options.
    [[nodiscard]] common::Result<FollowUpConfirmResult> confirm(
        const FollowUpPlanRequestId& id, const std::string& expectedPlanFingerprint, const FollowUpConfirmOptions& options = {});

private:
    [[nodiscard]] std::chrono::system_clock::time_point now() const;
    [[nodiscard]] common::Result<void> checkOwner() const;
    [[nodiscard]] FollowUpAdmissionPorts admissionPorts() const;
    [[nodiscard]] common::Result<FollowUpPlanRequest> load(const FollowUpPlanRequestId& id) const;
    [[nodiscard]] common::Result<FollowUpPlanRequest> step(
        const FollowUpPlanRequest& request, FollowUpPlanEvent event, const std::string& contextFingerprint = {});

    IFollowUpPlanRequestRepository& requests_;
    IFollowUpPlanConfirmPort& confirm_;
    const IFollowUpSourceReader& source_;
    const IProviderRepository& providers_;
    const IModelRepository& models_;
    const IRunDraftRepository& drafts_;
    Clock clock_;
    std::optional<common::Error> refusal_;

    mutable std::mutex hooksMutex_;
    std::function<void(const FollowUpPlanRequestId&)> nudge_;
    std::function<bool(const FollowUpPlanRequestId&)> cancelComposing_;
};

} // namespace ai::application
