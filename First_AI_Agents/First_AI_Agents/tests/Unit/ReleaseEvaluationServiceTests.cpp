#include "ai/application/analytics/ReleaseEvaluationService.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ai::application;

ReleaseEvaluationMeasurement measurement(
    ReleaseEvaluationMode mode, std::uint32_t completeness = 10'000)
{
    ReleaseEvaluationMeasurement value;
    value.mode = mode;
    value.taskFingerprint = "sha256:phase6-release-task";
    value.repositoryRevision = "phase6-fixture-revision";
    value.wallTimeMs = 10;
    value.providerCalls = 3;
    value.auditedToolCalls = 4;
    value.inputTokens = 120;
    value.outputTokens = 60;
    value.costMicros = 300;
    value.completenessBasisPoints = completeness;
    return value;
}

std::vector<ReleaseEvaluationMeasurement> primaryMeasurements()
{
    return {
        measurement(ReleaseEvaluationMode::SolBaseline),
        measurement(ReleaseEvaluationMode::ManualSolWorkers),
        measurement(ReleaseEvaluationMode::ManifestSolWorkers)};
}

struct ScriptedTurn {
    std::uint64_t inputTokens;
    std::uint64_t outputTokens;
    std::uint64_t auditedToolCalls;
};

ReleaseEvaluationMeasurement runNonBillableFixture(ReleaseEvaluationMode mode)
{
    const auto started = std::chrono::steady_clock::now();
    const std::set<std::string> expected {
        "CMakeLists.txt", "qml/AppShell.qml", "tests/AppShellTests.cpp"};
    std::set<std::string> proposed;
    std::vector<ScriptedTurn> turns;

    switch (mode) {
    case ReleaseEvaluationMode::SolBaseline:
        turns = {{64, 32, 4}};
        proposed = expected;
        break;
    case ReleaseEvaluationMode::ManualSolWorkers:
        turns = {{20, 10, 0}, {30, 15, 2}, {30, 15, 2}};
        proposed = expected;
        break;
    case ReleaseEvaluationMode::ManifestSolWorkers:
        turns = {{24, 12, 0}, {28, 14, 2}, {28, 14, 2}};
        proposed = expected;
        break;
    case ReleaseEvaluationMode::TerraWorkerSubstitution:
        turns = {{20, 10, 0}, {30, 15, 2}, {22, 11, 2}};
        proposed = expected;
        break;
    }

    ReleaseEvaluationMeasurement value;
    value.mode = mode;
    value.taskFingerprint = "sha256:phase6-cmake-layout-test-integration";
    value.repositoryRevision = "phase6-fixture-r1";
    value.providerCalls = turns.size();
    for (const auto& turn : turns) {
        value.inputTokens += turn.inputTokens;
        value.outputTokens += turn.outputTokens;
        value.auditedToolCalls += turn.auditedToolCalls;
    }
    value.costMicros = static_cast<std::int64_t>(
        value.inputTokens + value.outputTokens * 3);
    value.missedIntegrationItems = expected.size() - proposed.size();
    value.completenessBasisPoints = static_cast<std::uint32_t>(
        proposed.size() * 10'000 / expected.size());
    value.wallTimeMs = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count()));
    return value;
}

void testPrimaryGatePassesAndRendersEveryRequiredMetric()
{
    ReleaseEvaluationService service;
    auto values = primaryMeasurements();
    values.push_back(measurement(ReleaseEvaluationMode::TerraWorkerSubstitution));

    const auto result = service.evaluate(std::move(values));
    assert(result.success());
    assert(result.value().releaseGatePassed);
    assert(result.value().primaryRuns.size() == 3);
    assert(result.value().terraSubstitution.has_value());

    const std::string markdown = result.value().toMarkdown();
    for (const char* expected : {
             "Sol baseline", "Manual Sol workers", "Manifest Sol workers",
             "Terra worker substitution", "Provider calls", "Tool calls",
             "Tokens in/out", "Conflicts", "Validation failures",
             "Missed integration", "Manual corrections", "Completeness",
             "Release gate: **PASS**"}) {
        assert(markdown.find(expected) != std::string::npos);
    }
}

void testDifferentTaskOrRevisionCannotBeCompared()
{
    ReleaseEvaluationService service;
    auto values = primaryMeasurements();
    values.back().repositoryRevision = "different-revision";

    const auto result = service.evaluate(std::move(values));
    assert(!result.success());
    assert(result.error()->code == "release_evaluation.invalid_measurements");
}

void testTargetCannotHideRepairOrRegressCompleteness()
{
    ReleaseEvaluationService service;
    auto values = primaryMeasurements();
    values.back().completenessBasisPoints = 9'999;
    values.back().hiddenManualRepair = true;

    const auto result = service.evaluate(std::move(values));
    assert(result.success());
    assert(!result.value().releaseGatePassed);
    assert(result.value().gateFailures.size() == 2);
}

void testTerraCannotReplaceAPrimarySolMode()
{
    ReleaseEvaluationService service;
    auto values = primaryMeasurements();
    values[1].mode = ReleaseEvaluationMode::TerraWorkerSubstitution;

    const auto result = service.evaluate(std::move(values));
    assert(!result.success());
}

void testReproducibleThreeModeFixtureAndIsolatedTerraSubstitution()
{
    ReleaseEvaluationService service;
    std::vector<ReleaseEvaluationMeasurement> values {
        runNonBillableFixture(ReleaseEvaluationMode::SolBaseline),
        runNonBillableFixture(ReleaseEvaluationMode::ManualSolWorkers),
        runNonBillableFixture(ReleaseEvaluationMode::ManifestSolWorkers),
        runNonBillableFixture(ReleaseEvaluationMode::TerraWorkerSubstitution)};

    const auto report = service.evaluate(std::move(values));
    assert(report.success());
    assert(report.value().releaseGatePassed);
    for (const auto& run : report.value().primaryRuns) {
        assert(run.missedIntegrationItems == 0);
        assert(run.manualCorrections == 0);
        assert(!run.hiddenManualRepair);
        assert(run.completenessBasisPoints == 10'000);
    }
    std::cout << report.value().toMarkdown();
}

} // namespace

int main()
{
    testPrimaryGatePassesAndRendersEveryRequiredMetric();
    testDifferentTaskOrRevisionCannotBeCompared();
    testTargetCannotHideRepairOrRegressCompleteness();
    testTerraCannotReplaceAPrimarySolMode();
    testReproducibleThreeModeFixtureAndIsolatedTerraSubstitution();
    return 0;
}
