#include "ai/application/RunPathClaimRegistry.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

using namespace ai::application;

using PathClaim = RunPathClaimRegistry::PathClaim;

void testDisjointPathsBothClaim()
{
    RunPathClaimRegistry registry;
    const auto a = registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}});
    const auto b = registry.tryClaim("run-1", "cs-b", {{"src/b.cpp", "chg-b"}});
    assert(a.claimed);
    assert(b.claimed);
}

void testOverlappingPathRejectsSecondClaimant()
{
    RunPathClaimRegistry registry;
    const auto a = registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}});
    assert(a.claimed);

    const auto b = registry.tryClaim("run-1", "cs-b",
        {{"src/b.cpp", "chg-b1"}, {"src/a.cpp", "chg-b2"}});
    assert(!b.claimed);
    assert(b.conflictingChangeId == "chg-a");
    assert(b.conflictingChangeSetId == "cs-a");

    // A third claimant racing the same already-claimed path must also fail,
    // proving A's claim is untouched by B's failed attempt.
    const auto c = registry.tryClaim("run-1", "cs-c", {{"src/a.cpp", "chg-c"}});
    assert(!c.claimed);
    assert(c.conflictingChangeId == "chg-a");
    assert(c.conflictingChangeSetId == "cs-a");
}

void testSameChangeSetIdReclaimIsIdempotent()
{
    RunPathClaimRegistry registry;
    const auto first = registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}});
    const auto second = registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}});
    assert(first.claimed);
    assert(second.claimed);
}

void testReleaseFreesPathForOtherClaimant()
{
    RunPathClaimRegistry registry;
    const auto a = registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}});
    assert(a.claimed);

    registry.release("run-1", "cs-a");

    const auto b = registry.tryClaim("run-1", "cs-b", {{"src/a.cpp", "chg-b"}});
    assert(b.claimed);
}

void testReleaseRunClearsEverything()
{
    RunPathClaimRegistry registry;
    assert(registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}}).claimed);
    assert(registry.tryClaim("run-1", "cs-b", {{"src/b.cpp", "chg-b"}}).claimed);

    registry.releaseRun("run-1");

    assert(registry.tryClaim("run-1", "cs-c", {{"src/a.cpp", "chg-c"}}).claimed);
    assert(registry.tryClaim("run-1", "cs-d", {{"src/b.cpp", "chg-d"}}).claimed);
}

void testDifferentRunsAreIndependent()
{
    RunPathClaimRegistry registry;
    const auto run1 = registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}});
    const auto run2 = registry.tryClaim("run-2", "cs-b", {{"src/a.cpp", "chg-b"}});
    assert(run1.claimed);
    assert(run2.claimed);
}

void testAllOrNothingOnPartialCollision()
{
    RunPathClaimRegistry registry;
    assert(registry.tryClaim("run-1", "cs-a", {{"src/a.cpp", "chg-a"}}).claimed);

    // cs-b's request has one free path and one already-owned path; the
    // whole call must fail and claim nothing, including the free path.
    const auto b = registry.tryClaim("run-1", "cs-b",
        {{"src/free.cpp", "chg-b1"}, {"src/a.cpp", "chg-b2"}});
    assert(!b.claimed);

    // Proven by a third claimant still being able to take the "free" path.
    const auto c = registry.tryClaim("run-1", "cs-c", {{"src/free.cpp", "chg-c"}});
    assert(c.claimed);
}

} // namespace

int runRunPathClaimRegistryTests()
{
    testDisjointPathsBothClaim();
    testOverlappingPathRejectsSecondClaimant();
    testSameChangeSetIdReclaimIsIdempotent();
    testReleaseFreesPathForOtherClaimant();
    testReleaseRunClearsEverything();
    testDifferentRunsAreIndependent();
    testAllOrNothingOnPartialCollision();
    return 0;
}
