#include "ai/application/RunContextAssembler.hpp"

#include "ai/application/conversations/ChatContextAssembler.hpp"

#include <algorithm>
#include <string_view>

namespace ai::application {
namespace {

common::Error runContextError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

bool chronologicalLess(const ProjectMemorySegment& left, const ProjectMemorySegment& right)
{
    if (left.createdAt != right.createdAt) {
        return left.createdAt < right.createdAt;
    }
    return left.id < right.id;
}

std::string renderMemorySection(const std::vector<const ProjectMemorySegment*>& segments)
{
    if (segments.empty()) {
        return {};
    }

    std::string section = "# Project Memory\n\n";
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index != 0) {
            section += "\n\n---\n\n";
        }
        section += segments[index]->summary;
    }
    return section;
}

} // namespace

common::Result<RunContextAssembly> assembleRunContext(RunContextAssemblyRequest request)
{
    RunContextAssembly output;
    if (request.memorySegments.empty()) {
        return common::Result<RunContextAssembly>::ok(std::move(output));
    }
    if (request.memoryTokenBudget == 0) {
        return common::Result<RunContextAssembly>::failure(runContextError(
            "run_context.invalid_memory_budget",
            "Project memory requires a non-zero token budget"));
    }

    std::stable_sort(
        request.memorySegments.begin(), request.memorySegments.end(), chronologicalLess);

    std::vector<const ProjectMemorySegment*> accepted;
    for (auto cursor = request.memorySegments.rbegin();
         cursor != request.memorySegments.rend(); ++cursor) {
        std::vector<const ProjectMemorySegment*> candidate = accepted;
        candidate.push_back(&*cursor);
        std::sort(candidate.begin(), candidate.end(),
            [](const ProjectMemorySegment* left, const ProjectMemorySegment* right) {
                return chronologicalLess(*left, *right);
            });

        const std::string rendered = renderMemorySection(candidate);
        if (estimateChatTokens(rendered) <= request.memoryTokenBudget) {
            accepted = std::move(candidate);
        }
    }

    output.projectMemorySection = renderMemorySection(accepted);
    if (!output.projectMemorySection.empty()) {
        output.estimatedMemoryTokens = estimateChatTokens(output.projectMemorySection);
    }
    for (const ProjectMemorySegment* segment : accepted) {
        output.includedMemorySegmentIds.push_back(segment->id);
    }
    return common::Result<RunContextAssembly>::ok(std::move(output));
}

} // namespace ai::application
