#include "ai/application/IOutcomeBriefRepository.hpp"

#include "ai/application/IdGenerator.hpp"

#include <limits>

namespace ai::application {

namespace {

common::Error repositoryError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

} // namespace

common::Result<void> checkOutcomeBriefAppendRequest(const OutcomeBrief& brief, const OutcomeBriefExpectedHead& expected)
{
    switch (expected.kind) {
    case OutcomeBriefExpectedHead::Kind::Any:
    case OutcomeBriefExpectedHead::Kind::None:
        // Only a Revision names an id; an id next to Any / None is a caller bug, not a harmless extra.
        if (!expected.revisionId.empty()) {
            return common::Result<void>::failure(repositoryError(
                "outcome_brief.invalid_request", "Only an expected revision carries a revision id"));
        }
        break;
    case OutcomeBriefExpectedHead::Kind::Revision:
        if (expected.revisionId.empty()) {
            return common::Result<void>::failure(
                repositoryError("outcome_brief.invalid_request", "An expected revision needs its id"));
        }
        break;
    default:
        return common::Result<void>::failure(
            repositoryError("outcome_brief.invalid_request", "Unknown expected head kind"));
    }
    // The mandatory write gate: whatever is stored is exactly what the resolver could have derived.
    return validateOutcomeBrief(brief);
}

common::Result<void> checkOutcomeBriefListLimit(std::size_t limit)
{
    if (limit == 0 || limit > kOutcomeBriefMaxListLimit) {
        return common::Result<void>::failure(repositoryError(
            "outcome_brief.invalid_request", "The list limit must be between 1 and 1000"));
    }
    return common::Result<void>::ok();
}

common::Result<void> checkOutcomeBriefWriteAuthority(const OutcomeBrief& brief, const std::optional<OutcomeBriefRunOwner>& owner)
{
    if (!owner.has_value()) {
        return common::Result<void>::failure(repositoryError("outcome_brief.run_not_found", "The Run does not exist"));
    }
    if (owner->projectId != brief.projectId) {
        return common::Result<void>::failure(
            repositoryError("outcome_brief.project_mismatch", "The Run belongs to another project"));
    }
    if (owner->status != brief.terminalStatus) {
        return common::Result<void>::failure(repositoryError(
            "outcome_brief.run_status_mismatch", "The Run is no longer in the status this brief describes"));
    }
    return common::Result<void>::ok();
}

common::Result<void> checkOutcomeBriefReadAuthority(
    const OutcomeBrief& brief, const std::optional<OutcomeBriefRunOwner>& owner, bool isHead)
{
    const auto invalid = [](const char* what) {
        return common::Result<void>::failure(repositoryError("persistence.invalid_data", std::string("Outcome brief ") + what));
    };
    if (!owner.has_value()) return invalid("belongs to a Run that does not exist");
    if (owner->projectId != brief.projectId) return invalid("names another project than the Run it is about");
    if (isHead && owner->status != brief.terminalStatus) {
        return invalid("is stale: its Run is no longer in the terminal status the brief describes");
    }
    return common::Result<void>::ok();
}

common::Result<void> checkOutcomeBriefRunExists(bool runExists)
{
    if (runExists) return common::Result<void>::ok();
    return common::Result<void>::failure(
        repositoryError("persistence.invalid_data", "Outcome brief head was asked for a Run that does not exist"));
}

common::Result<OutcomeBriefAppendPlan> planOutcomeBriefAppend(
    const OutcomeBrief& brief, const OutcomeBriefExpectedHead& expected, const std::optional<OutcomeBriefRevision>& head)
{
    using Result = common::Result<OutcomeBriefAppendPlan>;
    const auto changed = [](const char* what) {
        return Result::failure(repositoryError("outcome_brief.head_changed", what));
    };
    switch (expected.kind) {
    case OutcomeBriefExpectedHead::Kind::Any:
        break;
    case OutcomeBriefExpectedHead::Kind::None:
        if (head.has_value()) return changed("The Run already has a brief head");
        break;
    case OutcomeBriefExpectedHead::Kind::Revision:
        if (!head.has_value() || head->id != expected.revisionId) {
            return changed("The brief head is not the expected revision any more");
        }
        break;
    }

    OutcomeBriefAppendPlan plan;
    if (head.has_value() && head->brief.sourceStateFingerprint == brief.sourceStateFingerprint) {
        // The head already says exactly this. Compared with the head ONLY: an older revision with the same source state
        // does not make a return to it a duplicate.
        plan.appendRevision = false;
        plan.ordinal = head->ordinal;
        return Result::ok(plan);
    }
    const std::uint64_t previous = head.has_value() ? head->ordinal : 0;
    if (previous >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Result::failure(repositoryError("outcome_brief.revision_limit", "The ordinal range of the Run is exhausted"));
    }
    plan.appendRevision = true;
    plan.ordinal = previous + 1;
    return Result::ok(plan);
}

OutcomeBriefRevision makeOutcomeBriefRevision(OutcomeBrief brief, std::uint64_t ordinal)
{
    OutcomeBriefRevision revision;
    revision.id = generateUuid();
    revision.ordinal = ordinal;
    revision.brief = std::move(brief);
    revision.createdAt = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));
    return revision;
}

bool outcomeBriefHeadComesBefore(const OutcomeBriefRevision& left, const OutcomeBriefRevision& right)
{
    if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
    return left.brief.runId > right.brief.runId;
}

} // namespace ai::application
