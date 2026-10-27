#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/RunContextAssembler.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ProjectMemorySegment segment(
    std::string id, std::string summary, std::chrono::system_clock::time_point createdAt)
{
    ProjectMemorySegment value;
    value.id = std::move(id);
    value.projectId = "project";
    value.runId = "run-" + value.id;
    value.summary = std::move(summary);
    value.summaryTokenCount = estimateChatTokens(value.summary);
    value.createdAt = createdAt;
    return value;
}

void testEmptyMemoryNeedsNoBudget()
{
    const auto result = assembleRunContext({});
    require(result.success(), "Empty project memory must assemble successfully");
    require(result.value().projectMemorySection.empty(), "Empty memory must render no section");
    require(result.value().includedMemorySegmentIds.empty(), "Empty memory must include no ids");
    require(result.value().estimatedMemoryTokens == 0, "Empty memory must consume zero tokens");
}

void testNonEmptyMemoryRejectsZeroBudget()
{
    RunContextAssemblyRequest request;
    request.memorySegments.push_back(
        segment("one", "summary", std::chrono::system_clock::time_point {1s}));

    const auto result = assembleRunContext(std::move(request));
    require(!result.success(), "Non-empty memory with zero budget must fail");
    require(result.error()->code == "run_context.invalid_memory_budget",
        "Zero memory budget must return the typed error");
}

void testShuffledInputRendersStableChronologicalOrder()
{
    const auto time = std::chrono::system_clock::time_point {5s};
    RunContextAssemblyRequest request;
    request.memorySegments = {
        segment("tie-z", "Z", time),
        segment("newer", "N", time + 1s),
        segment("older", "O", time - 1s),
        segment("tie-a", "A", time),
    };
    request.memoryTokenBudget = 1000;

    const auto result = assembleRunContext(std::move(request));
    require(result.success(), "Shuffled project memory must assemble");
    const std::vector<ProjectMemorySegmentId> expected {"older", "tie-a", "tie-z", "newer"};
    require(result.value().includedMemorySegmentIds == expected,
        "Accepted memory must be ordered by createdAt and id");
    require(result.value().projectMemorySection.find("# Project Memory") == 0,
        "Non-empty memory must render the project-memory heading");
}

void testNewestSegmentsHavePriority()
{
    const auto base = std::chrono::system_clock::time_point {10s};
    const auto old = segment("old", std::string(300, 'o'), base);
    const auto recent = segment("recent", std::string(300, 'n'), base + 1s);

    RunContextAssemblyRequest newestOnly;
    newestOnly.memorySegments = {old, recent};
    RunContextAssemblyRequest measurement;
    measurement.memorySegments = {recent};
    measurement.memoryTokenBudget = 1000;
    const auto measured = assembleRunContext(std::move(measurement));
    require(measured.success(), "Single recent segment must assemble");
    newestOnly.memoryTokenBudget = measured.value().estimatedMemoryTokens;

    const auto result = assembleRunContext(std::move(newestOnly));
    require(result.success(), "Budgeted memory must assemble");
    require(result.value().includedMemorySegmentIds
            == std::vector<ProjectMemorySegmentId> {"recent"},
        "The newest fitting segment must win when both do not fit");
}

void testOversizedNewestIsSkippedWhole()
{
    const auto base = std::chrono::system_clock::time_point {20s};
    const auto small = segment("small-old", "small summary", base);
    const auto oversized = segment("huge-new", std::string(6000, 'x'), base + 1s);

    RunContextAssemblyRequest measurement;
    measurement.memorySegments = {small};
    measurement.memoryTokenBudget = 1000;
    const auto measured = assembleRunContext(std::move(measurement));
    require(measured.success(), "Small segment measurement must succeed");

    RunContextAssemblyRequest request;
    request.memorySegments = {small, oversized};
    request.memoryTokenBudget = measured.value().estimatedMemoryTokens;
    const auto result = assembleRunContext(std::move(request));
    require(result.success(), "Assembler must skip an oversized segment");
    require(result.value().includedMemorySegmentIds
            == std::vector<ProjectMemorySegmentId> {"small-old"},
        "Oversized newest segment must be skipped whole while an older one may fit");
    require(result.value().projectMemorySection.find(std::string(100, 'x')) == std::string::npos,
        "Assembler must never partially include an oversized segment");
}

} // namespace

int runRunContextAssemblerTests()
{
    testEmptyMemoryNeedsNoBudget();
    testNonEmptyMemoryRejectsZeroBudget();
    testShuffledInputRendersStableChronologicalOrder();
    testNewestSegmentsHavePriority();
    testOversizedNewestIsSkippedWhole();
    return 0;
}
