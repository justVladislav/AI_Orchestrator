#include "ai/orchestration/Workflow.hpp"

#include "ai/providers/ModelType.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

namespace fs = std::filesystem;

std::optional<fs::path> findProjectRootFrom(fs::path start)
{
    // The executable may be launched from the source tree or from a build
    // directory, so walk upward until the known project layout is found.
    for (fs::path candidate = std::move(start); !candidate.empty(); candidate = candidate.parent_path()) {
        if (fs::exists(candidate / "CMakeLists.txt")
            && fs::exists(candidate / "sourceFiles")
            && fs::exists(candidate / "includes")) {
            return candidate;
        }

        const fs::path nested = candidate / "First_AI_Agents";
        if (fs::exists(nested / "CMakeLists.txt")
            && fs::exists(nested / "sourceFiles")
            && fs::exists(nested / "includes")) {
            return nested;
        }

        if (candidate == candidate.root_path()) {
            break;
        }
    }

    return std::nullopt;
}

fs::path findProjectRoot()
{
    if (const auto root = findProjectRootFrom(fs::current_path())) {
        return *root;
    }

    if (const auto root = findProjectRootFrom(fs::path(__FILE__).parent_path())) {
        return *root;
    }

    return fs::current_path();
}

void printRunSummary(const ai::orchestration::OrchestrationResult& result)
{
    if (!result.message.empty()) {
        std::cout << result.message << '\n';
    }

    if (!result.runId.empty()) {
        std::cout << "Run ID: " << result.runId << '\n';
    }

    if (!result.runRoot.empty()) {
        std::cout << "Workspace: " << result.runRoot.string() << '\n';
    }
}

} // namespace

int main()
{
    try {
        const fs::path projectRoot = findProjectRoot();

        ai::orchestration::OrchestrationConfig config;
        config.projectRoot = projectRoot;
        config.workspaceRoot = projectRoot / "workspace";
        config.mode = ai::orchestration::WorkflowMode::FocusedSequential;
        config.focusedScope = ai::orchestration::FocusedScope::ProjectContextCollection;
        config.focusedArchitectModel = ai::providers::ModelTypeOpenAi56Sol;
        config.focusedImplementationModel = ai::providers::ModelTypeClaudeSonnet;
        config.focusedDocumentationModel = ai::providers::ModelTypeClaudeHaiku;

        ai::orchestration::Workflow workflow {std::move(config)};
        const ai::orchestration::OrchestrationResult result = workflow.run();

        printRunSummary(result);
        return result.success ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Fatal orchestration error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Fatal orchestration error: unknown exception\n";
        return EXIT_FAILURE;
    }
}
