#include "ai/application/RunDraftGroup.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/NextRunPlan.hpp"
#include "ai/application/RunInterfaceContract.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace ai::application {

namespace {

constexpr std::size_t kMaxGroupIdLength = 64;
constexpr std::size_t kMaxPlanTitleBytes = 512;
constexpr std::size_t kMaxPlanKeyLength = 64;

common::Result<void> reject(std::string code, std::string message)
{
    return common::Result<void>::failure({std::move(code), std::move(message)});
}

bool isLowerHex64(std::string_view value) noexcept
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

bool hasAsciiControl(std::string_view text) noexcept
{
    return std::any_of(text.begin(), text.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return ch < 0x20 || ch == 0x7F;
    });
}

bool isAsciiAlnum(char ch) noexcept
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

bool validPlanKey(std::string_view key) noexcept
{
    if (key.empty() || key.size() > kMaxPlanKeyLength || !isAsciiAlnum(key.front())) return false;
    return std::all_of(key.begin(), key.end(),
        [](char ch) { return isAsciiAlnum(ch) || ch == '_' || ch == '.' || ch == '-'; });
}

std::string lowerAscii(std::string_view value)
{
    std::string out(value);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

common::Result<void> validateGroup(const RunDraftGroup& group)
{
    const auto bad = [](std::string message) { return reject("run_draft_group.invalid_group", std::move(message)); };
    if (group.id.empty() || group.id.size() > kMaxGroupIdLength) return bad("The group id must be 1-64 characters");
    if (group.projectId.empty()) return bad("The group needs a project");
    if (!isLowerHex64(group.idempotencyKey)) return bad("The idempotency key must be 64 lowercase hex characters");
    if (!isLowerHex64(group.planFingerprint)) return bad("The plan fingerprint must be 64 lowercase hex characters");
    if (group.planTitle.empty() || group.planTitle.size() > kMaxPlanTitleBytes || hasAsciiControl(group.planTitle)) {
        return bad("The plan title must be 1-512 bytes without control characters");
    }

    const bool anySource = group.sourceConversationId.has_value() || group.sourceMessageId.has_value()
        || group.sourceMessageFingerprint.has_value();
    // An origin the code does not know is refused, never read as another one (an enum can hold any number).
    if (group.origin == RunDraftGroupOrigin::ChatMessage) {
        if (!group.sourceConversationId.has_value() || group.sourceConversationId->empty()
                || !group.sourceMessageId.has_value() || group.sourceMessageId->empty()
                || !group.sourceMessageFingerprint.has_value() || !isLowerHex64(*group.sourceMessageFingerprint)) {
            return bad("A chat message group needs a conversation id, a message id and a 64-hex message fingerprint");
        }
        if (group.sourceRequestId.has_value()) return bad("A chat message group carries no follow-up request id");
    } else if (group.origin == RunDraftGroupOrigin::FollowUpRequest) {
        // The request is the source: its id, the sealed context it paid for, and the Run it was planned from.
        if (!group.sourceRequestId.has_value() || group.sourceRequestId->empty()
                || group.sourceRequestId->size() > kMaxGroupIdLength) {
            return bad("A follow-up group needs the 1-64 character id of its request");
        }
        if (anySource) return bad("A follow-up group carries no conversation, message or message fingerprint");
        if (!group.contextFingerprint.has_value()) {
            return bad("A follow-up group names the sealed context fingerprint of its request");
        }
        if (group.sourceRunIds.empty()) return bad("A follow-up group names the Run it was planned from");
    } else if (group.origin == RunDraftGroupOrigin::ManualImport) {
        if (anySource) return bad("A manual import carries no conversation, message or message fingerprint");
        if (group.sourceRequestId.has_value()) return bad("A manual import carries no follow-up request id");
        // The context fingerprint names a snapshot that was really attached to the source Chat request. A manual
        // paste has no such request, so a fingerprint here would be false provenance that can never be corrected.
        if (group.contextFingerprint.has_value()) {
            return bad("A manual import has no context fingerprint: there was no Chat request to attach a snapshot to");
        }
    } else {
        return bad("The origin of the group is unknown");
    }
    if (group.contextFingerprint.has_value() && !isLowerHex64(*group.contextFingerprint)) {
        return bad("The context fingerprint must be 64 lowercase hex characters");
    }
    if (group.sourceRunIds.size() > kRunDraftGroupMaxSourceRuns) {
        return bad("A group names at most " + std::to_string(kRunDraftGroupMaxSourceRuns) + " source runs");
    }
    std::set<std::string> seenRuns;
    for (const auto& runId : group.sourceRunIds) {
        if (runId.empty() || !seenRuns.insert(runId).second) return bad("Source run ids must be non-empty and unique");
    }
    return common::Result<void>::ok();
}

common::Result<void> validateDependencies(const std::vector<RunDraft>& drafts)
{
    const auto bad = [](std::string message) { return reject("run_draft_group.invalid_dependencies", std::move(message)); };
    std::vector<const RunDraft*> byOrdinal(drafts.size(), nullptr);
    for (const auto& draft : drafts) byOrdinal[draft.provenance->ordinal] = &draft;

    std::vector<std::vector<std::size_t>> edges(drafts.size());
    for (std::size_t i = 0; i < drafts.size(); ++i) {
        const auto& provenance = *byOrdinal[i]->provenance;
        std::set<std::string> seen;
        for (const auto& dependency : provenance.dependsOn) {
            if (dependency == provenance.planKey) return bad("Draft '" + provenance.planKey + "' depends on itself");
            std::size_t target = drafts.size();
            for (std::size_t j = 0; j < drafts.size(); ++j) {
                if (byOrdinal[j]->provenance->planKey == dependency) target = j;
            }
            if (target == drafts.size()) {
                return bad("Draft '" + provenance.planKey + "' depends on a key that is not in the group");
            }
            if (!seen.insert(dependency).second) {
                return bad("Draft '" + provenance.planKey + "' lists a dependency twice");
            }
            edges[i].push_back(target);
        }
    }

    // 0 = unvisited, 1 = on the current path, 2 = done.
    std::vector<int> state(drafts.size(), 0);
    bool cycle = false;
    const auto visit = [&](auto&& self, std::size_t node) -> void {
        if (cycle) return;
        state[node] = 1;
        for (const std::size_t next : edges[node]) {
            if (state[next] == 1) {
                cycle = true;
                return;
            }
            if (state[next] == 0) self(self, next);
            if (cycle) return;
        }
        state[node] = 2;
    };
    for (std::size_t i = 0; i < drafts.size() && !cycle; ++i) {
        if (state[i] == 0) visit(visit, i);
    }
    if (cycle) return bad("The dependencies of the drafts form a cycle");
    return common::Result<void>::ok();
}

} // namespace

const char* toString(RunDraftGroupOrigin origin) noexcept
{
    switch (origin) {
    case RunDraftGroupOrigin::ChatMessage: return "chat_message";
    case RunDraftGroupOrigin::ManualImport: return "manual_import";
    case RunDraftGroupOrigin::FollowUpRequest: return "follow_up_request";
    }
    // Never a valid origin name: an unknown enum value must not be stored (or shown) as another origin.
    return "unknown";
}

std::optional<RunDraftGroupOrigin> runDraftGroupOriginFromString(std::string_view value) noexcept
{
    if (value == "chat_message") return RunDraftGroupOrigin::ChatMessage;
    if (value == "manual_import") return RunDraftGroupOrigin::ManualImport;
    if (value == "follow_up_request") return RunDraftGroupOrigin::FollowUpRequest;
    return std::nullopt;
}

common::Result<void> validateRunDraftGroupInsertion(const RunDraftGroup& group, const std::vector<RunDraft>& drafts)
{
    if (const auto checked = validateGroup(group); !checked.success()) return checked;

    const auto badDrafts = [](std::string message) { return reject("run_draft_group.invalid_drafts", std::move(message)); };
    const auto badProvenance = [](std::string message) { return reject("run_draft_group.invalid_provenance", std::move(message)); };
    if (drafts.empty() || drafts.size() > kRunDraftGroupMaxDrafts) {
        return badDrafts("A group holds 1-" + std::to_string(kRunDraftGroupMaxDrafts) + " drafts");
    }

    std::set<std::string> ids;
    std::set<std::string> keys;
    std::vector<bool> ordinals(drafts.size(), false);
    for (const auto& draft : drafts) {
        if (draft.id.empty() || !ids.insert(draft.id).second) return badDrafts("Draft ids must be non-empty and unique");
        if (draft.projectId != group.projectId) return badDrafts("Every draft of a group belongs to the group's project");
        if (draft.launchMode != RunLaunchMode::CustomRoster || draft.workflowId != kExecutionManifestWorkflowId) {
            return badDrafts("A group draft is a per-Run roster draft on the Manifest workflow");
        }
        const auto canonical = normalizeRunInterfaceContracts(draft.interfaceContracts);
        if (!canonical.success()
                || serializeRunInterfaceContracts(canonical.value()) != serializeRunInterfaceContracts(draft.interfaceContracts)) {
            return badDrafts("The contracts of a group draft must be in canonical form");
        }

        if (!draft.provenance.has_value() || draft.provenance->groupId != group.id) {
            return badProvenance("Every draft of a group carries provenance naming that group");
        }
        const auto& provenance = *draft.provenance;
        if (provenance.ordinal >= drafts.size() || ordinals[provenance.ordinal]) {
            return badProvenance("The ordinals of a group are exactly 0..n-1, each once");
        }
        ordinals[provenance.ordinal] = true;
        if (!validPlanKey(provenance.planKey) || !keys.insert(lowerAscii(provenance.planKey)).second) {
            return badProvenance("Plan keys must be valid and unique ignoring case");
        }
        // The "edited since creation" indicator recomputes this from the current draft and compares: a value that
        // is not the fingerprint of the draft as stored would make it wrong from the first moment and, being
        // immutable, forever. The same function the plan service uses computes the expected value.
        if (provenance.createdContentFingerprint
                != nextRunPlanDraftContentFingerprint(draft.title, draft.task, draft.interfaceContracts, provenance.dependsOn)) {
            return badProvenance("The created-content fingerprint of draft '" + provenance.planKey
                + "' is not the fingerprint of its title, task, contracts and dependencies");
        }
    }
    return validateDependencies(drafts);
}

common::Result<void> checkRunDraftGroupPublicInsert(const RunDraftGroup& group)
{
    if (group.origin == RunDraftGroupOrigin::FollowUpRequest) {
        return reject("run_draft_group.follow_up_requires_confirm",
            "A group of a follow-up request is saved only by confirming that request");
    }
    return common::Result<void>::ok();
}

common::Result<void> checkRunDraftProvenanceUnchanged(
    const std::optional<RunDraftProvenance>& storedProvenance, const ProjectId& storedProjectId,
    const RunDraft& incoming)
{
    if (storedProvenance != incoming.provenance) {
        return reject("run_draft.provenance_immutable",
            "The group and plan provenance of a draft never change after it was created");
    }
    if (storedProvenance.has_value() && storedProjectId != incoming.projectId) {
        return reject("run_draft.group_project_immutable", "A draft of a group cannot move to another project");
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
