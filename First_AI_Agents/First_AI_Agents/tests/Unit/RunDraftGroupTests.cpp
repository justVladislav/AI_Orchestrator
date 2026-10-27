// PC-3B: the pure group rules and the in-memory IRunDraftRepository. The in-memory adapter runs the SAME contract
// suite as the SQLite adapter (RunDraftRepositoryContract.hpp), so the two cannot drift apart silently.

#include "RunDraftRepositoryContract.hpp"

#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ai::application;
namespace rc = rundraft_contract;

namespace {

rc::Env inMemoryEnv()
{
    auto repository = std::make_shared<InMemoryRunDraftRepository>();
    rc::Env env;
    env.keepAlive = repository;
    env.repository = repository.get();
    env.projectId = "project-1";
    env.otherProjectId = "project-2";
    env.providerId = "provider-1";
    env.modelA = "model-a";
    env.modelB = "model-b";
    return env;
}

void testTheInMemoryRepositoryHonoursTheContract()
{
    rc::runRunDraftRepositoryContract([] { return inMemoryEnv(); });
}

void testOriginNames()
{
    rc::check(std::string(toString(RunDraftGroupOrigin::ChatMessage)) == "chat_message"
            && std::string(toString(RunDraftGroupOrigin::ManualImport)) == "manual_import",
        "The stored origin names are stable");
    rc::check(runDraftGroupOriginFromString("chat_message") == RunDraftGroupOrigin::ChatMessage
            && runDraftGroupOriginFromString("manual_import") == RunDraftGroupOrigin::ManualImport
            && runDraftGroupOriginFromString("follow_up_request") == RunDraftGroupOrigin::FollowUpRequest
            && std::string(toString(RunDraftGroupOrigin::FollowUpRequest)) == "follow_up_request",
        "All three origins parse");
    for (const char* bad : {"", "Chat_Message", "chat", "manual", " manual_import", "chat_message ", "unknown"}) {
        rc::check(!runDraftGroupOriginFromString(bad).has_value(), std::string("Not an origin: '") + bad + "'");
    }
    // A number no enumerator names never turns into a real origin name.
    for (const int raw : {-1, 3, 7, 255}) {
        const std::string name = toString(static_cast<RunDraftGroupOrigin>(raw));
        rc::check(name == "unknown" && !runDraftGroupOriginFromString(name).has_value(),
            "An unknown origin value must not be named like a real origin: " + name);
    }
}

// The rule every repository's update() applies, on its own.
void testTheProvenanceRule()
{
    const auto env = inMemoryEnv();
    const RunDraft stored = rc::groupDraft(env, "g1", "d1", 1, "view", {"model"});
    const RunDraft manual = rc::manualDraft(env, "m1");

    const auto same = checkRunDraftProvenanceUnchanged(stored.provenance, stored.projectId, stored);
    rc::check(same.success(), "An unchanged draft passes");
    RunDraft edited = stored;
    edited.title = "other";
    edited.task = "other";
    rc::check(checkRunDraftProvenanceUnchanged(stored.provenance, stored.projectId, edited).success(),
        "Editable fields are not the rule's business");
    rc::check(checkRunDraftProvenanceUnchanged(manual.provenance, manual.projectId, manual).success(),
        "A manual draft passes");
    RunDraft moved = manual;
    moved.projectId = "somewhere-else";
    rc::check(checkRunDraftProvenanceUnchanged(manual.provenance, manual.projectId, moved).success(),
        "A manual draft may change project");

    const auto code = [&](RunDraft incoming, const std::optional<RunDraftProvenance>& storedProvenance, const std::string& project) {
        const auto result = checkRunDraftProvenanceUnchanged(storedProvenance, project, incoming);
        rc::check(!result.success(), "The change must be refused");
        return result.error()->code;
    };
    auto differing = [&](auto&& change) {
        RunDraft incoming = stored;
        change(*incoming.provenance);
        return incoming;
    };
    rc::check(code(differing([](RunDraftProvenance& p) { p.groupId = "g2"; }), stored.provenance, stored.projectId)
                  == "run_draft.provenance_immutable", "group");
    rc::check(code(differing([](RunDraftProvenance& p) { p.ordinal = 9; }), stored.provenance, stored.projectId)
                  == "run_draft.provenance_immutable", "ordinal");
    rc::check(code(differing([](RunDraftProvenance& p) { p.planKey = "x"; }), stored.provenance, stored.projectId)
                  == "run_draft.provenance_immutable", "key");
    rc::check(code(differing([](RunDraftProvenance& p) { p.dependsOn.push_back("x"); }), stored.provenance, stored.projectId)
                  == "run_draft.provenance_immutable", "dependencies");
    rc::check(code(differing([](RunDraftProvenance& p) { p.createdContentFingerprint = rc::hex64('0'); }), stored.provenance,
                  stored.projectId) == "run_draft.provenance_immutable", "fingerprint");
    RunDraft dropped = stored;
    dropped.provenance.reset();
    rc::check(code(dropped, stored.provenance, stored.projectId) == "run_draft.provenance_immutable", "dropped");
    RunDraft attached = manual;
    attached.provenance = stored.provenance;
    rc::check(code(attached, manual.provenance, manual.projectId) == "run_draft.provenance_immutable", "attached");
    RunDraft relocated = stored;
    relocated.projectId = "other-project";
    rc::check(code(relocated, stored.provenance, stored.projectId) == "run_draft.group_project_immutable", "project");
}

// The code says which rule, the message says what is wrong: a self reference is not reported as a cycle, an unknown
// key is not reported as a repeat. (The contract suite checks the codes; this checks the words the user reads.)
void testValidationMessagesNameTheProblem()
{
    const auto env = inMemoryEnv();
    const RunDraftGroup group = rc::makeGroup(env, "g", 'a');
    const auto drafts = [&] {
        std::vector<RunDraft> list;
        for (std::uint32_t i = 0; i < 3; ++i) list.push_back(rc::groupDraft(env, "g", "d" + std::to_string(i), i, "k" + std::to_string(i)));
        return list;
    };
    const auto messageOf = [&](const std::function<void(std::vector<RunDraft>&)>& change) {
        auto list = drafts();
        change(list);
        for (auto& draft : list) rc::reseal(draft);   // only the dependencies are wrong, not the seal
        const auto result = validateRunDraftGroupInsertion(group, list);
        rc::check(!result.success(), "The group must be refused");
        rc::check(result.error()->code == "run_draft_group.invalid_dependencies", "The code is the dependency one");
        return result.error()->message;
    };
    const auto has = [](const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; };

    const auto self = messageOf([](auto& d) { d[1].provenance->dependsOn = {"k1"}; });
    rc::check(has(self, "itself") && has(self, "k1") && !has(self, "cycle"), "A self reference says so and names the draft: " + self);
    const auto unknown = messageOf([](auto& d) { d[1].provenance->dependsOn = {"nope"}; });
    rc::check(has(unknown, "not in the group") && has(unknown, "k1"), "An unknown key says so: " + unknown);
    const auto twice = messageOf([](auto& d) { d[2].provenance->dependsOn = {"k0", "k0"}; });
    rc::check(has(twice, "twice") && has(twice, "k2"), "A repeated dependency says so: " + twice);
    const auto cycle = messageOf([](auto& d) {
        d[0].provenance->dependsOn = {"k1"};
        d[1].provenance->dependsOn = {"k0"};
    });
    rc::check(has(cycle, "cycle") && !has(cycle, "itself"), "A cycle says so: " + cycle);
}

// The words for the other refusals added with the review of 3B: the seal, the origin and the context fingerprint.
void testValidationMessagesForTheSealAndTheOrigin()
{
    const auto env = inMemoryEnv();
    const auto has = [](const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; };

    std::vector<RunDraft> drafts {rc::groupDraft(env, "g", "d0", 0, "k0"), rc::groupDraft(env, "g", "d1", 1, "k1")};
    drafts[1].task = "edited after sealing";
    const auto stale = validateRunDraftGroupInsertion(rc::makeGroup(env, "g", 'a'), drafts);
    rc::check(!stale.success() && stale.error()->code == "run_draft_group.invalid_provenance"
            && has(stale.error()->message, "k1") && has(stale.error()->message, "fingerprint"),
        "A stale seal is named with the draft: " + (stale.error() ? stale.error()->message : "accepted"));

    drafts = {rc::groupDraft(env, "g", "d0", 0, "k0")};
    RunDraftGroup manual = rc::makeGroup(env, "g", 'a');
    manual.contextFingerprint = rc::hex64('d');
    const auto withContext = validateRunDraftGroupInsertion(manual, drafts);
    rc::check(!withContext.success() && withContext.error()->code == "run_draft_group.invalid_group"
            && has(withContext.error()->message, "context fingerprint") && has(withContext.error()->message, "manual"),
        "A manual import with a context fingerprint says why: " + (withContext.error() ? withContext.error()->message : "accepted"));

    RunDraftGroup odd = rc::makeGroup(env, "g", 'a');
    odd.origin = static_cast<RunDraftGroupOrigin>(5);
    const auto unknown = validateRunDraftGroupInsertion(odd, drafts);
    rc::check(!unknown.success() && has(unknown.error()->message, "origin") && has(unknown.error()->message, "unknown"),
        "An unknown origin says so: " + (unknown.error() ? unknown.error()->message : "accepted"));

    rc::check(validateRunDraftGroupInsertion(rc::makeGroup(env, "g", 'a'), drafts).success(),
        "...while the sealed draft of a manual import is fine");
}

// The returned copies are values: changing one never changes what the repository holds.
void testTheRepositoryHandsOutCopies()
{
    auto env = inMemoryEnv();
    auto& repo = *env.repository;
    rc::check(repo.insertGroup(rc::makeGroup(env, "g1", 'a'), {rc::groupDraft(env, "g1", "d0", 0, "model")}).success(),
        "setup: a group");
    auto group = repo.findGroupById("g1").value().value();
    group.planTitle = "tampered";
    auto drafts = repo.listGroupDrafts("g1").value();
    drafts[0].title = "tampered";
    drafts[0].provenance->planKey = "tampered";
    rc::check(repo.findGroupById("g1").value()->planTitle == "History page"
            && repo.findById("d0").value()->title != "tampered"
            && repo.findById("d0").value()->provenance->planKey == "model",
        "A copy taken from the repository cannot rewrite it");
}

// PC-5: the follow-up origin has its own source field rules, and the public ports refuse it.
void testTheFollowUpOriginRules()
{
    const auto env = inMemoryEnv();
    const std::vector<RunDraft> drafts {rc::groupDraft(env, "g", "d0", 0, "k0")};
    const auto followUp = [&] {
        RunDraftGroup group = rc::makeGroup(env, "g", 'a');
        group.origin = RunDraftGroupOrigin::FollowUpRequest;
        group.sourceRequestId = "request-1";
        group.contextFingerprint = rc::hex64('d');
        group.sourceRunIds = {"run-1"};
        return group;
    };
    const auto code = [&](const RunDraftGroup& group) {
        const auto result = validateRunDraftGroupInsertion(group, drafts);
        return result.success() ? std::string("ok") : result.error()->code;
    };
    rc::check(code(followUp()) == "ok", "A follow-up group with its request, sealed context and source Run is valid");

    const std::vector<std::pair<const char*, std::function<void(RunDraftGroup&)>>> refused = {
        {"no request id", [](auto& g) { g.sourceRequestId.reset(); }},
        {"an empty request id", [](auto& g) { g.sourceRequestId = ""; }},
        {"a request id over 64", [](auto& g) { g.sourceRequestId = std::string(65, 'r'); }},
        {"no sealed context", [](auto& g) { g.contextFingerprint.reset(); }},
        {"a malformed sealed context", [](auto& g) { g.contextFingerprint = std::string(63, 'a'); }},
        {"no source Run", [](auto& g) { g.sourceRunIds.clear(); }},
        {"a conversation id", [](auto& g) { g.sourceConversationId = "c"; }},
        {"a message id", [](auto& g) { g.sourceMessageId = "m"; }},
        {"a message fingerprint", [](auto& g) { g.sourceMessageFingerprint = rc::hex64('c'); }}};
    for (const auto& [what, edit] : refused) {
        auto group = followUp();
        edit(group);
        rc::check(code(group) == "run_draft_group.invalid_group", std::string("A follow-up group with ") + what + " is refused");
    }
    // A request id belongs to the follow-up origin only.
    auto manual = rc::makeGroup(env, "g", 'a');
    manual.sourceRequestId = "request-1";
    rc::check(code(manual) == "run_draft_group.invalid_group", "A manual import cannot name a request");
    auto chat = rc::makeGroup(env, "g", 'a', RunDraftGroupOrigin::ChatMessage);
    chat.sourceRequestId = "request-1";
    rc::check(code(chat) == "run_draft_group.invalid_group", "A chat message group cannot name a request");

    // The public insert refuses the origin before validation; the other origins pass it.
    const auto refusedPublic = checkRunDraftGroupPublicInsert(followUp());
    rc::check(!refusedPublic.success() && refusedPublic.error()->code == "run_draft_group.follow_up_requires_confirm",
        "The public port refuses a follow-up group");
    rc::check(checkRunDraftGroupPublicInsert(manual).success() && checkRunDraftGroupPublicInsert(chat).success(),
        "...and only that origin");
    InMemoryRunDraftRepository repository;
    const auto viaPort = repository.insertGroup(followUp(), drafts);
    rc::check(!viaPort.success() && viaPort.error()->code == "run_draft_group.follow_up_requires_confirm"
            && !repository.findGroupById("g").value().has_value(),
        "The in-memory insertGroup refuses it and stores nothing");
    // The confirmation path takes only that origin, and a request has at most one group.
    const auto wrongOrigin = repository.insertConfirmedFollowUpGroup(rc::makeGroup(env, "g", 'a'), drafts);
    rc::check(!wrongOrigin.success() && wrongOrigin.error()->code == "run_draft_group.invalid_group", "The confirmation path takes no other origin");
    rc::check(repository.insertConfirmedFollowUpGroup(followUp(), drafts).success(), "The confirmation path saves a follow-up group");
    auto second = followUp();
    second.id = "g2";
    second.idempotencyKey = rc::hex64('b');
    const std::vector<RunDraft> otherDrafts {rc::groupDraft(env, "g2", "d9", 0, "k0")};
    const auto twice = repository.insertConfirmedFollowUpGroup(second, otherDrafts);
    rc::check(!twice.success() && twice.error()->code == "run_draft_group.duplicate_source_request",
        "A request never has two groups");
}

// PC-5B review: the request's own record changes INSIDE the Draft store's critical section, so no reader of the store sees the
// group without it. The proof is one-sided on purpose: a reader that starts while the callback runs must stay blocked until
// the callback returns (a wait that is too short can only miss a bug, never invent one).
void testTheConfirmedGroupAppearsOnlyTogetherWithTheCallersRecord()
{
    const auto env = inMemoryEnv();
    const auto makeFollowUp = [&](const std::string& id, char key) {
        RunDraftGroup group = rc::makeGroup(env, id, key);
        group.origin = RunDraftGroupOrigin::FollowUpRequest;
        group.sourceRequestId = "request-" + id;
        group.contextFingerprint = rc::hex64('d');
        group.sourceRunIds = {"run-1"};
        return group;
    };

    // A refused insert never runs the callback.
    InMemoryRunDraftRepository refusing;
    int calls = 0;
    const auto refused = refusing.insertConfirmedFollowUpGroup(makeFollowUp("g", 'a'), {}, [&] { ++calls; });
    rc::check(!refused.success() && calls == 0, "A refused insert does not run the callback");

    InMemoryRunDraftRepository repository;
    std::atomic<bool> readerStarted {false};
    std::future<bool> readerSawGroup;
    bool readerFinishedInsideCallback = true;
    const auto result = repository.insertConfirmedFollowUpGroup(makeFollowUp("g", 'a'), {rc::groupDraft(env, "g", "d0", 0, "k0")}, [&] {
        ++calls;
        readerSawGroup = std::async(std::launch::async, [&] {
            readerStarted = true;
            const auto found = repository.findGroupById("g");
            return found.success() && found.value().has_value();
        });
        while (!readerStarted) std::this_thread::yield();
        // The reader is running (or about to ask for the lock). Were the store unlocked, it would be done almost at once.
        readerFinishedInsideCallback = readerSawGroup.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    });
    rc::check(result.success() && calls == 1, "The callback runs once, after the rows are in");
    rc::check(!readerFinishedInsideCallback, "A reader of the store waits for the callback to finish");
    rc::check(readerSawGroup.get(), "...and then sees the group");
}
} // namespace

int main()
{
    try {
        testTheInMemoryRepositoryHonoursTheContract();
        testOriginNames();
        testTheProvenanceRule();
        testValidationMessagesNameTheProblem();
        testValidationMessagesForTheSealAndTheOrigin();
        testTheFollowUpOriginRules();
        testTheConfirmedGroupAppearsOnlyTogetherWithTheCallersRecord();
        testTheRepositoryHandsOutCopies();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
