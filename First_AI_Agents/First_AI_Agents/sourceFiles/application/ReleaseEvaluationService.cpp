#include "ai/application/ReleaseEvaluationService.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ai::application {
namespace {

using ai::common::Error;

constexpr std::array<ReleaseEvaluationMode, 3> kPrimaryModes {
    ReleaseEvaluationMode::SolBaseline,
    ReleaseEvaluationMode::ManualSolWorkers,
    ReleaseEvaluationMode::ManifestSolWorkers};

Error invalid(std::string message)
{
    return {"release_evaluation.invalid_measurements", std::move(message)};
}

const ReleaseEvaluationMeasurement* findMode(
    const std::vector<ReleaseEvaluationMeasurement>& values,
    ReleaseEvaluationMode mode)
{
    const auto it = std::find_if(values.begin(), values.end(), [mode](const auto& value) {
        return value.mode == mode;
    });
    return it == values.end() ? nullptr : &*it;
}

std::string percent(std::uint32_t basisPoints)
{
    std::ostringstream out;
    out << (basisPoints / 100) << '.' << std::setw(2) << std::setfill('0')
        << (basisPoints % 100) << '%';
    return out.str();
}

} // namespace

const char* releaseEvaluationModeName(ReleaseEvaluationMode mode) noexcept
{
    switch (mode) {
    case ReleaseEvaluationMode::SolBaseline: return "Sol baseline";
    case ReleaseEvaluationMode::ManualSolWorkers: return "Manual Sol workers";
    case ReleaseEvaluationMode::ManifestSolWorkers: return "Manifest Sol workers";
    case ReleaseEvaluationMode::TerraWorkerSubstitution: return "Terra worker substitution";
    }
    return "Unknown";
}

ai::common::Result<ReleaseEvaluationReport> ReleaseEvaluationService::evaluate(
    std::vector<ReleaseEvaluationMeasurement> measurements) const
{
    if (measurements.size() < kPrimaryModes.size() || measurements.size() > 4) {
        return ai::common::Result<ReleaseEvaluationReport>::failure(
            invalid("Expected the three primary runs and at most one Terra substitution"));
    }

    const std::string task = measurements.front().taskFingerprint;
    const std::string revision = measurements.front().repositoryRevision;
    if (task.empty() || revision.empty()) {
        return ai::common::Result<ReleaseEvaluationReport>::failure(
            invalid("Task fingerprint and repository revision must be non-empty"));
    }

    for (const auto& measurement : measurements) {
        if (measurement.taskFingerprint != task
            || measurement.repositoryRevision != revision) {
            return ai::common::Result<ReleaseEvaluationReport>::failure(
                invalid("Every run must use the same task fingerprint and repository revision"));
        }
        if (measurement.completenessBasisPoints > 10'000) {
            return ai::common::Result<ReleaseEvaluationReport>::failure(
                invalid("Completeness cannot exceed 10000 basis points"));
        }
    }

    ReleaseEvaluationReport report;
    for (const auto mode : kPrimaryModes) {
        const auto count = std::count_if(
            measurements.begin(), measurements.end(), [mode](const auto& value) {
                return value.mode == mode;
            });
        if (count != 1) {
            return ai::common::Result<ReleaseEvaluationReport>::failure(
                invalid("Each primary Sol mode must occur exactly once"));
        }
        report.primaryRuns.push_back(*findMode(measurements, mode));
    }

    const auto terraCount = std::count_if(
        measurements.begin(), measurements.end(), [](const auto& value) {
            return value.mode == ReleaseEvaluationMode::TerraWorkerSubstitution;
        });
    if (terraCount > 1 || measurements.size() != kPrimaryModes.size() + terraCount) {
        return ai::common::Result<ReleaseEvaluationReport>::failure(
            invalid("Terra may appear once and only as the optional isolated substitution"));
    }
    if (terraCount == 1) {
        report.terraSubstitution = *findMode(
            measurements, ReleaseEvaluationMode::TerraWorkerSubstitution);
    }

    const auto& baseline = report.primaryRuns.front();
    const auto& target = report.primaryRuns.back();
    if (target.completenessBasisPoints < baseline.completenessBasisPoints) {
        report.gateFailures.push_back("Target completeness is lower than the Sol baseline");
    }
    if (target.hiddenManualRepair) {
        report.gateFailures.push_back("Target required hidden manual repair");
    }
    report.releaseGatePassed = report.gateFailures.empty();
    return ai::common::Result<ReleaseEvaluationReport>::ok(std::move(report));
}

std::string ReleaseEvaluationReport::toMarkdown() const
{
    std::ostringstream out;
    out << "| Mode | Wall ms | Provider calls | Tool calls | Tokens in/out | Cost (micros) "
           "| Conflicts | Validation failures | Missed integration | Manual corrections "
           "| Completeness | Hidden repair |\n"
           "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|\n";
    const auto write = [&out](const ReleaseEvaluationMeasurement& value) {
        out << "| " << releaseEvaluationModeName(value.mode)
            << " | " << value.wallTimeMs
            << " | " << value.providerCalls
            << " | " << value.auditedToolCalls
            << " | " << value.inputTokens << '/' << value.outputTokens
            << " | " << value.costMicros
            << " | " << value.conflicts
            << " | " << value.validationFailures
            << " | " << value.missedIntegrationItems
            << " | " << value.manualCorrections
            << " | " << percent(value.completenessBasisPoints)
            << " | " << (value.hiddenManualRepair ? "yes" : "no") << " |\n";
    };
    for (const auto& value : primaryRuns) write(value);
    if (terraSubstitution.has_value()) write(*terraSubstitution);

    out << "\nRelease gate: **" << (releaseGatePassed ? "PASS" : "FAIL") << "**.\n";
    for (const auto& failure : gateFailures) out << "- " << failure << '\n';
    return out.str();
}

} // namespace ai::application
