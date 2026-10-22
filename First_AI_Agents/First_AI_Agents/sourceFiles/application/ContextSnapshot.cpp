#include "ai/application/ContextSnapshot.hpp"

#include <chrono>
#include <ctime>
#include <sstream>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

std::string formatTimestamp(std::chrono::system_clock::time_point value)
{
    const auto asTimeT = std::chrono::system_clock::to_time_t(value);
    std::tm tmValue {};
#if defined(_WIN32)
    gmtime_s(&tmValue, &asTimeT);
#else
    gmtime_r(&asTimeT, &tmValue);
#endif
    std::ostringstream stream;
    stream << (tmValue.tm_year + 1900) << '-'
           << (tmValue.tm_mon < 9 ? "0" : "") << (tmValue.tm_mon + 1) << '-'
           << (tmValue.tm_mday < 10 ? "0" : "") << tmValue.tm_mday << 'T'
           << (tmValue.tm_hour < 10 ? "0" : "") << tmValue.tm_hour << ':'
           << (tmValue.tm_min < 10 ? "0" : "") << tmValue.tm_min << ':'
           << (tmValue.tm_sec < 10 ? "0" : "") << tmValue.tm_sec << 'Z';
    return stream.str();
}

} // namespace

common::Result<RunContextSnapshot> buildRunContextSnapshot(
    const RunId& runId,
    const ProjectId& projectId,
    std::string projectDescription,
    std::string runTask,
    std::vector<ContextSnapshotFile> selectedFiles,
    std::optional<std::uint64_t> maxTotalBytes,
    std::string projectMemorySection,
    std::vector<ProjectMemorySegmentId> includedProjectMemorySegmentIds)
{
    std::uint64_t total =
        projectDescription.size() + projectMemorySection.size() + runTask.size();
    for (const auto& file : selectedFiles) {
        total += file.byteSize;
    }

    if (maxTotalBytes.has_value() && total > *maxTotalBytes) {
        // An explicit, typed Result failure: the caller must select fewer
        // files, summarize, or raise the budget - this function never drops
        // bytes on the caller's behalf.
        return common::Result<RunContextSnapshot>::failure(error(
            "context_snapshot.budget_exceeded",
            "Context snapshot size " + std::to_string(total)
                + " bytes exceeds the configured budget of "
                + std::to_string(*maxTotalBytes) + " bytes"));
    }

    RunContextSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = projectId;
    snapshot.projectDescription = std::move(projectDescription);
    snapshot.projectMemorySection = std::move(projectMemorySection);
    snapshot.includedProjectMemorySegmentIds =
        std::move(includedProjectMemorySegmentIds);
    snapshot.runTask = std::move(runTask);
    snapshot.files = std::move(selectedFiles);
    snapshot.totalByteCount = total;
    snapshot.createdAt = std::chrono::system_clock::now();
    return common::Result<RunContextSnapshot>::ok(std::move(snapshot));
}

std::string renderRunContextSnapshot(const RunContextSnapshot& snapshot)
{
    std::ostringstream out;
    out << "# Run Context Snapshot\n\n";
    out << "Run: " << snapshot.runId << "\n";
    out << "Project: " << snapshot.projectId << "\n";
    out << "Created: " << formatTimestamp(snapshot.createdAt) << "\n";
    out << "Total bytes: " << snapshot.totalByteCount << "\n\n";

    out << "## Project Description\n\n";
    out << (snapshot.projectDescription.empty() ? "(empty)" : snapshot.projectDescription) << "\n\n";

    out << "## Project Memory\n\n";
    if (snapshot.projectMemorySection.empty()) {
        out << "(none selected)\n\n";
    } else {
        out << snapshot.projectMemorySection << "\n\n";
        out << "Included segment ids:\n";
        for (const auto& id : snapshot.includedProjectMemorySegmentIds) {
            out << "- " << id << "\n";
        }
        out << "\n";
    }

    out << "## Run Task\n\n";
    out << (snapshot.runTask.empty() ? "(empty)" : snapshot.runTask) << "\n\n";

    out << "## Context Files\n\n";
    if (snapshot.files.empty()) {
        out << "(none selected)\n";
    } else {
        for (const auto& file : snapshot.files) {
            out << "- " << file.relativePath << " (" << file.byteSize << " bytes)";
            if (!file.content.has_value()) {
                out << " [content stored as a separate artifact]";
            }
            out << "\n";
        }
    }
    return out.str();
}

} // namespace ai::application
