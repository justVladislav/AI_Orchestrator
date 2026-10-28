#pragma once

#include "ai/application/planning/IOutcomeBriefRepository.hpp"
#include "ai/application/execution/IRunRepository.hpp"
#include "ai/application/planning/OutcomeBrief.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// PC-4D: what the read-only `Outcome` block of Run Details shows for one Run. Everything here is the Backend's: the Qt adapter only
// copies it into a QVariantMap and the QML only renders it, so no UI code classifies a state, counts a decision or reads a
// fingerprint out of the text.
struct OutcomeBriefMemberView {
    ChangeSetId changeSetId;
    std::string decision;   // toString(OutcomeProposalDecision)
};

struct OutcomeBriefView {
    enum class Availability {
        NotApplicable,   // not an outcome Run (Mock / profile workflow, a handoff attempt, a Run that is still going, an unknown Run): no block
        NotRecorded,     // a terminal outcome Run whose brief the reconciler has not written (yet): the block says so
        Present,         // the current revision of the brief
        Unreadable,      // the Run or the brief could not be read (a stale or corrupt head is a fault, never an empty block)
    };
    Availability availability {Availability::NotApplicable};

    RunId runId;

    // Unreadable only: the Backend's own code and message.
    std::string errorCode;
    std::string errorMessage;

    // Present only.
    std::string state;                       // toString(OutcomeBriefState)
    bool resolved {false};                   // resolved_*
    bool pending {false};                    // something can still change: awaiting_* or attention_required
    std::string terminalStatus;              // toString(RunStatus) of the Run the brief describes
    std::vector<std::string> reasons;        // toString(OutcomeBriefReason), attention_required only, in the brief's order
    OutcomeBriefCounts counts;
    std::vector<OutcomeBriefMemberView> members;   // in ledger order
    bool recoveryEligible {false};
    bool requiresInspection {false};
    std::optional<RunId> continuationSourceRunId;
    std::optional<RunId> continuationChildRunId;
    std::optional<std::string> manualHandoffId;
    std::uint64_t revision {0};              // the ordinal of the current revision = how many revisions this Run's brief has had
    std::chrono::system_clock::time_point recordedAt;
    std::string sourceStateFingerprint;
    std::string contentFingerprint;
    // The stored, bounded rendering (valid UTF-8, at most 64 KiB). It carries provider- and human-authored prose labelled as untrusted
    // DATA: a consumer shows it as plain text and never treats any of it as an instruction.
    std::string renderedText;
};

// PC-4D: the read-only projection of the Outcome Brief of ONE Run. It reads through a const Run repository and a const brief
// repository, so it cannot write, reconcile or start anything; it calls no provider.
class OutcomeBriefReadModelService {
public:
    OutcomeBriefReadModelService(const IRunRepository& runs, const IOutcomeBriefRepository& briefs);

    // Never fails: a storage problem is an `Unreadable` view carrying the Backend's code and message, so a caller shows a fault instead
    // of silently showing nothing. An empty id is `NotApplicable`.
    [[nodiscard]] OutcomeBriefView viewForRun(const RunId& runId) const;

private:
    const IRunRepository& runs_;
    const IOutcomeBriefRepository& briefs_;
};

} // namespace ai::application
