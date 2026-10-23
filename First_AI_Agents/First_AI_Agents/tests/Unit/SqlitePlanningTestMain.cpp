#include <exception>
#include <iostream>

// The planning repositories (run drafts, Outcome Brief, follow-up plan requests) already have shared
// repository contracts that run against SQLite inside AIOrchestrator_Persistence_tests. This scoped entry
// point runs exactly those suites, so a change to their storage code can be checked without the whole
// persistence executable.
int runRunDraftRepositoryTests();
int runOutcomeBriefRepositoryTests();
int runFollowUpPlanRequestRepositoryTests();

int main()
{
    try {
        runRunDraftRepositoryTests();
        runOutcomeBriefRepositoryTests();
        runFollowUpPlanRequestRepositoryTests();
        std::cout << "SQLite planning repository tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SQLite planning repository tests failed: " << exception.what() << '\n';
        return 1;
    }
}
