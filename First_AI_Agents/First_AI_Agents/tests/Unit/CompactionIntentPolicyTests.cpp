// B3 phase I: the PURE rules of the compaction intent (ProjectMemoryCompactionIntentPolicy.hpp and the vocabulary of the intent
// port). No storage, no clock, no provider. The checks use throwing require(), so they stay active under NDEBUG.
#include "ai/application/conversations/IProjectMemoryCompactionIntentRepository.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace ai::application;
using std::chrono::microseconds;
using std::chrono::milliseconds;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::chrono::system_clock::time_point atMs(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(milliseconds(ms));
}

ProjectMemoryCompactionIntent intent()
{
    ProjectMemoryCompactionIntent value;
    value.id = "op-1";
    value.projectId = "project-1";
    value.providerId = "provider-1";
    value.modelId = "model-1";
    value.sourceSegmentIds = {"s3", "s1", "s1", "s2"};   // not sorted and with a duplicate: both are part of the identity
    value.sourceContentHash = std::string(64, 'c');
    value.createdAt = atMs(1'700'000'000'123LL);
    return value;
}

ProjectMemoryCompactionRecord recordOf(const ProjectMemoryCompactionIntent& source, ProjectMemoryCompactionStatus status)
{
    ProjectMemoryCompactionRecord record;
    record.id = source.id;
    record.projectId = source.projectId;
    record.providerId = source.providerId;
    record.modelId = source.modelId;
    record.sourceSegmentIds = source.sourceSegmentIds;
    record.sourceContentHash = source.sourceContentHash;
    record.createdAt = source.createdAt;
    record.status = status;
    // Fields that exist only on the record and must never take part in the comparison.
    record.resultSegmentId = status == ProjectMemoryCompactionStatus::Completed ? std::optional<std::string>("result-1") : std::nullopt;
    record.inputTokens = 111;
    record.outputTokens = 22;
    record.costMicros = 333;
    record.costPriceKnown = true;
    record.errorCode = "some.code";
    record.errorMessage = "some message";
    return record;
}

void testTheIdentityIsExactlyTheSevenSharedFieldsInOrder()
{
    const ProjectMemoryCompactionIntent base = intent();
    for (const auto status : {ProjectMemoryCompactionStatus::Completed, ProjectMemoryCompactionStatus::Failed,
             ProjectMemoryCompactionStatus::Cancelled}) {
        require(!compactionIntentIdentityDifference(base, recordOf(base, status)).has_value(),
            "status, result segment, usage, cost and error text must not take part in the comparison");
    }

    struct Mutation {
        CompactionIntentIdentityField field;
        void (*apply)(ProjectMemoryCompactionRecord&);
    };
    const Mutation mutations[] = {
        {CompactionIntentIdentityField::Id, [](ProjectMemoryCompactionRecord& r) { r.id = "op-2"; }},
        {CompactionIntentIdentityField::ProjectId, [](ProjectMemoryCompactionRecord& r) { r.projectId = "project-2"; }},
        {CompactionIntentIdentityField::ProviderId, [](ProjectMemoryCompactionRecord& r) { r.providerId = "provider-2"; }},
        {CompactionIntentIdentityField::ModelId, [](ProjectMemoryCompactionRecord& r) { r.modelId = "model-2"; }},
        {CompactionIntentIdentityField::SourceSegmentIds, [](ProjectMemoryCompactionRecord& r) { r.sourceSegmentIds.push_back("s9"); }},
        {CompactionIntentIdentityField::SourceContentHash, [](ProjectMemoryCompactionRecord& r) { r.sourceContentHash.back() = 'd'; }},
        {CompactionIntentIdentityField::CreatedAt, [](ProjectMemoryCompactionRecord& r) { r.createdAt += milliseconds(1); }},
    };
    for (const Mutation& mutation : mutations) {
        ProjectMemoryCompactionRecord record = recordOf(base, ProjectMemoryCompactionStatus::Failed);
        mutation.apply(record);
        const auto difference = compactionIntentIdentityDifference(base, record);
        require(difference.has_value() && *difference == mutation.field,
            std::string("a changed ") + toString(mutation.field) + " must be reported as exactly that field");
    }

    // The FIRST differing field is reported, in the fixed order.
    ProjectMemoryCompactionRecord two = recordOf(base, ProjectMemoryCompactionStatus::Failed);
    two.modelId = "x";
    two.projectId = "y";
    require(compactionIntentIdentityDifference(base, two) == CompactionIntentIdentityField::ProjectId,
        "with several differences the first one in the fixed order is reported");
}

void testSourceOrderDuplicatesAndTimeAreCompared()
{
    const ProjectMemoryCompactionIntent base = intent();
    ProjectMemoryCompactionRecord reordered = recordOf(base, ProjectMemoryCompactionStatus::Failed);
    reordered.sourceSegmentIds = {"s1", "s2", "s3", "s1"};   // same members, another order
    require(compactionIntentIdentityDifference(base, reordered) == CompactionIntentIdentityField::SourceSegmentIds,
        "the order of the source ids is part of the identity");
    ProjectMemoryCompactionRecord deduplicated = recordOf(base, ProjectMemoryCompactionStatus::Failed);
    deduplicated.sourceSegmentIds = {"s3", "s1", "s2"};
    require(compactionIntentIdentityDifference(base, deduplicated) == CompactionIntentIdentityField::SourceSegmentIds,
        "a duplicate in the source ids is part of the identity");

    ProjectMemoryCompactionRecord subMillisecond = recordOf(base, ProjectMemoryCompactionStatus::Failed);
    subMillisecond.createdAt = base.createdAt + microseconds(999);
    require(!compactionIntentIdentityDifference(base, subMillisecond).has_value(),
        "createdAt is compared in whole milliseconds, as it is stored: microseconds below the millisecond are not an identity");
    // The truncation is the stored one (toward zero for the epoch-relative count), not a rounding.
    ProjectMemoryCompactionIntent earlier = base;
    earlier.createdAt = atMs(1'700'000'000'122LL) + microseconds(999);
    require(compactionIntentIdentityDifference(earlier, recordOf(base, ProjectMemoryCompactionStatus::Failed)) ==
            CompactionIntentIdentityField::CreatedAt,
        "999 microseconds past the previous millisecond is that millisecond, not the next one");

    ProjectMemoryCompactionIntent unicode = base;
    unicode.id = "op-\xE6\xB8\xAC";
    unicode.sourceSegmentIds = {"\xD0\xA1\xD0\xB5\xD0\xB3", "s2"};
    ProjectMemoryCompactionRecord same = recordOf(unicode, ProjectMemoryCompactionStatus::Failed);
    require(!compactionIntentIdentityDifference(unicode, same).has_value(), "a UTF-8 identity compares byte for byte");
    same.sourceSegmentIds[0] = "\xD0\xA1\xD0\xB5\xD0\xB4";
    require(compactionIntentIdentityDifference(unicode, same) == CompactionIntentIdentityField::SourceSegmentIds,
        "a different UTF-8 id differs");
}

void testANewIntentMustBeOpenAndUnmarked()
{
    require(validateNewCompactionIntent(intent()).success(), "a plain open intent is valid");
    ProjectMemoryCompactionIntent unresolved = intent();
    unresolved.state = CompactionIntentState::Unresolved;
    unresolved.unresolvedMarkedAt = atMs(5);
    require(!validateNewCompactionIntent(unresolved).success(), "an unresolved intent is not a new one");
    ProjectMemoryCompactionIntent marked = intent();
    marked.unresolvedMarkedAt = atMs(5);
    require(!validateNewCompactionIntent(marked).success(), "an open intent with a mark is not valid");
    ProjectMemoryCompactionIntent noId = intent();
    noId.id.clear();
    const auto refused = validateNewCompactionIntent(noId);
    require(!refused.success() && refused.error()->code == kIntentInvalidCode, "an intent without an operation id is refused");
    ProjectMemoryCompactionIntent noProject = intent();
    noProject.projectId.clear();
    require(!validateNewCompactionIntent(noProject).success(), "an intent without a project is refused");
}

CompactionIntentWriteOutcome writeOutcome(bool success, CompactionPersistState state, bool inserted)
{
    CompactionIntentWriteOutcome outcome;
    outcome.persist.result = success ? ai::common::Result<void>::ok() : ai::common::Result<void>::failure({"x", "y"});
    outcome.persist.state = state;
    outcome.newIntentInserted = inserted;
    return outcome;
}

void testOnlyAConfirmedNewInsertAdmits()
{
    require(mayDispatch(writeOutcome(true, CompactionPersistState::Committed, true)), "a confirmed new insert admits");
    require(compactionIntentAdmissionOf(writeOutcome(true, CompactionPersistState::Committed, true)) == CompactionIntentAdmission::Admitted,
        "and is named Admitted");
    // Every other combination does not.
    for (const bool success : {false, true}) {
        for (const auto state : {CompactionPersistState::Committed, CompactionPersistState::NotCommitted, CompactionPersistState::Unknown}) {
            for (const bool inserted : {false, true}) {
                const bool expected = success && state == CompactionPersistState::Committed && inserted;
                require(mayDispatch(writeOutcome(success, state, inserted)) == expected,
                    "mayDispatch is true for exactly one combination: success, Committed and a new row");
                require((compactionIntentAdmissionOf(writeOutcome(success, state, inserted)) == CompactionIntentAdmission::Admitted) == expected,
                    "the admission name follows mayDispatch");
            }
        }
    }
    // A duplicate that reports Committed (it must never happen) is still not a new row of this call.
    require(!mayDispatch(writeOutcome(true, CompactionPersistState::Committed, false)),
        "a Committed result without a new row (a duplicate) never admits");
}

void testNoDefaultImpliesHealthyAdmittedOrProven()
{
    const CompactionIntentWriteOutcome write;
    require(!mayDispatch(write), "a default-constructed write outcome never admits");
    require(compactionIntentAdmissionOf(write) == CompactionIntentAdmission::NotAdmitted, "and is NotAdmitted, not NotApplicable");
    require(write.persist.state == CompactionPersistState::Unknown && !write.persist.connection.has_value() && !write.newIntentInserted,
        "a default write is Unknown, with no connection observed and no inserted row");

    const CompactionIntentObservationOutcome observation;
    require(observation.observation == CompactionIntentObservation::NotObserved && observation.source == CompactionIntentObservationSource::None,
        "a default observation observed nothing");
    require(!observation.connection.has_value() && !observation.intent.has_value() && !observation.recordIdentity.has_value() &&
            !observation.error.has_value() && observation.diagnostics.empty() && !observation.diagnosticsIncomplete,
        "a default observation carries no health, no rows and no error");

    const CompactionIntentMutationOutcome mutation;
    require(mutation.mutation == CompactionIntentMutation::Unconfirmed && mutation.persist.state == CompactionPersistState::Unknown,
        "a default mutation is Unconfirmed and its write Unknown");

    require(static_cast<int>(CompactionIntentAdmission::NotApplicable) == 0 && static_cast<int>(CompactionIntentAdmission::Admitted) == 1,
        "the admission order is fixed: NotApplicable, Admitted, NotAdmitted");
}

void testTheObservationClassificationNeverInfersSending()
{
    const ProjectMemoryCompactionIntent base = intent();
    const CompactionIntentIdentity expected = compactionIntentIdentityOf(base);
    const CompactionIntentIdentity matchingRecord = compactionIntentIdentityOf(recordOf(base, ProjectMemoryCompactionStatus::Completed));

    require(classifyCompactionOperation(expected, std::nullopt, std::nullopt) == CompactionIntentObservation::NeitherPresent,
        "no rows is NeitherPresent (which says nothing about delivery)");
    require(classifyCompactionOperation(expected, base, std::nullopt) == CompactionIntentObservation::IntentPresent,
        "a matching intent alone is IntentPresent");
    ProjectMemoryCompactionIntent unresolved = base;
    unresolved.state = CompactionIntentState::Unresolved;
    unresolved.unresolvedMarkedAt = atMs(9);
    require(classifyCompactionOperation(expected, unresolved, std::nullopt) == CompactionIntentObservation::IntentPresent,
        "an unresolved matching intent is still IntentPresent: the state is the row's, not a verdict");
    require(classifyCompactionOperation(expected, std::nullopt, matchingRecord) == CompactionIntentObservation::FinalRecordPresent,
        "a matching record alone is FinalRecordPresent");
    require(classifyCompactionOperation(expected, base, matchingRecord) == CompactionIntentObservation::Inconsistent,
        "both present is Inconsistent");

    ProjectMemoryCompactionIntent foreignIntent = base;
    foreignIntent.modelId = "other-model";
    require(classifyCompactionOperation(expected, foreignIntent, std::nullopt) == CompactionIntentObservation::Inconsistent,
        "a row under the id that is not the expected operation is Inconsistent, never IntentPresent");
    CompactionIntentIdentity foreignRecord = matchingRecord;
    foreignRecord.createdAt += milliseconds(1);
    require(classifyCompactionOperation(expected, std::nullopt, foreignRecord) == CompactionIntentObservation::Inconsistent,
        "a foreign record under the id is Inconsistent, never FinalRecordPresent");
}

void testTheCodesAndWordingAreStable()
{
    require(std::string(kIntentNotRecordedCode) == "project_memory.intent_not_recorded" &&
            std::string(kIntentDuplicateIdCode) == "project_memory.intent_duplicate_id" &&
            std::string(kIntentIdentityConflictCode) == "project_memory.intent_identity_conflict" &&
            std::string(kIntentRecordConflictCode) == "project_memory.intent_record_conflict" &&
            std::string(kIntentObservationInconsistentCode) == "project_memory.intent_observation_inconsistent" &&
            std::string(kIntentRecoveryForbiddenCode) == "project_memory.intent_recovery_forbidden",
        "the machine codes are fixed");
    // The wording makes no claim about delivery, billing or recovery.
    for (const std::string text : {std::string(kIntentNotRecordedMessage), std::string(kIntentDuplicateIdMessage), std::string(kIntentUnresolvedMessage)}) {
        for (const char* forbidden : {"free", "crash", "billed", "charged", "recovered"}) {
            require(text.find(forbidden) == std::string::npos, "the wording must not claim: " + std::string(forbidden));
        }
    }
    require(std::string(kIntentUnresolvedMessage).find("unconfirmed") != std::string::npos &&
            std::string(kIntentUnresolvedMessage).find("may not have started") != std::string::npos,
        "the unresolved wording keeps delivery and cost unconfirmed and the start possible-not");
    require(std::string(toString(CompactionIntentState::Open)) == "open" && std::string(toString(CompactionIntentState::Unresolved)) == "unresolved",
        "the stored state names are fixed");
}

// A fake of the port: the interface can be implemented by a feature test double without any storage.
class RefusingIntentRepository final : public IProjectMemoryCompactionIntentRepository {
public:
    int writes {0};
    CompactionIntentWriteOutcome recordIntentObserved(const ProjectMemoryCompactionIntent&) override
    {
        ++writes;
        return {};
    }
    ai::common::Result<std::optional<ProjectMemoryCompactionIntent>> findIntent(const ProjectMemoryCompactionId&) const override
    {
        return ai::common::Result<std::optional<ProjectMemoryCompactionIntent>>::ok(std::nullopt);
    }
    ai::common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntents(const ProjectId&, std::optional<CompactionIntentState>) const override
    {
        return ai::common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({});
    }
    ai::common::Result<std::vector<ProjectMemoryCompactionIntent>> listIntentsByState(CompactionIntentState) const override
    {
        return ai::common::Result<std::vector<ProjectMemoryCompactionIntent>>::ok({});
    }
    CompactionIntentMutationOutcome markUnresolvedObserved(const CompactionIntentIdentity&, std::chrono::system_clock::time_point) override { return {}; }
    CompactionIntentMutationOutcome closeIntentObserved(const CompactionIntentIdentity&) override { return {}; }
    CompactionIntentObservationOutcome observeOperation(const CompactionIntentIdentity&) const override { return {}; }
};

void testThePortCanBeImplementedByAFeatureDouble()
{
    RefusingIntentRepository port;
    const CompactionIntentWriteOutcome outcome = port.recordIntentObserved(intent());
    require(port.writes == 1 && !mayDispatch(outcome), "a double that reports nothing never admits");
}

} // namespace

int runCompactionIntentPolicyTests()
{
    try {
        testTheIdentityIsExactlyTheSevenSharedFieldsInOrder();
        testSourceOrderDuplicatesAndTimeAreCompared();
        testANewIntentMustBeOpenAndUnmarked();
        testOnlyAConfirmedNewInsertAdmits();
        testNoDefaultImpliesHealthyAdmittedOrProven();
        testTheObservationClassificationNeverInfersSending();
        testTheCodesAndWordingAreStable();
        testThePortCanBeImplementedByAFeatureDouble();
    } catch (const std::exception& exception) {
        std::cerr << "CompactionIntentPolicyTests failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "CompactionIntentPolicyTests: OK\n";
    return 0;
}
