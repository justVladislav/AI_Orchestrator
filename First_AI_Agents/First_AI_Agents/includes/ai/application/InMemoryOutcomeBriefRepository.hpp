#pragma once

#include "ai/application/planning/IOutcomeBriefRepository.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace ai::application {

// The in-memory twin of SqliteOutcomeBriefRepository: it runs the very same contract suite. It has no `runs` table, so the Run
// authority (write and read, see IOutcomeBriefRepository.hpp) comes from the injected lookup and goes through the very same shared
// checks. WITHOUT a lookup the adapter knows no Runs: it keeps only briefs, checks no authority, and a Run that already has a
// history keeps the project of that history (`project_mismatch`). That mode is for tests of the shared rules; whatever stands in
// for the SQLite adapter in a test of a consumer must be given a lookup.
class InMemoryOutcomeBriefRepository final : public IOutcomeBriefRepository {
public:
    explicit InMemoryOutcomeBriefRepository(OutcomeBriefRunLookup runLookup = {});

    common::Result<OutcomeBriefAppendResult> append(OutcomeBrief brief, OutcomeBriefExpectedHead expected) override;
    [[nodiscard]] common::Result<std::optional<OutcomeBriefRevision>> findHead(const RunId& runId) const override;
    [[nodiscard]] common::Result<std::optional<OutcomeBriefRevision>> findRevision(
        const OutcomeBriefRevisionId& id) const override;
    [[nodiscard]] common::Result<std::vector<OutcomeBriefRevision>> listRevisions(
        const RunId& runId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<OutcomeBriefRevision>> listHeads(
        const ProjectId& projectId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::optional<OutcomeBriefHeadVersion>> findHeadVersionForAppend(
        const RunId& runId) const override;

private:
    [[nodiscard]] std::optional<OutcomeBriefRunOwner> ownerOf(const RunId& runId) const;
    // The read authority of one stored revision; a no-op without a lookup.
    [[nodiscard]] common::Result<void> checkRead(const OutcomeBriefRevision& revision, bool isHead) const;

    OutcomeBriefRunLookup runLookup_;
    mutable std::mutex mutex_;
    // Per Run, ascending by ordinal, never modified once appended. The last element is the head.
    std::map<RunId, std::vector<OutcomeBriefRevision>> revisionsByRun_;
};

} // namespace ai::application
