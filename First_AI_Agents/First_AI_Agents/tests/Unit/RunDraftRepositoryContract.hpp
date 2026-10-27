#pragma once

// PC-3B: ONE contract suite for IRunDraftRepository drafts and groups. The SQLite adapter and the in-memory adapter
// run exactly the same scenarios, so a difference between them is a test failure, not a surprise. Anything that
// depends on SQLite (foreign keys, triggers, rollback of a half-written group, migrations) lives in the SQLite
// tests instead.

#include "ai/application/planning/IRunDraftRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/planning/NextRunPlan.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"
#include "ai/application/RunInterfaceContract.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rundraft_contract {

using namespace ai::application;

struct Env {
    std::shared_ptr<void> keepAlive;          // owns the repository and whatever backs it
    IRunDraftRepository* repository {nullptr};
    std::string projectId;                    // an existing project
    std::string otherProjectId;               // a second existing project
    std::string providerId;                   // existing provider / models for the roster rows
    std::string modelA;
    std::string modelB;
};
using EnvFactory = std::function<Env()>;

inline void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

inline std::string hex64(char digit) { return std::string(64, digit); }

// Fixed millisecond timestamps: SQLite stores milliseconds, so a round trip is exact.
inline std::chrono::system_clock::time_point at(std::int64_t offsetMs)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'000 + offsetMs));
}

inline StageModelAssignment rosterEntry(const Env& env, std::string stageType, std::uint32_t position)
{
    StageModelAssignment entry;
    entry.stageType = std::move(stageType);
    entry.providerId = env.providerId;
    entry.modelId = position % 2 == 0 ? env.modelA : env.modelB;
    entry.timeout = std::chrono::milliseconds(60'000);
    entry.agentName = "Agent " + std::to_string(position);
    entry.role = position == 0 ? "lead" : "worker";
    entry.instructions = "Do the thing " + std::to_string(position) + ".";
    entry.position = position;
    entry.leadArchitect = position == 0;
    entry.maxAttempts = 1;
    entry.workspaceAccess = position != 0;
    return entry;
}

inline RunInterfaceContract contract(std::string id, std::string text)
{
    RunInterfaceContract value;
    value.id = std::move(id);
    value.title = "Contract " + value.id;
    value.text = std::move(text);
    value.appliesToPaths = {"qml/models"};
    return value;
}

// A manual draft: roster, contracts, no provenance.
inline RunDraft manualDraft(const Env& env, std::string id)
{
    RunDraft draft;
    draft.id = std::move(id);
    draft.name = "Manual " + draft.id;
    draft.projectId = env.projectId;
    draft.title = "Manual title";
    draft.task = "Manual task";
    draft.workflowId = kExecutionManifestWorkflowId;
    draft.launchMode = RunLaunchMode::CustomRoster;
    draft.roster = {rosterEntry(env, "lead", 0), rosterEntry(env, "worker", 1)};
    draft.maxParallelism = 2;
    draft.interfaceContracts = {contract("M1", "property string metricId")};
    draft.createdAt = at(10);
    draft.updatedAt = at(20);
    return draft;
}

// The created-content fingerprint a draft of a group must carry: the one of its CURRENT title, task, contracts and
// dependencies. A test that changes any of them after building the draft calls this again.
inline void reseal(RunDraft& draft)
{
    check(draft.provenance.has_value(), "Only a draft of a group is sealed");
    draft.provenance->createdContentFingerprint = nextRunPlanDraftContentFingerprint(
        draft.title, draft.task, draft.interfaceContracts, draft.provenance->dependsOn);
}

// A draft of the group `groupId`, sealed with the fingerprint of its content.
inline RunDraft groupDraft(const Env& env, const std::string& groupId, std::string id, std::uint32_t ordinal,
    std::string planKey, std::vector<std::string> dependsOn = {})
{
    RunDraft draft;
    draft.id = std::move(id);
    draft.name = "Draft " + planKey;
    draft.projectId = env.projectId;
    draft.title = "Title " + planKey;
    draft.task = "Task for " + planKey;
    draft.workflowId = kExecutionManifestWorkflowId;
    draft.launchMode = RunLaunchMode::CustomRoster;
    draft.maxParallelism = 3;
    draft.createdAt = at(100 + ordinal);
    draft.updatedAt = at(200 + ordinal);
    RunDraftProvenance provenance;
    provenance.groupId = groupId;
    provenance.ordinal = ordinal;
    provenance.planKey = std::move(planKey);
    provenance.dependsOn = std::move(dependsOn);
    draft.provenance = std::move(provenance);
    reseal(draft);
    return draft;
}

inline RunDraftGroup makeGroup(const Env& env, std::string id, char keyDigit,
    RunDraftGroupOrigin origin = RunDraftGroupOrigin::ManualImport)
{
    RunDraftGroup group;
    group.id = std::move(id);
    group.projectId = env.projectId;
    group.origin = origin;
    group.idempotencyKey = hex64(keyDigit);
    group.planTitle = "History page";
    group.planFingerprint = hex64('b');
    if (origin == RunDraftGroupOrigin::ChatMessage) {
        group.sourceConversationId = "conversation-1";
        group.sourceMessageId = "message-1";
        group.sourceMessageFingerprint = hex64('c');
    }
    group.createdAt = at(50);
    return group;
}

inline bool sameRoster(const std::vector<StageModelAssignment>& left, const std::vector<StageModelAssignment>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto& a = left[i];
        const auto& b = right[i];
        if (a.stageType != b.stageType || a.providerId != b.providerId || a.modelId != b.modelId
                || a.timeout != b.timeout || a.agentName != b.agentName || a.role != b.role
                || a.instructions != b.instructions || a.position != b.position || a.leadArchitect != b.leadArchitect
                || a.maxAttempts != b.maxAttempts || a.workspaceAccess != b.workspaceAccess) {
            return false;
        }
    }
    return true;
}

inline bool sameDraft(const RunDraft& a, const RunDraft& b)
{
    return a.id == b.id && a.name == b.name && a.projectId == b.projectId && a.title == b.title && a.task == b.task
        && a.workflowId == b.workflowId && a.launchMode == b.launchMode && a.maxParallelism == b.maxParallelism
        && a.createdAt == b.createdAt && a.updatedAt == b.updatedAt && sameRoster(a.roster, b.roster)
        && serializeRunInterfaceContracts(a.interfaceContracts) == serializeRunInterfaceContracts(b.interfaceContracts)
        && a.provenance == b.provenance;
}

inline bool sameGroup(const RunDraftGroup& a, const RunDraftGroup& b)
{
    return a.id == b.id && a.projectId == b.projectId && a.origin == b.origin && a.idempotencyKey == b.idempotencyKey
        && a.planTitle == b.planTitle && a.planFingerprint == b.planFingerprint
        && a.sourceConversationId == b.sourceConversationId && a.sourceMessageId == b.sourceMessageId
        && a.sourceMessageFingerprint == b.sourceMessageFingerprint && a.sourceRequestId == b.sourceRequestId && a.contextFingerprint == b.contextFingerprint
        && a.sourceRunIds == b.sourceRunIds && a.createdAt == b.createdAt;
}

// Everything the repository holds that a refused insertion must leave alone.
struct Snapshot {
    std::vector<std::string> draftIds;            // listAll, sorted
    std::vector<std::string> groupDraftIds;       // listGroupDrafts of the probed group, in order
    bool probedGroupExists {false};
    bool operator==(const Snapshot&) const = default;
};

inline Snapshot snapshot(const IRunDraftRepository& repository, const std::string& probedGroupId)
{
    Snapshot out;
    const auto all = repository.listAll();
    check(all.success(), "listAll must work");
    for (const auto& draft : all.value()) out.draftIds.push_back(draft.id);
    std::sort(out.draftIds.begin(), out.draftIds.end());
    const auto group = repository.findGroupById(probedGroupId);
    check(group.success(), "findGroupById must work");
    out.probedGroupExists = group.value().has_value();
    const auto drafts = repository.listGroupDrafts(probedGroupId);
    check(drafts.success(), "listGroupDrafts must work");
    for (const auto& draft : drafts.value()) out.groupDraftIds.push_back(draft.id);
    return out;
}

inline std::string codeOf(const ai::common::Result<std::vector<RunDraft>>& result)
{
    check(!result.success() && result.error().has_value(), "The call must fail");
    return result.error()->code;
}

// ---------------------------------------------------------------------------------------------------------------

inline void manualDraftsAreNotGrouped(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    RunDraft draft = manualDraft(env, "manual-1");
    check(repo.insert(draft).success(), "A manual draft must insert");
    auto found = repo.findById("manual-1");
    check(found.success() && found.value().has_value() && !found.value()->provenance.has_value()
            && sameDraft(*found.value(), draft),
        "A manual draft has no provenance and round-trips with its roster and contracts");

    draft.title = "Edited";
    draft.task = "Edited task";
    draft.roster.push_back(rosterEntry(env, "extra", 2));
    check(repo.update(draft).success(), "A manual draft must update");
    found = repo.findById("manual-1");
    check(found.success() && found.value().has_value() && found.value()->title == "Edited"
            && found.value()->roster.size() == 3 && !found.value()->provenance.has_value(),
        "An update of a manual draft keeps working and keeps it manual");

    check(repo.findGroupById("nope").success() && !repo.findGroupById("nope").value().has_value(), "An unknown group");
    check(repo.findGroupByIdempotencyKey(hex64('a')).success()
            && !repo.findGroupByIdempotencyKey(hex64('a')).value().has_value(),
        "An unknown idempotency key");
    check(repo.listGroupDrafts("nope").success() && repo.listGroupDrafts("nope").value().empty(),
        "An unknown group has no drafts");
    check(repo.listAll().value().size() == 1, "listAll sees the manual draft");

    check(repo.remove("manual-1").success(), "A manual draft must remove");
    const auto again = repo.remove("manual-1");
    check(!again.success() && again.error()->code == "run_draft.not_found", "Removing it again is not_found");
    check(repo.update(draft).error()->code == "run_draft.not_found", "Updating a removed draft is not_found");
}

inline void insertGroupRoundTrips(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    check(repo.insert(manualDraft(env, "manual-before")).success(), "A manual draft before the group");

    RunDraftGroup group = makeGroup(env, "group-1", 'a', RunDraftGroupOrigin::ChatMessage);
    group.contextFingerprint = hex64('d');
    group.sourceRunIds = {"run-1", "run-2"};

    RunDraft model = groupDraft(env, "group-1", "draft-model", 0, "model");
    model.roster = {rosterEntry(env, "lead", 0), rosterEntry(env, "worker", 1)};
    model.interfaceContracts = {contract("S1", "property string metricId\nproperty real share")};
    reseal(model);
    RunDraft view = groupDraft(env, "group-1", "draft-view", 1, "view", {"model"});
    view.interfaceContracts = {contract("S1", "property string metricId\nproperty real share"), contract("V1", "signal x()")};
    reseal(view);
    RunDraft docs = groupDraft(env, "group-1", "draft-docs", 2, "docs", {"model", "view"});

    // Handed over out of order: the repository orders them by ordinal.
    const auto inserted = repo.insertGroup(group, {docs, model, view});
    check(inserted.success(), "A valid group must insert: " + (inserted.error() ? inserted.error()->message : ""));
    check(inserted.value().size() == 3 && inserted.value()[0].id == "draft-model" && inserted.value()[1].id == "draft-view"
            && inserted.value()[2].id == "draft-docs",
        "insertGroup returns the drafts in ordinal order");

    const auto foundGroup = repo.findGroupById("group-1");
    check(foundGroup.success() && foundGroup.value().has_value() && sameGroup(*foundGroup.value(), group),
        "The group round-trips with every field");
    const auto byKey = repo.findGroupByIdempotencyKey(group.idempotencyKey);
    check(byKey.success() && byKey.value().has_value() && byKey.value()->id == "group-1", "Found by idempotency key");
    check(!repo.findGroupByIdempotencyKey(hex64('e')).value().has_value(), "Another key finds nothing");

    const auto listed = repo.listGroupDrafts("group-1");
    check(listed.success() && listed.value().size() == 3, "listGroupDrafts returns the three drafts");
    const std::vector<RunDraft> expected = {model, view, docs};
    for (std::size_t i = 0; i < 3; ++i) {
        check(sameDraft(listed.value()[i], expected[i]), "Draft " + std::to_string(i) + " round-trips completely");
        const auto single = repo.findById(expected[i].id);
        check(single.success() && single.value().has_value() && sameDraft(*single.value(), expected[i]),
            "findById returns the draft with its provenance");
    }
    check(listed.value()[2].provenance->dependsOn == (std::vector<std::string> {"model", "view"}),
        "The dependency keys keep their order");

    // A second group of the other origin, and a manual draft after it: all independent.
    RunDraftGroup manual = makeGroup(env, "group-2", 'f', RunDraftGroupOrigin::ManualImport);
    check(repo.insertGroup(manual, {groupDraft(env, "group-2", "draft-solo", 0, "model")}).success(),
        "The same plan key is fine in another group");
    const auto foundManual = repo.findGroupById("group-2");
    check(foundManual.success() && foundManual.value().has_value() && sameGroup(*foundManual.value(), manual)
            && !foundManual.value()->sourceMessageId.has_value() && foundManual.value()->sourceRunIds.empty(),
        "A manual import group has no source message and no source runs");
    check(repo.insert(manualDraft(env, "manual-after")).success(), "A manual draft after the groups");
    const auto all = repo.listAll();
    check(all.success() && all.value().size() == 6, "listAll sees manual and grouped drafts together");
    std::size_t grouped = 0;
    for (const auto& draft : all.value()) grouped += draft.provenance.has_value() ? 1 : 0;
    check(grouped == 4, "Exactly the four group drafts carry provenance");
    check(repo.listGroupDrafts("group-1").value().size() == 3 && repo.listGroupDrafts("group-2").value().size() == 1,
        "The groups stay separate");
}

inline void refusedInsertionsLeaveNothing(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    check(repo.insert(manualDraft(env, "manual-1")).success(), "setup: a manual draft");
    check(repo.insertGroup(makeGroup(env, "group-existing", 'a'),
                           {groupDraft(env, "group-existing", "existing-0", 0, "model"),
                            groupDraft(env, "group-existing", "existing-1", 1, "view", {"model"})})
              .success(),
        "setup: an existing group");
    const Snapshot before = snapshot(repo, "group-existing");

    const auto fresh = [&](const std::string& groupId, std::size_t count) {
        std::vector<RunDraft> drafts;
        for (std::size_t i = 0; i < count; ++i) {
            drafts.push_back(groupDraft(env, groupId, groupId + "-d" + std::to_string(i),
                static_cast<std::uint32_t>(i), "k" + std::to_string(i)));
        }
        return drafts;
    };
    const auto expectRefusal = [&](const std::string& what, RunDraftGroup group, std::vector<RunDraft> drafts,
                                   const std::string& code) {
        const auto result = repo.insertGroup(std::move(group), std::move(drafts));
        check(!result.success() && result.error()->code == code,
            what + ": expected " + code + " but got " + (result.error() ? result.error()->code : "success"));
        check(snapshot(repo, "group-existing") == before, what + ": the refusal must leave the repository unchanged");
    };

    // Collisions that only the store can see. In SQLite the group row and the earlier drafts are already written
    // when the last draft collides, so this is also the rollback proof.
    {
        auto drafts = fresh("g-a", 3);
        drafts[2].id = "manual-1";
        expectRefusal("last draft id equals a manual draft", makeGroup(env, "g-a", 'b'), drafts, "run_draft.duplicate_id");
    }
    {
        auto drafts = fresh("g-b", 3);
        drafts[1].id = "existing-1";
        expectRefusal("a draft id equals a grouped draft", makeGroup(env, "g-b", 'c'), drafts, "run_draft.duplicate_id");
    }
    expectRefusal("same idempotency key", makeGroup(env, "g-c", 'a'), fresh("g-c", 2),
        "run_draft_group.duplicate_idempotency_key");
    expectRefusal("same group id", makeGroup(env, "group-existing", 'd'), fresh("group-existing", 2),
        "run_draft_group.duplicate_id");

    // Refused by the shared validation, before anything is written.
    const auto groupWith = [&](auto&& edit) {
        RunDraftGroup group = makeGroup(env, "g-v", 'e', RunDraftGroupOrigin::ChatMessage);
        edit(group);
        return group;
    };
    // The edit is applied first and the drafts are sealed again afterwards, so only the rule under test is violated -
    // except when the test is about the seal itself (`sealAfter` false).
    const auto draftsWith = [&](auto&& edit, std::size_t count = 3, bool sealAfter = true) {
        auto drafts = fresh("g-v", count);
        edit(drafts);
        if (sealAfter) {
            for (auto& draft : drafts) {
                if (draft.provenance.has_value()) reseal(draft);
            }
        }
        return drafts;
    };
    const auto nothing = [](auto&) {};

    expectRefusal("no drafts", groupWith(nothing), {}, "run_draft_group.invalid_drafts");
    expectRefusal("nine drafts", groupWith(nothing), draftsWith(nothing, 9), "run_draft_group.invalid_drafts");
    expectRefusal("a draft in another project", groupWith(nothing),
        draftsWith([&](auto& d) { d[1].projectId = env.otherProjectId; }), "run_draft_group.invalid_drafts");
    expectRefusal("a mock draft", groupWith(nothing), draftsWith([](auto& d) { d[1].launchMode = RunLaunchMode::Mock; }),
        "run_draft_group.invalid_drafts");
    expectRefusal("a profile draft", groupWith(nothing),
        draftsWith([](auto& d) { d[0].launchMode = RunLaunchMode::SavedExecutionProfile; }), "run_draft_group.invalid_drafts");
    expectRefusal("no Manifest workflow", groupWith(nothing), draftsWith([](auto& d) { d[2].workflowId.clear(); }),
        "run_draft_group.invalid_drafts");
    expectRefusal("duplicate draft ids in the batch", groupWith(nothing),
        draftsWith([](auto& d) { d[1].id = d[0].id; }), "run_draft_group.invalid_drafts");
    expectRefusal("contracts not in canonical form", groupWith(nothing),
        draftsWith([&](auto& d) { d[0].interfaceContracts = {contract("C1", "x")}; d[0].interfaceContracts[0].title = "  padded  "; }),
        "run_draft_group.invalid_drafts");
    expectRefusal("a draft without provenance", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance.reset(); }), "run_draft_group.invalid_provenance");
    expectRefusal("provenance of another group", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance->groupId = "someone-else"; }), "run_draft_group.invalid_provenance");
    expectRefusal("a gap in the ordinals", groupWith(nothing),
        draftsWith([](auto& d) { d[2].provenance->ordinal = 3; }), "run_draft_group.invalid_provenance");
    expectRefusal("a repeated ordinal", groupWith(nothing),
        draftsWith([](auto& d) { d[2].provenance->ordinal = 1; }), "run_draft_group.invalid_provenance");
    expectRefusal("plan keys equal ignoring case", groupWith(nothing),
        draftsWith([](auto& d) { d[0].provenance->planKey = "K1"; d[1].provenance->planKey = "k1"; }),
        "run_draft_group.invalid_provenance");
    expectRefusal("an invalid plan key", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance->planKey = "_bad"; }), "run_draft_group.invalid_provenance");
    // The created-content fingerprint must be the one of the content being inserted (never re-sealed in these cases).
    expectRefusal("a malformed content fingerprint", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance->createdContentFingerprint = "abc"; }, 3, false),
        "run_draft_group.invalid_provenance");
    expectRefusal("a well-formed fingerprint of nothing", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance->createdContentFingerprint = std::string(64, 'f'); }, 3, false),
        "run_draft_group.invalid_provenance");
    expectRefusal("a fingerprint of another title", groupWith(nothing),
        draftsWith([](auto& d) { d[1].title = "Changed title"; }, 3, false), "run_draft_group.invalid_provenance");
    expectRefusal("a fingerprint of another task", groupWith(nothing),
        draftsWith([](auto& d) { d[1].task = "Changed task"; }, 3, false), "run_draft_group.invalid_provenance");
    expectRefusal("a fingerprint of other contracts", groupWith(nothing),
        draftsWith([](auto& d) { d[1].interfaceContracts = {contract("C9", "other")}; }, 3, false),
        "run_draft_group.invalid_provenance");
    expectRefusal("a fingerprint of other dependencies", groupWith(nothing),
        draftsWith([](auto& d) { d[2].provenance->dependsOn = {"k0"}; }, 3, false), "run_draft_group.invalid_provenance");
    expectRefusal("fingerprints swapped between two drafts", groupWith(nothing),
        draftsWith([](auto& d) { std::swap(d[0].provenance->createdContentFingerprint, d[1].provenance->createdContentFingerprint); },
            3, false),
        "run_draft_group.invalid_provenance");
    expectRefusal("an unknown dependency", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance->dependsOn = {"nope"}; }), "run_draft_group.invalid_dependencies");
    expectRefusal("a dependency in another case", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance->dependsOn = {"K0"}; }), "run_draft_group.invalid_dependencies");
    expectRefusal("a self dependency", groupWith(nothing),
        draftsWith([](auto& d) { d[1].provenance->dependsOn = {"k1"}; }), "run_draft_group.invalid_dependencies");
    expectRefusal("a repeated dependency", groupWith(nothing),
        draftsWith([](auto& d) { d[2].provenance->dependsOn = {"k0", "k0"}; }), "run_draft_group.invalid_dependencies");
    expectRefusal("a dependency cycle", groupWith(nothing),
        draftsWith([](auto& d) {
            d[0].provenance->dependsOn = {"k2"};
            d[1].provenance->dependsOn = {"k0"};
            d[2].provenance->dependsOn = {"k1"};
        }),
        "run_draft_group.invalid_dependencies");
    expectRefusal("a bad idempotency key", groupWith([](auto& g) { g.idempotencyKey = "short"; }), draftsWith(nothing),
        "run_draft_group.invalid_group");
    expectRefusal("an uppercase idempotency key", groupWith([](auto& g) { g.idempotencyKey = std::string(64, 'A'); }),
        draftsWith(nothing), "run_draft_group.invalid_group");
    expectRefusal("a bad plan fingerprint", groupWith([](auto& g) { g.planFingerprint = "x"; }), draftsWith(nothing),
        "run_draft_group.invalid_group");
    expectRefusal("an empty plan title", groupWith([](auto& g) { g.planTitle.clear(); }), draftsWith(nothing),
        "run_draft_group.invalid_group");
    expectRefusal("a plan title over 512 bytes", groupWith([](auto& g) { g.planTitle = std::string(513, 't'); }),
        draftsWith(nothing), "run_draft_group.invalid_group");
    expectRefusal("a chat group without a message id", groupWith([](auto& g) { g.sourceMessageId.reset(); }),
        draftsWith(nothing), "run_draft_group.invalid_group");
    expectRefusal("a chat group without a message fingerprint",
        groupWith([](auto& g) { g.sourceMessageFingerprint.reset(); }), draftsWith(nothing), "run_draft_group.invalid_group");
    expectRefusal("a manual group that names a message",
        groupWith([](auto& g) { g.origin = RunDraftGroupOrigin::ManualImport; }), draftsWith(nothing),
        "run_draft_group.invalid_group");
    expectRefusal("a bad context fingerprint", groupWith([](auto& g) { g.contextFingerprint = "zz"; }), draftsWith(nothing),
        "run_draft_group.invalid_group");
    // A manual import has no Chat request, so it has no attached snapshot to fingerprint.
    expectRefusal("a manual import with a context fingerprint", groupWith([](auto& g) {
        g.origin = RunDraftGroupOrigin::ManualImport;
        g.sourceConversationId.reset();
        g.sourceMessageId.reset();
        g.sourceMessageFingerprint.reset();
        g.contextFingerprint = std::string(64, 'd');
    }), draftsWith(nothing), "run_draft_group.invalid_group");
    // An enum can hold a number no enumerator names: it is refused, never read as another origin.
    for (const int raw : {-1, 3, 7, 255}) {
        expectRefusal("an unknown origin " + std::to_string(raw),
            groupWith([raw](auto& g) { g.origin = static_cast<RunDraftGroupOrigin>(raw); }), draftsWith(nothing),
            "run_draft_group.invalid_group");
    }
    expectRefusal("an unknown origin that looks like a manual import", groupWith([](auto& g) {
        g.origin = static_cast<RunDraftGroupOrigin>(9);
        g.sourceConversationId.reset();
        g.sourceMessageId.reset();
        g.sourceMessageFingerprint.reset();
    }), draftsWith(nothing), "run_draft_group.invalid_group");
    expectRefusal("too many source runs", groupWith([](auto& g) {
        for (int i = 0; i < 17; ++i) g.sourceRunIds.push_back("run-" + std::to_string(i));
    }), draftsWith(nothing), "run_draft_group.invalid_group");
    expectRefusal("repeated source runs", groupWith([](auto& g) { g.sourceRunIds = {"run-1", "run-1"}; }),
        draftsWith(nothing), "run_draft_group.invalid_group");
    expectRefusal("an empty group id", groupWith([](auto& g) { g.id.clear(); }), draftsWith(nothing),
        "run_draft_group.invalid_group");

    // The refusals did not poison the repository: a valid group still goes in.
    check(repo.insertGroup(makeGroup(env, "g-ok", 'f'), fresh("g-ok", 8)).success(), "Eight drafts are the maximum");
}

inline void provenanceIsImmutable(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    check(repo.insert(manualDraft(env, "manual-1")).success(), "setup: a manual draft");
    check(repo.insertGroup(makeGroup(env, "group-1", 'a'),
                           {groupDraft(env, "group-1", "d0", 0, "model"), groupDraft(env, "group-1", "d1", 1, "view", {"model"})})
              .success(),
        "setup: a group");
    const auto stored = repo.findById("d1");
    check(stored.success() && stored.value().has_value(), "setup: the grouped draft");
    const RunDraft original = *stored.value();

    // An ordinary update changes the editable fields and leaves group, ordinal, key, dependencies and fingerprint.
    RunDraft edited = original;
    edited.name = "Renamed";
    edited.title = "New title";
    edited.task = "New task";
    edited.roster = {rosterEntry(env, "lead", 0), rosterEntry(env, "worker", 1)};
    edited.interfaceContracts = {contract("E1", "edited text")};
    edited.maxParallelism = 5;
    edited.updatedAt = at(900);
    check(repo.update(edited).success(), "An ordinary update of a grouped draft must work");
    auto afterEdit = repo.findById("d1");
    check(afterEdit.success() && afterEdit.value().has_value() && sameDraft(*afterEdit.value(), edited)
            && afterEdit.value()->provenance == original.provenance,
        "The edit sticks and the provenance is exactly the original");
    check(repo.listGroupDrafts("group-1").value()[1].title == "New title", "The group sees the edit");

    // Any other provenance is refused, and nothing is written.
    const auto refuse = [&](const std::string& what, const std::function<void(RunDraft&)>& change, const std::string& code) {
        RunDraft attempt = edited;
        attempt.title = "Must not be written";
        change(attempt);
        const auto result = repo.update(attempt);
        check(!result.success() && result.error()->code == code,
            what + ": expected " + code + " but got " + (result.error() ? result.error()->code : "success"));
        const auto reread = repo.findById("d1");
        check(reread.success() && reread.value().has_value() && sameDraft(*reread.value(), edited),
            what + ": nothing may be written by a refused update");
    };
    refuse("another group", [](RunDraft& d) { d.provenance->groupId = "group-2"; }, "run_draft.provenance_immutable");
    refuse("another ordinal", [](RunDraft& d) { d.provenance->ordinal = 7; }, "run_draft.provenance_immutable");
    refuse("another plan key", [](RunDraft& d) { d.provenance->planKey = "other"; }, "run_draft.provenance_immutable");
    refuse("other dependencies", [](RunDraft& d) { d.provenance->dependsOn.clear(); }, "run_draft.provenance_immutable");
    refuse("another content fingerprint", [](RunDraft& d) { d.provenance->createdContentFingerprint = hex64('9'); },
        "run_draft.provenance_immutable");
    refuse("dropped provenance", [](RunDraft& d) { d.provenance.reset(); }, "run_draft.provenance_immutable");
    refuse("another project", [&](RunDraft& d) { d.projectId = env.otherProjectId; }, "run_draft.group_project_immutable");

    // A manual draft cannot be turned into a grouped one by an update either.
    auto manual = repo.findById("manual-1").value().value();
    RunDraft attach = manual;
    attach.provenance = original.provenance;
    const auto attached = repo.update(attach);
    check(!attached.success() && attached.error()->code == "run_draft.provenance_immutable",
        "Attaching provenance to a manual draft is refused");
    check(!repo.findById("manual-1").value()->provenance.has_value(), "The manual draft stays manual");
    // ...but a manual draft may still change project.
    manual.projectId = env.otherProjectId;
    check(repo.update(manual).success(), "A manual draft keeps its freedom to move to another project");
}

inline void groupedDraftsEnterOnlyThroughInsertGroup(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    const auto result = repo.insert(groupDraft(env, "group-1", "loose", 0, "model"));
    check(!result.success() && result.error()->code == "run_draft.group_insert_required",
        "insert refuses a draft that carries provenance");
    check(repo.listAll().value().empty() && !repo.findGroupById("group-1").value().has_value(),
        "...and writes nothing");
}

inline void removingTheLastDraftRemovesTheGroup(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    RunDraftGroup group = makeGroup(env, "group-1", 'a');
    check(repo.insertGroup(group, {groupDraft(env, "group-1", "d0", 0, "model"),
                                   groupDraft(env, "group-1", "d1", 1, "view", {"model"}),
                                   groupDraft(env, "group-1", "d2", 2, "docs", {"model", "view"})})
              .success(),
        "setup: a group of three");
    check(repo.insert(manualDraft(env, "manual-1")).success(), "setup: a manual draft");

    // Removing one draft leaves the group and its other drafts alone (the dependencies are not rewritten).
    check(repo.remove("d1").success(), "A draft of a group removes");
    check(repo.findGroupById("group-1").value().has_value(), "The group stays while it has drafts");
    auto rest = repo.listGroupDrafts("group-1");
    check(rest.success() && rest.value().size() == 2 && rest.value()[0].id == "d0" && rest.value()[1].id == "d2",
        "The other drafts stay, in ordinal order");
    check(rest.value()[1].provenance->dependsOn == (std::vector<std::string> {"model", "view"}),
        "A removed draft is not erased from the dependencies of the others");
    check(repo.findGroupByIdempotencyKey(group.idempotencyKey).value().has_value(),
        "The idempotency key still resolves while the group lives");

    // A refused removal changes nothing.
    const auto missing = repo.remove("d1");
    check(!missing.success() && missing.error()->code == "run_draft.not_found", "Removing it again is not_found");
    check(repo.findGroupById("group-1").value().has_value() && repo.listGroupDrafts("group-1").value().size() == 2,
        "A refused removal leaves the group alone");

    // Removing a manual draft never touches a group.
    check(repo.remove("manual-1").success(), "A manual draft removes");
    check(repo.findGroupById("group-1").value().has_value(), "...and the group is untouched");

    // The last draft takes the group with it, in the same step.
    check(repo.remove("d0").success(), "Another draft removes");
    check(repo.findGroupById("group-1").value().has_value(), "One draft left: the group stays");
    check(repo.remove("d2").success(), "The last draft removes");
    check(!repo.findGroupById("group-1").value().has_value(), "The group is gone with its last draft");
    check(!repo.findGroupByIdempotencyKey(group.idempotencyKey).value().has_value(),
        "...and its idempotency key no longer resolves");
    check(repo.listGroupDrafts("group-1").value().empty() && repo.listAll().value().empty(), "Nothing is left");

    // So the same plan can be imported again: a fresh group, with the same key and even the same group id.
    check(repo.insertGroup(group, {groupDraft(env, "group-1", "d0", 0, "model")}).success(),
        "After the group is gone the same idempotency key and id can be used again");
    check(repo.findGroupByIdempotencyKey(group.idempotencyKey).value().has_value(), "The new group is found");
}

inline void groupsAreIsolatedFromEachOther(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    check(repo.insertGroup(makeGroup(env, "group-a", 'a'), {groupDraft(env, "group-a", "a0", 0, "model"),
                                                              groupDraft(env, "group-a", "a1", 1, "view", {"model"})})
              .success(),
        "setup: group A");
    check(repo.insertGroup(makeGroup(env, "group-b", 'b'), {groupDraft(env, "group-b", "b0", 0, "model")}).success(),
        "setup: group B uses the same plan keys and ordinals");
    check(repo.remove("b0").success(), "Removing the only draft of group B");
    check(!repo.findGroupById("group-b").value().has_value() && repo.findGroupById("group-a").value().has_value()
            && repo.listGroupDrafts("group-a").value().size() == 2,
        "Group A is untouched by the disappearance of group B");
    check(repo.listGroupDrafts("group-b").value().empty(), "Group B has no drafts");
}

// The edges of every rule: what is exactly allowed, and what is one step beyond.
inline void groupValidationBoundaries(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    const std::string allHex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    const auto base = [&](std::size_t count = 2) {
        std::vector<RunDraft> drafts;
        for (std::size_t i = 0; i < count; ++i) {
            drafts.push_back(groupDraft(env, "g", "d" + std::to_string(i), static_cast<std::uint32_t>(i), "k" + std::to_string(i)));
        }
        return drafts;
    };
    // Dependencies and contracts are part of a draft's content: an edit of them is followed by a new seal.
    const auto seal = [](std::vector<RunDraft>& drafts) {
        for (auto& draft : drafts) reseal(draft);
    };
    const auto accepted = [&](const std::string& what, const RunDraftGroup& group, const std::vector<RunDraft>& drafts) {
        const auto result = repo.insertGroup(group, drafts);
        check(result.success(), what + ": must be accepted but was " + (result.error() ? result.error()->code + " " + result.error()->message : ""));
        for (const auto& draft : result.value()) check(repo.remove(draft.id).success(), what + ": cleanup");
        check(repo.listAll().value().empty(), what + ": cleanup leaves nothing");
    };
    const auto refused = [&](const std::string& what, const RunDraftGroup& group, const std::vector<RunDraft>& drafts,
                             const std::string& code) {
        const auto result = repo.insertGroup(group, drafts);
        check(!result.success() && result.error()->code == code,
            what + ": expected " + code + " but got " + (result.error() ? result.error()->code : "success"));
        check(repo.listAll().value().empty(), what + ": a refusal writes nothing");
    };
    const auto group = [&](char key = 'a', RunDraftGroupOrigin origin = RunDraftGroupOrigin::ManualImport) {
        return makeGroup(env, "g", key, origin);
    };

    // Group fields: exactly at the limit is fine, one step beyond is not.
    {
        auto g = group(); g.id = std::string(64, 'i');
        auto drafts = base();
        for (auto& d : drafts) d.provenance->groupId = g.id;
        accepted("a group id of 64 characters", g, drafts);
        g.id = std::string(65, 'i');
        for (auto& d : drafts) d.provenance->groupId = g.id;
        refused("a group id of 65 characters", g, drafts, "run_draft_group.invalid_group");
    }
    {
        auto g = group(); g.idempotencyKey = allHex; g.planFingerprint = allHex;
        accepted("every hex digit in the keys", g, base());
        g.idempotencyKey = std::string(65, 'a');
        refused("a 65-character key", g, base(), "run_draft_group.invalid_group");
        g.idempotencyKey = std::string(63, 'a') + "g";
        refused("a non-hex key", g, base(), "run_draft_group.invalid_group");
        g.idempotencyKey = std::string(63, 'a') + "F";
        refused("an uppercase hex digit in the key", g, base(), "run_draft_group.invalid_group");
        g = group(); g.planFingerprint = std::string(63, 'a') + "g";
        refused("a non-hex plan fingerprint", g, base(), "run_draft_group.invalid_group");
        g = group(); g.projectId.clear();
        refused("no project", g, base(), "run_draft_group.invalid_group");
    }
    {
        auto g = group(); g.planTitle = std::string(512, 't');
        accepted("a plan title of 512 bytes", g, base());
        g.planTitle = "line\nbreak";
        refused("a newline in the plan title", g, base(), "run_draft_group.invalid_group");
        g.planTitle = std::string("del") + '\x7F';
        refused("DEL in the plan title", g, base(), "run_draft_group.invalid_group");
    }
    {
        auto g = group('a', RunDraftGroupOrigin::ChatMessage);
        g.sourceMessageFingerprint = allHex; g.contextFingerprint = allHex;
        accepted("a chat group with every hex digit", g, base());
        g.sourceConversationId = "";
        refused("a chat group with an empty conversation id", g, base(), "run_draft_group.invalid_group");
        g = group('a', RunDraftGroupOrigin::ChatMessage); g.sourceMessageId = "";
        refused("a chat group with an empty message id", g, base(), "run_draft_group.invalid_group");
        g = group('a', RunDraftGroupOrigin::ChatMessage); g.sourceMessageFingerprint = std::string(63, 'a') + "g";
        refused("a chat group with a non-hex message fingerprint", g, base(), "run_draft_group.invalid_group");
        g = group('a', RunDraftGroupOrigin::ChatMessage); g.sourceConversationId.reset();
        refused("a chat group without a conversation", g, base(), "run_draft_group.invalid_group");
        g = group(); g.sourceConversationId = "c";
        refused("a manual group naming only a conversation", g, base(), "run_draft_group.invalid_group");
        g = group(); g.sourceMessageFingerprint = hex64('c');
        refused("a manual group naming only a message fingerprint", g, base(), "run_draft_group.invalid_group");
        // A manual paste has no Chat request, hence no attached snapshot and no context fingerprint.
        g = group(); g.contextFingerprint = allHex;
        refused("a manual group with a context fingerprint", g, base(), "run_draft_group.invalid_group");
        g = group('a', RunDraftGroupOrigin::ChatMessage); g.contextFingerprint = allHex;
        accepted("a chat group with a context fingerprint", g, base());
        g.contextFingerprint = std::string(63, 'a') + "G";
        refused("an uppercase context fingerprint", g, base(), "run_draft_group.invalid_group");
        g.contextFingerprint = std::string(65, 'a');
        refused("a 65-character context fingerprint", g, base(), "run_draft_group.invalid_group");
        g = group('a', RunDraftGroupOrigin::ChatMessage);
        accepted("a chat group without a context fingerprint", g, base());
        g = group(); g.origin = static_cast<RunDraftGroupOrigin>(3);
        refused("an origin no enumerator names", g, base(), "run_draft_group.invalid_group");
        g = group();
        for (int i = 0; i < 16; ++i) g.sourceRunIds.push_back("run-" + std::to_string(i));
        accepted("16 source runs", g, base());
        g.sourceRunIds.back() = "";
        refused("an empty source run id", g, base(), "run_draft_group.invalid_group");
    }

    // Draft fields.
    {
        auto drafts = base(); drafts[1].id.clear();
        refused("an empty draft id", group(), drafts, "run_draft_group.invalid_drafts");
        drafts = base(); drafts[0].interfaceContracts = {contract("1bad", "x")}; seal(drafts);
        refused("a contract with an invalid id", group(), drafts, "run_draft_group.invalid_drafts");
        drafts = base(); drafts[0].interfaceContracts = {contract("C1", "x"), contract("C2", "y")}; seal(drafts);
        accepted("canonical contracts", group(), drafts);
        drafts = base(); drafts[1].title = "Another title"; drafts[1].task = "Another task"; seal(drafts);
        accepted("a title and a task sealed after a change", group(), drafts);
        drafts = base(1);
        accepted("a single draft", group(), drafts);
    }

    // Plan keys: [A-Za-z0-9][A-Za-z0-9_.-]{0,63}, unique ignoring case.
    for (const char* ok : {"a", "A", "0", "9x", "a.b-c_d", "MODEL", "k", "Zz9", "z", "Z", "9"}) {
        auto drafts = base(); drafts[0].provenance->planKey = ok;
        accepted(std::string("the plan key '") + ok + "'", group(), drafts);
    }
    {
        auto drafts = base(); drafts[0].provenance->planKey = std::string(64, 'k');
        accepted("a plan key of 64 characters", group(), drafts);
        drafts[0].provenance->planKey = std::string(65, 'k');
        refused("a plan key of 65 characters", group(), drafts, "run_draft_group.invalid_provenance");
    }
    for (const char* bad : {"", " ", "a b", "-a", ".a", "_a", "a/b", "a:b"}) {
        auto drafts = base(); drafts[0].provenance->planKey = bad;
        refused(std::string("the plan key '") + bad + "'", group(), drafts, "run_draft_group.invalid_provenance");
    }
    {
        auto drafts = base();
        drafts[0].provenance->createdContentFingerprint = std::string(63, 'a') + "F";
        refused("an uppercase content fingerprint", group(), drafts, "run_draft_group.invalid_provenance");
        drafts[0].provenance->createdContentFingerprint = std::string(65, 'a');
        refused("a 65-character content fingerprint", group(), drafts, "run_draft_group.invalid_provenance");
    }

    // Dependencies: any acyclic graph over the keys of the group is legal, forward references included.
    {
        auto diamond = base(4);
        diamond[1].provenance->dependsOn = {"k0"};
        diamond[2].provenance->dependsOn = {"k0"};
        diamond[3].provenance->dependsOn = {"k1", "k2"};
        seal(diamond);
        accepted("a diamond", group(), diamond);
        auto forward = base(3);
        forward[0].provenance->dependsOn = {"k2", "k1"};
        forward[1].provenance->dependsOn = {"k2"};
        seal(forward);
        accepted("forward references", group(), forward);
        auto chainBack = base(4);
        chainBack[0].provenance->dependsOn = {"k1"};
        chainBack[1].provenance->dependsOn = {"k2"};
        chainBack[2].provenance->dependsOn = {"k3"};
        seal(chainBack);
        accepted("a chain written backwards", group(), chainBack);
        chainBack[3].provenance->dependsOn = {"k0"};
        seal(chainBack);
        refused("the same chain closed into a cycle", group(), chainBack, "run_draft_group.invalid_dependencies");
        auto lateCycle = base(5);
        lateCycle[1].provenance->dependsOn = {"k0"};
        lateCycle[2].provenance->dependsOn = {"k3"};
        lateCycle[3].provenance->dependsOn = {"k4"};
        lateCycle[4].provenance->dependsOn = {"k2"};
        seal(lateCycle);
        refused("a cycle that does not touch the first draft", group(), lateCycle, "run_draft_group.invalid_dependencies");
        auto twoCycles = base(4);
        twoCycles[0].provenance->dependsOn = {"k1"};
        twoCycles[1].provenance->dependsOn = {"k0"};
        seal(twoCycles);
        refused("a two-draft cycle next to free drafts", group(), twoCycles, "run_draft_group.invalid_dependencies");
    }
}

// listAll is the "recent drafts" list: newest update first, ties by id; grouped drafts take part like any other.
inline void listAllIsOrderedByUpdateTimeThenId(const EnvFactory& make)
{
    auto env = make();
    auto& repo = *env.repository;
    const auto manual = [&](const std::string& id, std::int64_t updated) {
        RunDraft draft = manualDraft(env, id);
        draft.updatedAt = at(updated);
        return draft;
    };
    check(repo.insert(manual("m-c", 200)).success() && repo.insert(manual("m-a", 300)).success()
              && repo.insert(manual("m-d", 100)).success() && repo.insert(manual("m-b", 200)).success(),
        "setup: four manual drafts");
    RunDraft grouped = groupDraft(env, "g", "g-1", 0, "k0");
    grouped.updatedAt = at(250);
    check(repo.insertGroup(makeGroup(env, "g", 'a'), {grouped}).success(), "setup: a grouped draft");
    const auto all = repo.listAll();
    check(all.success(), "listAll works");
    std::vector<std::string> ids;
    for (const auto& draft : all.value()) ids.push_back(draft.id);
    check(ids == (std::vector<std::string> {"m-a", "g-1", "m-b", "m-c", "m-d"}),
        "listAll is ordered by update time, newest first, ties by id");
}

// Runs every scenario against a fresh repository from the factory.
inline void runRunDraftRepositoryContract(const EnvFactory& make)
{
    manualDraftsAreNotGrouped(make);
    listAllIsOrderedByUpdateTimeThenId(make);
    insertGroupRoundTrips(make);
    groupValidationBoundaries(make);
    refusedInsertionsLeaveNothing(make);
    provenanceIsImmutable(make);
    groupedDraftsEnterOnlyThroughInsertGroup(make);
    removingTheLastDraftRemovesTheGroup(make);
    groupsAreIsolatedFromEachOther(make);
}

} // namespace rundraft_contract
