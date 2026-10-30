#include <iostream>
#include <exception>

int runSqliteRepositoryTests();
int runModelExecutionRepositoryTests();
int runChatServicePricingTests();
int runAgentToolAuditTests();
int runRunDraftRepositoryTests();
int runOutcomeBriefRepositoryTests();
int runFollowUpPlanRequestRepositoryTests();
int runExecutionManifestRepositoryTests();
int runManifestTaskExecutionRepositoryTests();
int runManifestWorkLedgerRepositoryTests();
int runManifestVerificationContinuationRepositoryTests();
int runRunWorkspaceScopeRepositoryTests();
int runVerificationEnvironmentSnapshotRepositoryTests();
int runVerificationBootstrapRepositoryTests();
int runSqlitePreMigrationBackupTests();

int main()
{
    // Plain C++ entry point: native sqlite3 needs no Qt application object,
    // plugin search path, or event loop. Keep persistence failures isolated
    // from provider/runtime test output.
    try {
        runSqliteRepositoryTests();
        runModelExecutionRepositoryTests();
        runChatServicePricingTests();
        runAgentToolAuditTests();
        runRunDraftRepositoryTests();
        runOutcomeBriefRepositoryTests();
        runFollowUpPlanRequestRepositoryTests();
        runExecutionManifestRepositoryTests();
        runManifestTaskExecutionRepositoryTests();
        runManifestWorkLedgerRepositoryTests();
        runManifestVerificationContinuationRepositoryTests();
        runRunWorkspaceScopeRepositoryTests();
        runVerificationEnvironmentSnapshotRepositoryTests();
        runVerificationBootstrapRepositoryTests();
        runSqlitePreMigrationBackupTests();
        std::cout << "SQLite repository tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        // Report the actual contract assertion to CTest instead of allowing a
        // Debug CRT unhandled-exception dialog to block headless verification.
        std::cerr << "SQLite repository tests failed: " << exception.what() << '\n';
        return 1;
    }
}
