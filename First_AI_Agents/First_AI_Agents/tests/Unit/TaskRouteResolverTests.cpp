// Unit tests for AG-17.2's TaskRouteResolver: the one place that turns a frozen
// Manifest task into the route its worker runs on (provider, model, output ceiling,
// turn timeout, pricing, member instructions), verified against the frozen routing
// record. Pure value tests plus the Backend builder over in-memory repositories: no
// provider, no SQLite.
#include "ai/application/agents/AgentToolTurnLoopRunner.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/application/execution/TaskRouteResolver.hpp"

#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

StageModelAssignment assignment(const std::string& stageType, const std::string& providerId,
    const std::string& modelId, const std::string& role, bool lead, bool workspace,
    const std::string& instructions, std::uint32_t position, std::uint64_t timeoutMs = 60000)
{
    StageModelAssignment value;
    value.stageType = stageType;
    value.providerId = providerId;
    value.modelId = modelId;
    value.role = role;
    value.leadArchitect = lead;
    value.workspaceAccess = workspace;
    value.instructions = instructions;
    value.position = position;
    value.timeout = std::chrono::milliseconds(timeoutMs);
    return value;
}

Provider providerRow(const std::string& id, const std::string& kind)
{
    Provider value;
    value.id = id;
    value.name = id;
    value.kind = kind;
    return value;
}

Model modelRow(const std::string& id, const std::string& providerId, const std::string& apiModelId,
    std::uint64_t maxOutputTokens, std::uint64_t inputPrice = 0)
{
    Model value;
    value.id = id;
    value.providerId = providerId;
    value.apiModelId = apiModelId;
    value.maxOutputTokens = maxOutputTokens;
    value.inputPriceMicrosPerMillionTokens = inputPrice;
    return value;
}

std::vector<StageModelAssignment> rosterAssignments()
{
    return {
        assignment("architect", "p-openai", "m-gpt-a", "lead", true, false, "Plan carefully.", 0),
        assignment("qml-worker", "p-openai", "m-gpt-b", "qml", false, true, "Write QML.", 1, 30000),
        assignment("cpp-worker", "p-anthropic", "m-claude", "cpp", false, true, "Write C++.", 2, 400000),
        assignment("doc-writer", "p-openai", "m-gpt-b", "docs", false, false, "", 3)};
}

// The Backend-resolved input of one assignment, exactly as buildTaskRouteResolver
// would assemble it.
TaskRouteInput inputFor(const StageModelAssignment& source, const std::string& kind,
    std::uint64_t maxOutputTokens = 0, std::uint64_t price = 0)
{
    TaskRouteInput input;
    input.memberId = source.stageType;
    input.provider = providerRow(source.providerId, kind);
    input.model = modelRow(source.modelId, source.providerId, "api-" + source.modelId,
        maxOutputTokens, price);
    input.role = ManifestRouting::effectiveRole(source);
    input.instructions = source.instructions;
    input.leadArchitect = source.leadArchitect;
    input.workspaceAccess = source.workspaceAccess;
    input.position = source.position;
    input.timeoutMs = ManifestRouting::effectiveTimeoutMs(source);
    return input;
}

std::vector<TaskRouteInput> rosterInputs()
{
    const auto members = rosterAssignments();
    return {inputFor(members[0], "openai", 4096, 100), inputFor(members[1], "openai", 32000, 200),
        inputFor(members[2], "anthropic", 0, 300), inputFor(members[3], "openai", 4096, 200)};
}

ExecutionManifestTask task(const std::string& id, WorkspaceCapability capability,
    std::optional<std::string> assignee = std::nullopt)
{
    ExecutionManifestTask value;
    value.id = id;
    value.requestedCapability = capability;
    value.assigneeMemberId = std::move(assignee);
    return value;
}

TaskRouteResolver singleResolver(std::uint64_t modelMaxOutput = 0, std::uint64_t runCeiling = 64000,
    std::uint64_t timeoutMs = 5000)
{
    const auto member = assignment("architect", "p-openai", "m-gpt-a", "", false, false,
        "Plan.", 0, timeoutMs);
    const auto record = ManifestRouting::fromAssignments({member});
    auto created = TaskRouteResolver::create(record, {inputFor(member, "openai", modelMaxOutput, 100)},
        runCeiling);
    require(created.success(), "test fixture: single resolver could not be created");
    return std::move(created.value());
}

TaskRouteResolver rosterResolver(std::uint64_t runCeiling = 64000)
{
    const auto record = ManifestRouting::fromAssignments(rosterAssignments());
    auto created = TaskRouteResolver::create(record, rosterInputs(), runCeiling);
    require(created.success(), "test fixture: roster resolver could not be created");
    return std::move(created.value());
}

void requireCode(const ai::common::Result<TaskRouteResolver>& result, const char* code, const char* message)
{
    require(!result.success() && result.error()->code == code, message);
}

// ---- resolution ---------------------------------------------------------------

void testSingleAssignmentRouteIsTheOneDefaultRoute()
{
    const auto resolver = singleResolver(/*modelMaxOutput=*/0, 64000, /*timeoutMs=*/5000);
    for (const auto& candidate : {task("a", WorkspaceCapability::Propose),
             task("b", WorkspaceCapability::Inspect), task("c", WorkspaceCapability::Propose, "architect")}) {
        const auto route = resolver.resolve(candidate);
        require(route.success(), "A single-assignment task must resolve to the sole member");
        require(route.value().memberId == "architect" && route.value().providerId == "p-openai"
                && route.value().modelId == "m-gpt-a" && route.value().apiModelId == "api-m-gpt-a"
                && route.value().providerKind == "openai",
            "The single route must carry the sole member's provider, model and kind");
        require(route.value().pricingModel.has_value()
                && route.value().pricingModel->id == "m-gpt-a"
                && route.value().pricingModel->inputPriceMicrosPerMillionTokens == 100,
            "The route must carry its own model record for pricing");
        require(route.value().turnTimeout == kWorkerProposeTurnTimeout,
            "A single-assignment worker turn timeout must stay exactly the Backend constant");
        require(route.value().workerInstructions.empty(),
            "A single-assignment worker must not receive the Architect's instructions (unchanged)");
    }
}

void testRosterTasksResolveToTheirOwnMemberRoute()
{
    const auto resolver = rosterResolver();
    const auto qml = resolver.resolve(task("t1", WorkspaceCapability::Propose, "qml-worker"));
    const auto cpp = resolver.resolve(task("t2", WorkspaceCapability::Inspect, "cpp-worker"));
    require(qml.success() && cpp.success(), "Roster tasks assigned to workers must resolve");
    require(qml.value().providerId == "p-openai" && qml.value().modelId == "m-gpt-b"
            && qml.value().providerKind == "openai" && qml.value().memberId == "qml-worker",
        "A task must resolve to its own member's provider and model");
    require(cpp.value().providerId == "p-anthropic" && cpp.value().modelId == "m-claude"
            && cpp.value().providerKind == "anthropic" && cpp.value().apiModelId == "api-m-claude",
        "A second member on another provider must get ITS provider, model and kind");
    require(qml.value().pricingModel->inputPriceMicrosPerMillionTokens == 200
            && cpp.value().pricingModel->inputPriceMicrosPerMillionTokens == 300,
        "Each route must be priced by its own model record");
    require(qml.value().workerInstructions == "Write QML."
            && cpp.value().workerInstructions == "Write C++.",
        "A roster worker must receive exactly its own member's instructions");
    require(qml.value().agentRole == "qml" && cpp.value().agentRole == "cpp"
            && qml.value().modelExecutionPosition == std::optional<std::uint32_t>(1)
            && cpp.value().modelExecutionPosition == std::optional<std::uint32_t>(2),
        "A roster route must carry its member's frozen role and position");
    // Several tasks may share one member.
    const auto again = resolver.resolve(task("t3", WorkspaceCapability::Propose, "qml-worker"));
    require(again.success() && again.value().memberId == "qml-worker",
        "Several tasks for one member must resolve to the same route");
}

void testModelExecutionPositionsAreUniqueAndDispatchOrderIndependent()
{
    ExecutionManifest rosterManifest;
    rosterManifest.tasks = {
        task("a", WorkspaceCapability::Propose, "qml-worker"),
        task("b", WorkspaceCapability::Propose, "cpp-worker"),
        task("c", WorkspaceCapability::Inspect, "qml-worker")};
    const auto rosterPlan = rosterResolver().allocateModelExecutionPositions(
        rosterManifest, 99);
    require(rosterPlan.success()
            && rosterPlan.value().byTaskId.at("a") == 1
            && rosterPlan.value().byTaskId.at("b") == 2
            && rosterPlan.value().byTaskId.at("c") == 4
            && rosterPlan.value().lastPosition == 4,
        "Roster positions must keep first-member slots and place repeated tasks after every frozen member");

    ExecutionManifest singleManifest;
    singleManifest.tasks = {
        task("coord", WorkspaceCapability::None),
        task("worker", WorkspaceCapability::Propose)};
    const auto singlePlan = singleResolver().allocateModelExecutionPositions(
        singleManifest, 10);
    require(singlePlan.success()
            && !singlePlan.value().byTaskId.contains("coord")
            && singlePlan.value().byTaskId.at("worker") == 11
            && singlePlan.value().lastPosition == 11,
        "Single-assignment positions must retain the legacy Manifest-ordinal allocation");

    // A Manifest frozen before AG-17.1 has no routing record and therefore no
    // resolver: the allocation must be the same ordinal one, never a null dereference.
    const auto legacyPlan = allocateWorkerModelExecutionPositions(nullptr, singleManifest, 10);
    require(legacyPlan.success()
            && !legacyPlan.value().byTaskId.contains("coord")
            && legacyPlan.value().byTaskId.at("worker") == 11
            && legacyPlan.value().lastPosition == 11,
        "Without a resolver the allocation must be the legacy Manifest-ordinal one");
    const auto resolver = singleResolver();
    const auto viaResolver = allocateWorkerModelExecutionPositions(&resolver, singleManifest, 10);
    require(viaResolver.success() && viaResolver.value().byTaskId == singlePlan.value().byTaskId,
        "The free allocator must delegate to the resolver when there is one");

    // Several repeats of one member take consecutive slots after every frozen member,
    // in Manifest order, whatever order the tasks are later dispatched in.
    ExecutionManifest repeatedManifest;
    repeatedManifest.tasks = {
        task("a", WorkspaceCapability::Propose, "qml-worker"),
        task("c", WorkspaceCapability::Propose, "qml-worker"),
        task("b", WorkspaceCapability::Propose, "cpp-worker"),
        task("d", WorkspaceCapability::Inspect, "qml-worker"),
        task("e", WorkspaceCapability::Propose, "cpp-worker")};
    const auto repeatedPlan = rosterResolver().allocateModelExecutionPositions(repeatedManifest, 0);
    require(repeatedPlan.success()
            && repeatedPlan.value().byTaskId.at("a") == 1
            && repeatedPlan.value().byTaskId.at("b") == 2
            && repeatedPlan.value().byTaskId.at("c") == 4
            && repeatedPlan.value().byTaskId.at("d") == 5
            && repeatedPlan.value().byTaskId.at("e") == 6
            && repeatedPlan.value().lastPosition == 6,
        "Repeated tasks of one member must take consecutive slots after all frozen members");
    std::set<std::uint32_t> distinct;
    for (const auto& [id, position] : repeatedPlan.value().byTaskId) distinct.insert(position);
    require(distinct.size() == repeatedPlan.value().byTaskId.size(),
        "Every task must get its own durable position");

    // An unresolvable task fails the whole plan (typed), never gets a position.
    ExecutionManifest brokenManifest;
    brokenManifest.tasks = {task("a", WorkspaceCapability::Propose, "qml-worker"),
        task("z", WorkspaceCapability::Propose, "architect")};
    const auto brokenPlan = rosterResolver().allocateModelExecutionPositions(brokenManifest, 0);
    require(!brokenPlan.success() && brokenPlan.error()->code == kRouteUnresolvedCode,
        "A plan over an unresolvable task must fail with the typed code");
}

void testRecordOnlyAllocationIsExactlyTheResolversAllocation()
{
    // AG-17.7B: a read-side projection inverts the writer's allocation from the routing
    // record alone, so that function must agree with the resolver's own, success and failure.
    ExecutionManifest rosterManifest;
    rosterManifest.tasks = {
        task("a", WorkspaceCapability::Propose, "qml-worker"),
        task("coord", WorkspaceCapability::None),
        task("c", WorkspaceCapability::Propose, "qml-worker"),
        task("b", WorkspaceCapability::Inspect, "cpp-worker"),
        task("d", WorkspaceCapability::Propose, "qml-worker")};
    ExecutionManifest singleManifest;
    singleManifest.tasks = {
        task("coord", WorkspaceCapability::None), task("worker", WorkspaceCapability::Propose),
        task("second", WorkspaceCapability::Inspect, "architect")};
    ExecutionManifest brokenRoster;
    brokenRoster.tasks = {task("a", WorkspaceCapability::Propose, "qml-worker"),
        task("z", WorkspaceCapability::Propose, "architect")};
    ExecutionManifest brokenSingle;
    brokenSingle.tasks = {task("a", WorkspaceCapability::Propose, "ghost")};
    ExecutionManifest noWorkspaceWorker;
    noWorkspaceWorker.tasks = {task("a", WorkspaceCapability::Propose, "doc-writer")};

    const auto roster = rosterResolver();
    const auto single = singleResolver();
    const std::vector<std::pair<const TaskRouteResolver*, const ExecutionManifest*>> cases {
        {&roster, &rosterManifest}, {&single, &singleManifest}, {&roster, &brokenRoster},
        {&single, &brokenSingle}, {&roster, &noWorkspaceWorker}};
    for (const auto& [resolver, manifest] : cases) {
        const auto viaResolver = resolver->allocateModelExecutionPositions(*manifest, 7);
        const auto viaRecord =
            allocateModelExecutionPositionsFromRecord(resolver->routing(), *manifest, 7);
        require(viaResolver.success() == viaRecord.success(),
            "The record-only allocation and the resolver's disagree on success");
        if (viaResolver.success()) {
            require(viaResolver.value().byTaskId == viaRecord.value().byTaskId
                    && viaResolver.value().lastPosition == viaRecord.value().lastPosition,
                "The record-only allocation must produce exactly the resolver's positions");
        } else {
            require(viaResolver.error()->code == viaRecord.error()->code
                    && viaResolver.error()->message == viaRecord.error()->message,
                "The record-only allocation must fail exactly like the resolver's");
        }
    }
    const auto rosterPlan = allocateModelExecutionPositionsFromRecord(roster.routing(), rosterManifest, 7);
    require(rosterPlan.success() && rosterPlan.value().byTaskId.at("a") == 1
            && rosterPlan.value().byTaskId.at("b") == 2 && rosterPlan.value().byTaskId.at("c") == 4
            && rosterPlan.value().byTaskId.at("d") == 5 && !rosterPlan.value().byTaskId.contains("coord"),
        "The record-only allocation keeps first-member slots and places repeats after all members");
}

void testUnresolvableTasksFailWithTheTypedCode()
{
    const auto expectUnresolved = [](const TaskRouteResolver& resolver,
                                     const ExecutionManifestTask& candidate, const char* message) {
        const auto route = resolver.resolve(candidate);
        require(!route.success() && route.error()->code == kRouteUnresolvedCode, message);
    };
    const auto single = singleResolver();
    expectUnresolved(single, task("a", WorkspaceCapability::Propose, "ghost"),
        "An unknown assignee in a single-assignment Manifest resolved");
    expectUnresolved(single, task("a", WorkspaceCapability::None),
        "A coordination task resolved to a route");
    const auto roster = rosterResolver();
    expectUnresolved(roster, task("a", WorkspaceCapability::Propose),
        "A roster task without an assignee resolved");
    expectUnresolved(roster, task("a", WorkspaceCapability::Propose, "ghost"),
        "A roster task with an unknown assignee resolved");
    expectUnresolved(roster, task("a", WorkspaceCapability::Propose, "architect"),
        "A roster task assigned to the lead Architect resolved");
    expectUnresolved(roster, task("a", WorkspaceCapability::Inspect, "doc-writer"),
        "A roster task assigned to a member without workspace access resolved");
    expectUnresolved(roster, task("a", WorkspaceCapability::None, "qml-worker"),
        "A roster coordination task resolved to a route");
}

void testLeadWithWorkspaceAccessIsStillNeverAnAssignee()
{
    // A record may legitimately carry a lead with workspace access (the two flags are
    // independent assignment columns). The lead only plans in a roster: the workspace
    // check must not be the only thing keeping it away from worker tasks.
    auto members = rosterAssignments();
    members[0].workspaceAccess = true;
    const auto record = ManifestRouting::fromAssignments(members);
    require(!ManifestRouting::recordProblem(record).has_value(),
        "A lead with workspace access is a valid routing record");
    auto inputs = rosterInputs();
    inputs[0].workspaceAccess = true;
    auto created = TaskRouteResolver::create(record, std::move(inputs), 64000);
    require(created.success(), "A roster whose lead has workspace access must still resolve");
    const auto route = created.value().resolve(task("a", WorkspaceCapability::Propose, "architect"));
    require(!route.success() && route.error()->code == kRouteUnresolvedCode,
        "A lead with workspace access must still never be a task's assignee");
    require(created.value().resolve(task("b", WorkspaceCapability::Propose, "qml-worker")).success(),
        "Workers of that roster must still resolve");
}

void testOutputCeilingFollowsTheWorkerFloorAndTheRunCap()
{
    require(singleResolver(0).resolve(task("a", WorkspaceCapability::Propose)).value().maxOutputTokens
                == kWorkerProposeMinOutputTokens,
        "A model without a ceiling must get the worker floor");
    require(singleResolver(4096).resolve(task("a", WorkspaceCapability::Propose)).value().maxOutputTokens
                == kWorkerProposeMinOutputTokens,
        "A 4096-token model record must be raised to the worker floor (the incident fix)");
    require(singleResolver(32000).resolve(task("a", WorkspaceCapability::Propose)).value().maxOutputTokens
                == 32000,
        "A larger model ceiling must be kept");
    require(singleResolver(32000, /*runCeiling=*/20000)
                    .resolve(task("a", WorkspaceCapability::Propose)).value().maxOutputTokens == 20000,
        "The Run provider budget must cap the route's ceiling");
    require(singleResolver(0, /*runCeiling=*/8000)
                    .resolve(task("a", WorkspaceCapability::Propose)).value().maxOutputTokens == 8000,
        "The Run cap applies even below the worker floor, exactly as the primary workflow did");
}

void testTurnTimeoutRules()
{
    // single_assignment: the Backend constant, whatever the member configured.
    require(singleResolver(0, 64000, 5000).resolve(task("a", WorkspaceCapability::Propose))
                    .value().turnTimeout == kWorkerProposeTurnTimeout
            && singleResolver(0, 64000, 600000).resolve(task("a", WorkspaceCapability::Propose))
                    .value().turnTimeout == kWorkerProposeTurnTimeout,
        "A single-assignment turn timeout must not depend on the member's configured timeout");
    // roster: max(member timeout, floor).
    const auto roster = rosterResolver();
    require(roster.resolve(task("a", WorkspaceCapability::Propose, "qml-worker")).value().turnTimeout
                == kWorkerProposeTurnTimeout,
        "A short member timeout must never lower the Backend's incident-driven floor");
    require(roster.resolve(task("b", WorkspaceCapability::Propose, "cpp-worker")).value().turnTimeout
                == std::chrono::milliseconds(400000),
        "A longer member timeout may raise the floor");
}

// ---- identity verification against the frozen record ---------------------------

void testCreateRejectsEveryIdentityDrift()
{
    const auto record = ManifestRouting::fromAssignments(rosterAssignments());
    const auto expectDrift = [&](const std::function<void(std::vector<TaskRouteInput>&)>& mutate,
                                 const char* code, const char* message) {
        auto inputs = rosterInputs();
        mutate(inputs);
        requireCode(TaskRouteResolver::create(record, std::move(inputs), 64000), code, message);
    };
    expectDrift([](auto& in) { in[1].provider.id = "p-other"; }, kRouteIdentityMismatchCode,
        "A drifted provider id was accepted");
    expectDrift([](auto& in) { in[1].model.id = "m-other"; }, kRouteIdentityMismatchCode,
        "A drifted model id was accepted");
    expectDrift([](auto& in) { in[1].model.providerId = "p-anthropic"; }, kRouteIdentityMismatchCode,
        "A model owned by another provider was accepted");
    expectDrift([](auto& in) { in[1].role = "changed"; }, kRouteIdentityMismatchCode,
        "A drifted role was accepted");
    expectDrift([](auto& in) { in[1].instructions = "Write QML!"; }, kRouteIdentityMismatchCode,
        "Edited instructions were accepted against the frozen hash");
    expectDrift([](auto& in) { in[1].leadArchitect = true; }, kRouteIdentityMismatchCode,
        "A drifted lead flag was accepted");
    expectDrift([](auto& in) { in[1].workspaceAccess = false; }, kRouteIdentityMismatchCode,
        "A drifted workspace capability was accepted");
    expectDrift([](auto& in) { in[1].position = 9; }, kRouteIdentityMismatchCode,
        "A drifted member position was accepted");
    expectDrift([](auto& in) { in[1].timeoutMs += 1; }, kRouteIdentityMismatchCode,
        "A drifted member timeout was accepted");
    expectDrift([](auto& in) { in[2].provider.kind.clear(); }, kRouteResolutionFailedCode,
        "A provider without a kind was accepted");
    expectDrift([](auto& in) { in.pop_back(); }, kRouteResolutionFailedCode,
        "A missing member input was accepted");
    expectDrift([](auto& in) { in.push_back(in.front()); }, kRouteResolutionFailedCode,
        "An extra member input was accepted");
    expectDrift([](auto& in) { in[3].memberId = in[2].memberId; }, kRouteResolutionFailedCode,
        "Two inputs for one member were accepted");

    auto defective = record;
    defective.fingerprint = std::string(64, '0');
    requireCode(TaskRouteResolver::create(defective, rosterInputs(), 64000), kRouteResolutionFailedCode,
        "A defective routing record was accepted");
}

// ---- the Backend builder --------------------------------------------------------

void testBuilderResolvesFromTheSnapshotAndRepositories()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    require(providers.insert(providerRow("p-openai", "openai")).success()
            && providers.insert(providerRow("p-anthropic", "anthropic")).success(),
        "test fixture: providers");
    require(models.insert(modelRow("m-gpt-a", "p-openai", "gpt-a", 4096)).success()
            && models.insert(modelRow("m-gpt-b", "p-openai", "gpt-b", 32000, 200)).success()
            && models.insert(modelRow("m-claude", "p-anthropic", "claude", 0, 300)).success(),
        "test fixture: models");
    const auto assignments = rosterAssignments();
    const auto record = ManifestRouting::fromAssignments(assignments);

    auto built = buildTaskRouteResolver(record, assignments, providers, models, 64000);
    require(built.success(), "The builder must resolve a consistent snapshot");
    const auto cpp = built.value().resolve(task("t", WorkspaceCapability::Propose, "cpp-worker"));
    require(cpp.success() && cpp.value().providerKind == "anthropic"
            && cpp.value().apiModelId == "claude" && cpp.value().modelId == "m-claude",
        "The builder must read the provider kind and API model id from the repositories");

    // A frozen instruction edit (the profile/snapshot no longer matches the record).
    auto drifted = assignments;
    drifted[1].instructions = "Write QML differently.";
    const auto drift = buildTaskRouteResolver(record, drifted, providers, models, 64000);
    require(!drift.success() && drift.error()->code == kRouteIdentityMismatchCode,
        "An instruction edit after freeze must be a route identity mismatch");

    // A member that is not in the snapshot at all.
    auto missingMember = assignments;
    missingMember.erase(missingMember.begin() + 2);
    const auto missing = buildTaskRouteResolver(record, missingMember, providers, models, 64000);
    require(!missing.success() && missing.error()->code == kRouteResolutionFailedCode,
        "A routing member absent from the snapshot must fail resolution");

    // A model that disappeared.
    InMemoryModelRepository fewerModels;
    require(fewerModels.insert(modelRow("m-gpt-a", "p-openai", "gpt-a", 4096)).success()
            && fewerModels.insert(modelRow("m-gpt-b", "p-openai", "gpt-b", 32000)).success(),
        "test fixture: models");
    const auto gone = buildTaskRouteResolver(record, assignments, providers, fewerModels, 64000);
    require(!gone.success() && gone.error()->code == kRouteResolutionFailedCode,
        "A vanished model must fail resolution");

    // A real-profile style assignment (empty role) builds and resolves: the role the
    // record froze (the stage id) is the role the builder re-checks.
    StageModelAssignment real;
    real.stageType = "architect";
    real.providerId = "p-openai";
    real.modelId = "m-gpt-a";
    real.timeout = std::chrono::milliseconds(600000);
    const auto realRecord = ManifestRouting::fromAssignments({real});
    require(buildTaskRouteResolver(realRecord, {real}, providers, models, 64000).success(),
        "A real profile assignment with an empty role must build a resolver");
}

void testResolverIsImmutableAfterConstruction()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    require(providers.insert(providerRow("p-openai", "openai")).success(), "test fixture: provider");
    require(models.insert(modelRow("m-gpt-a", "p-openai", "gpt-a", 0, 100)).success(), "test fixture: model");
    StageModelAssignment architect = assignment("architect", "p-openai", "m-gpt-a", "lead", true,
        false, "Plan.", 0);
    auto built = buildTaskRouteResolver(ManifestRouting::fromAssignments({architect}), {architect},
        providers, models, 64000);
    require(built.success(), "test fixture: resolver");
    const auto before = built.value().resolve(task("a", WorkspaceCapability::Propose));
    // Repositories change after construction; the frozen route does not.
    auto changed = modelRow("m-gpt-a", "p-openai", "gpt-renamed", 99999, 5);
    require(models.update(changed).success(), "test fixture: model update");
    const auto after = built.value().resolve(task("a", WorkspaceCapability::Propose));
    require(before.success() && after.success()
            && before.value().apiModelId == after.value().apiModelId
            && before.value().apiModelId == "gpt-a"
            && before.value().maxOutputTokens == after.value().maxOutputTokens,
        "A resolver must keep the route it was built with, whatever the repositories later say");
}

// ---- AG-17.5: the Run-wide routing context ---------------------------------------

void seedRosterRepositories(InMemoryProviderRepository& providers, InMemoryModelRepository& models)
{
    Provider openai = providerRow("p-openai", "openai");
    openai.name = "OpenAI";
    Provider anthropic = providerRow("p-anthropic", "anthropic");
    anthropic.name = ""; // an unnamed provider: the label falls back to the model name alone
    require(providers.insert(openai).success() && providers.insert(anthropic).success(),
        "test fixture: providers");
    Model gptA = modelRow("m-gpt-a", "p-openai", "gpt-a", 4096);
    gptA.displayName = "GPT A";
    Model gptB = modelRow("m-gpt-b", "p-openai", "gpt-b", 32000, 200);
    gptB.displayName = ""; // an unnamed model: the label falls back to its API id
    Model claude = modelRow("m-claude", "p-anthropic", "claude", 0, 300);
    claude.displayName = "Claude";
    require(models.insert(gptA).success() && models.insert(gptB).success()
            && models.insert(claude).success(),
        "test fixture: models");
}

void testRunRoutingContextSelectsTheLeadAndBuildsTheRosterCatalog()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    seedRosterRepositories(providers, models);
    const auto assignments = rosterAssignments();
    const auto record = ManifestRouting::fromAssignments(assignments);

    auto built = buildManifestRunRoutingContext(record, assignments, providers, models, 64000);
    require(built.success(), "A consistent roster snapshot must resolve into a routing context");
    const auto& context = built.value();
    require(context.leadAssignment.stageType == "architect"
            && context.leadProvider.id == "p-openai" && context.leadModel.id == "m-gpt-a"
            && context.routing.fingerprint == record.fingerprint,
        "The context must carry the frozen record and the one lead Architect's identity");
    require(context.taskRoutes.resolve(task("t", WorkspaceCapability::Propose, "cpp-worker"))
                .success(),
        "The context must carry a resolver for the worker tasks");

    require(context.rosterCatalog.size() == 4
            && ManifestRouting::rosterCatalogProblem(record, context.rosterCatalog)
                   == std::nullopt,
        "The catalog must describe exactly the record's members and pass the Planner's check");
    const auto labelOf = [&](const std::string& memberId) {
        for (const auto& entry : context.rosterCatalog) {
            if (entry.memberId == memberId) return entry.modelLabel;
        }
        return std::string("<missing>");
    };
    require(labelOf("architect") == "OpenAI / GPT A",
        "A named provider and a named model make 'provider / model'");
    require(labelOf("qml-worker") == "OpenAI / gpt-b",
        "A model without a display name must fall back to its API id");
    require(labelOf("cpp-worker") == "Claude",
        "A provider without a name must leave just the model name");
    for (const auto& entry : context.rosterCatalog) {
        std::string expected = "<unknown member>";
        for (const auto& item : assignments) {
            if (item.stageType == entry.memberId) expected = item.instructions;
        }
        require(entry.instructions == expected,
            "The catalog must carry each member's exact frozen instructions");
    }
}

void testRunRoutingContextOfASingleAssignmentHasNoCatalog()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    seedRosterRepositories(providers, models);
    const StageModelAssignment architect =
        assignment("architect", "p-openai", "m-gpt-a", "lead", false, false, "Plan.", 0);
    auto built = buildManifestRunRoutingContext(
        ManifestRouting::fromAssignments({architect}), {architect}, providers, models, 64000);
    require(built.success() && built.value().rosterCatalog.empty()
            && built.value().leadAssignment.stageType == "architect"
            && built.value().routing.mode == ManifestRoutingMode::SingleAssignment,
        "A single-assignment Run keeps its one implicit Architect and shows no roster");
}

void testRunRoutingContextFailsClosedBeforeAnyPlanningActivity()
{
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    seedRosterRepositories(providers, models);
    const auto assignments = rosterAssignments();
    const auto record = ManifestRouting::fromAssignments(assignments);

    // The lead's own model vanished: the Planner could not even start.
    InMemoryModelRepository withoutLeadModel;
    Model gptB = modelRow("m-gpt-b", "p-openai", "gpt-b", 32000);
    Model claude = modelRow("m-claude", "p-anthropic", "claude", 0);
    require(withoutLeadModel.insert(gptB).success() && withoutLeadModel.insert(claude).success(),
        "test fixture: models");
    const auto noLead = buildManifestRunRoutingContext(
        record, assignments, providers, withoutLeadModel, 64000);
    require(!noLead.success(), "A vanished lead model must fail the context");

    // A worker's provider vanished.
    InMemoryProviderRepository withoutAnthropic;
    require(withoutAnthropic.insert(providerRow("p-openai", "openai")).success(),
        "test fixture: provider");
    const auto noWorker = buildManifestRunRoutingContext(
        record, assignments, withoutAnthropic, models, 64000);
    require(!noWorker.success() && noWorker.error()->code == kRouteResolutionFailedCode,
        "A vanished worker provider must fail resolution");

    // Instructions edited after freeze: an identity mismatch, never a silent re-route.
    auto drifted = assignments;
    drifted[2].instructions = "Write C++ differently.";
    const auto drift = buildManifestRunRoutingContext(record, drifted, providers, models, 64000);
    require(!drift.success() && drift.error()->code == kRouteIdentityMismatchCode,
        "A drifted member must fail with the identity-mismatch code");

    // No worker could ever be assigned (a snapshot that bypassed profile validation).
    auto noWorkspace = assignments;
    noWorkspace[1].workspaceAccess = false;
    noWorkspace[2].workspaceAccess = false;
    const auto lonelyRecord = ManifestRouting::fromAssignments(noWorkspace);
    const auto nobody = buildManifestRunRoutingContext(
        lonelyRecord, noWorkspace, providers, models, 64000);
    require(!nobody.success() && nobody.error()->code == "planner.roster_no_assignable_member",
        "A roster with no assignable worker must fail before the Planner");
}

} // namespace

int runTaskRouteResolverTests()
{
    try {
        testSingleAssignmentRouteIsTheOneDefaultRoute();
        testRosterTasksResolveToTheirOwnMemberRoute();
        testModelExecutionPositionsAreUniqueAndDispatchOrderIndependent();
        testRecordOnlyAllocationIsExactlyTheResolversAllocation();
        testUnresolvableTasksFailWithTheTypedCode();
        testLeadWithWorkspaceAccessIsStillNeverAnAssignee();
        testOutputCeilingFollowsTheWorkerFloorAndTheRunCap();
        testTurnTimeoutRules();
        testCreateRejectsEveryIdentityDrift();
        testBuilderResolvesFromTheSnapshotAndRepositories();
        testResolverIsImmutableAfterConstruction();
        testRunRoutingContextSelectsTheLeadAndBuildsTheRosterCatalog();
        testRunRoutingContextOfASingleAssignmentHasNoCatalog();
        testRunRoutingContextFailsClosedBeforeAnyPlanningActivity();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TaskRouteResolver tests failed: " << exception.what() << '\n';
        return 1;
    }
}
