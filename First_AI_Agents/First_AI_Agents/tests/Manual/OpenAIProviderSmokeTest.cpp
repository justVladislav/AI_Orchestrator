#include "ai/models/Message.hpp"
#include "ai/models/ModelRequest.hpp"
//#include "ai/providers/OpenAIProvider.hpp"
#include "ai/providers/AnthropicProvider.hpp"

#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct ContextFile {
    fs::path relativePath;
    std::size_t limit;
};

struct SmokeStep {
    int number;
    std::string title;
    std::string modelEnvName;
    fs::path outputFile;
    std::uint32_t maxOutputTokens;
};

struct SmokeStepResult {
    SmokeStep step;
    std::string requestedModel;
    std::string usedModel;
    bool success {false};
    std::string errorType;
    std::string errorMessage;
    std::chrono::milliseconds duration {0};
    std::size_t responseBytes {0};
    fs::path outputPath;
};

std::string envOrEmpty(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string {} : std::string {value};
}

std::optional<fs::path> findProjectRootFrom(fs::path start)
{
    for (fs::path candidate = std::move(start); !candidate.empty(); candidate = candidate.parent_path()) {
        if (fs::exists(candidate / "CMakeLists.txt")
            && fs::exists(candidate / "includes" / "ai" / "providers" / "OpenAIProvider.hpp")
            && fs::exists(candidate / "sourceFiles" / "providers" / "OpenAIProvider.cpp")) {
            return candidate;
        }

        const fs::path nested = candidate / "First_AI_Agents";
        if (fs::exists(nested / "CMakeLists.txt")
            && fs::exists(nested / "includes" / "ai" / "providers" / "OpenAIProvider.hpp")
            && fs::exists(nested / "sourceFiles" / "providers" / "OpenAIProvider.cpp")) {
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

std::tm localTime(std::chrono::system_clock::time_point now)
{
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(now);
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &rawTime);
#else
    localtime_r(&rawTime, &local);
#endif
    return local;
}

std::string timestampForDirectory(std::chrono::system_clock::time_point now)
{
    const std::tm local = localTime(now);
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return value.str();
}

std::string timestampForSummary(std::chrono::system_clock::time_point now)
{
    const std::tm local = localTime(now);
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return value.str();
}

bool isSecretCharacter(char value)
{
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) != 0 || value == '-' || value == '_';
}

std::string redactTokenAfterMarker(std::string text, std::string_view marker)
{
    std::size_t position = 0;
    while ((position = text.find(marker, position)) != std::string::npos) {
        std::size_t end = position + marker.size();
        while (end < text.size() && isSecretCharacter(text[end])) {
            ++end;
        }

        if (end == position + marker.size()) {
            position = end;
            continue;
        }

        text.replace(position, end - position, std::string {marker} + "[redacted]");
        position += marker.size() + 10;
    }

    return text;
}

std::string redactSensitive(std::string text)
{
    text = redactTokenAfterMarker(std::move(text), "sk-");
    text = redactTokenAfterMarker(std::move(text), "Bearer ");
    return text;
}

std::string readTextLimited(const fs::path& path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return "[missing file]\n";
    }

    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    content = redactSensitive(std::move(content));
    if (content.size() <= limit) {
        return content;
    }

    content.resize(limit);
    content += "\n[truncated]\n";
    return content;
}

std::string buildProjectContext(const fs::path& projectRoot)
{
    const std::array<ContextFile, 8> files {{
        {fs::path {"includes"} / "ai" / "providers" / "IModelProvider.hpp", 2500},
        {fs::path {"includes"} / "ai" / "providers" / "OpenAIProvider.hpp", 3000},
        {fs::path {"sourceFiles"} / "providers" / "OpenAIProvider.cpp", 4000},
        {fs::path {"includes"} / "ai" / "providers" / "OpenAIResponseParser.hpp", 2000},
        {fs::path {"sourceFiles"} / "providers" / "OpenAIResponseParser.cpp", 2500},
        {fs::path {"sourceFiles"} / "main.cpp", 3000},
        {fs::path {"README.md"}, 2500},
        {fs::path {"CODEX_CONTEXT.md"}, 2500},
    }};

    constexpr std::size_t totalLimit = 14000;
    std::ostringstream context;
    std::size_t used = 0;

    for (const auto& file : files) {
        std::ostringstream section;
        section << "## File: " << file.relativePath.generic_string() << "\n\n";
        section << readTextLimited(projectRoot / file.relativePath, file.limit) << "\n\n";

        std::string sectionText = section.str();
        if (used + sectionText.size() > totalLimit) {
            const std::size_t remaining = totalLimit > used ? totalLimit - used : 0;
            if (remaining > 0) {
                sectionText.resize(remaining);
                context << sectionText << "\n[project context truncated]\n";
            }
            break;
        }

        context << sectionText;
        used += sectionText.size();
    }

    return context.str();
}

std::string modelForStep(const SmokeStep& step)
{
    const std::string explicitModel = envOrEmpty(step.modelEnvName.c_str());
    if (!explicitModel.empty()) {
        return explicitModel;
    }

    return envOrEmpty("CLAUDE_MODEL");
}

std::string modelForLog(const std::string& model)
{
    return model.empty() ? std::string {"project default model"} : model;
}

bool writeTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }

    output << text;
    return output.good();
}

bool hasAnyDirectOutputFile(const fs::path& outputRoot)
{
    return fs::exists(outputRoot / "model_1_architecture_review.md")
        || fs::exists(outputRoot / "model_2_code_review.md")
        || fs::exists(outputRoot / "PROJECT_OVERVIEW.md")
        || fs::exists(outputRoot / "smoke_test_summary.txt");
}

fs::path chooseOutputDirectory(const fs::path& projectRoot, std::chrono::system_clock::time_point startedAt)
{
    const fs::path outputRoot = projectRoot / "ai_test_outputs";
    fs::create_directories(outputRoot);
    if (!hasAnyDirectOutputFile(outputRoot)) {
        return outputRoot;
    }

    fs::path timestamped = outputRoot / timestampForDirectory(startedAt);
    int suffix = 2;
    while (fs::exists(timestamped)) {
        timestamped = outputRoot / (timestampForDirectory(startedAt) + "_" + std::to_string(suffix));
        ++suffix;
    }

    fs::create_directories(timestamped);
    return timestamped;
}

std::string classifyError(const std::string& message)
{
    std::string lower;
    lower.reserve(message.size());
    for (const char value : message) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    }

    if (lower.find("api key") != std::string::npos || lower.find("openai_api_key") != std::string::npos) {
        return "api-key";
    }
    if (lower.find("transport") != std::string::npos || lower.find("network") != std::string::npos) {
        return "network";
    }
    if (lower.find("http") != std::string::npos) {
        return "http-or-api";
    }
    if (lower.find("json") != std::string::npos) {
        return "json";
    }
    if (lower.find("refusal") != std::string::npos || lower.find("incomplete") != std::string::npos) {
        return "api-response";
    }

    return "provider";
}

std::string failedOutput(const SmokeStep& step, const std::string& errorType, const std::string& errorMessage)
{
    std::ostringstream output;
    output << "# Smoke Step Failed\n\n";
    output << "- Step: " << step.number << "/3 " << step.title << "\n";
    output << "- Error type: " << errorType << "\n";
    output << "- Error: " << redactSensitive(errorMessage) << "\n";
    return output.str();
}

std::string architecturePrompt(const std::string& projectContext)
{
    std::ostringstream prompt;
    prompt << "You are analyzing a small C++20 project with multiple AI providers.\n\n";
    prompt << "Based only on the provided files:\n";
    prompt << "1. briefly describe the purpose of maxOutPutTokens Logic;\n";
    prompt << "2. and Where can I see it ;\n";
    //prompt << "3. list the main components;\n";
    //prompt << "4. name no more than three potential technical risks.\n\n";
    prompt << "Keep the answer under 300 words. Do not modify code. Do not include secrets.\n\n";
    prompt << "Project context:\n\n" << projectContext;

    return prompt.str();
}

std::string codeReviewPrompt(const std::string& projectContext, const std::optional<std::string>& architectureReview)
{
    std::ostringstream prompt;
    prompt << "Review the provided OpenAIProvider implementation briefly.\n\n";
    prompt << "Check API key loading, HTTP request construction, JSON parsing, HTTP/network error handling, ";
    prompt << "secret safety, and extensibility for other models.\n\n";
    prompt << "Choose only one useful small improvement. Do not rewrite the class and do not create a new architecture.\n";
    prompt << "The answer must include the issue or improvement, why it matters, and a small example change.\n";
    prompt << "Keep the answer under 350 words. Do not include secrets.\n\n";
    if (architectureReview.has_value()) {
        prompt << "Previous architecture review:\n\n" << *architectureReview << "\n\n";
    } else {
        prompt << "Previous architecture review is unavailable because step 1 failed.\n\n";
    }
    prompt << "Project context:\n\n" << projectContext;
    return prompt.str();
}

std::string documentationPrompt(
    const std::string& projectContext,
    const std::optional<std::string>& architectureReview,
    const std::optional<std::string>& codeReview)
{
    std::ostringstream prompt;
    prompt << "Create short technical documentation for this C++ project.\n\n";
    prompt << "The document is for a developer opening the repository for the first time.\n";
    prompt << "Add these Markdown sections exactly:\n\n";
    prompt << "# Project Overview\n\n";
    prompt << "## Purpose\n\n";
    prompt << "## Current Architecture\n\n";
    prompt << "## OpenAIProvider\n\n";
    prompt << "## Configuration\n\n";
    prompt << "## Running the Smoke Test\n\n";
    prompt << "## Current Limitations\n\n";
    prompt << "## Next Small Steps\n\n";
    prompt << "Do not invent missing components. Base the document only on the provided context. ";
    prompt << "Mark unknown details explicitly. Do not include API keys. Keep it under about 600 words.\n\n";
    prompt << "Architecture review:\n\n";
    prompt << (architectureReview.has_value() ? *architectureReview : "Unavailable because step 1 failed.");
    prompt << "\n\nCode review:\n\n";
    prompt << (codeReview.has_value() ? *codeReview : "Unavailable because step 2 failed.");
    prompt << "\n\nProject context:\n\n" << projectContext;
    return prompt.str();
}

SmokeStepResult runStep(
    ai::providers::AnthropicProvider& provider,
    const SmokeStep& step,
    const std::string& requestedModel,
    const std::string& prompt,
    const fs::path& outputDirectory)
{
    const fs::path outputPath = outputDirectory / step.outputFile;
    std::cout << '[' << step.number << "/3] " << step.title
              << " with " << modelForLog(requestedModel) << "...\n";

    ai::models::ModelRequest request;
    request.options.model = requestedModel;
    request.options.temperature = 0.2;
    request.options.maxOutputTokens = step.maxOutputTokens;
    request.messages.push_back({
        ai::models::MessageRole::System,
        "You are a concise engineering assistant. Never include API keys or secrets."
    });
    request.messages.push_back({ai::models::MessageRole::User, prompt});

    const auto started = std::chrono::steady_clock::now();
    const ai::models::ModelResponse response = provider.generate(request);
    const auto finished = std::chrono::steady_clock::now();

    SmokeStepResult result;
    result.step = step;
    result.requestedModel = requestedModel;
    result.usedModel = response.model;
    result.success = response.success;
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
    result.outputPath = outputPath;

    if (response.success) {
        result.responseBytes = response.text.size();
        if (!writeTextFile(outputPath, response.text)) {
            result.success = false;
            result.errorType = "file";
            result.errorMessage = "Failed to write output file: " + outputPath.string();
        }
    } else {
        result.errorType = classifyError(response.errorMessage);
        result.errorMessage = redactSensitive(response.errorMessage);
        const std::string failureText = failedOutput(step, result.errorType, result.errorMessage);
        result.responseBytes = failureText.size();
        if (!writeTextFile(outputPath, failureText)) {
            result.errorType = "file";
            result.errorMessage = "Failed to write output file: " + outputPath.string();
        }
    }

    if (result.success) {
        std::cout << '[' << step.number << "/3] Completed. Output: "
                  << outputPath.generic_string() << "\n\n";
    } else {
        std::cout << '[' << step.number << "/3] Failed (" << result.errorType << "). Output: "
                  << outputPath.generic_string() << "\n\n";
    }

    return result;
}

std::string summaryText(
    std::chrono::system_clock::time_point startedAt,
    const std::vector<SmokeStepResult>& results,
    bool overallSuccess)
{
    std::ostringstream summary;
    summary << "OpenAIProvider smoke test summary\n";
    summary << "Started at: " << timestampForSummary(startedAt) << "\n";
    summary << "Overall status: " << (overallSuccess ? "success" : "failed") << "\n\n";

    for (const auto& result : results) {
        summary << '[' << result.step.number << "/3] " << result.step.title << "\n";
        summary << "Requested model: " << modelForLog(result.requestedModel) << "\n";
        summary << "Used model: " << (result.usedModel.empty() ? "unknown" : result.usedModel) << "\n";
        summary << "Status: " << (result.success ? "success" : "failed") << "\n";
        if (!result.success) {
            summary << "Error type: " << result.errorType << "\n";
            summary << "Error: " << redactSensitive(result.errorMessage) << "\n";
        }
        summary << "Duration ms: " << result.duration.count() << "\n";
        summary << "Response bytes: " << result.responseBytes << "\n";
        summary << "Output path: " << result.outputPath.generic_string() << "\n\n";
    }

    summary << "No API key or Authorization header is recorded in this summary.\n";
    return summary.str();
}

} // namespace

int main()
{
    const auto startedAt = std::chrono::system_clock::now();

    try {
        const fs::path projectRoot = findProjectRoot();
        const fs::path outputDirectory = chooseOutputDirectory(projectRoot, startedAt);
        const std::string projectContext = buildProjectContext(projectRoot);

        if (envOrEmpty("OPENAI_API_KEY").empty()) {
            std::cout << "OPENAI_API_KEY is not set. The smoke test will verify error handling only.\n\n";
        }

        const std::array<SmokeStep, 2> steps {{
            {1, "Check maxOutPutTokens Logic", "OPENAI_TEST_MODEL_1", "model_1_logic_review.md", 700},
            {2, "Running OpenAIProvider code review", "OPENAI_TEST_MODEL_2", "model_2_code_review.md", 800},           
        }};
        //const std::array<SmokeStep, 3> steps {{
        //    {1, "Running architecture review", "OPENAI_TEST_MODEL_1", "model_1_architecture_review.md", 700},
        //    {2, "Running OpenAIProvider code review", "OPENAI_TEST_MODEL_2", "model_2_code_review.md", 800},
        //    {3, "Generating project documentation", "OPENAI_TEST_MODEL_3", "PROJECT_OVERVIEW.md", 5000},
        //}};

        ai::providers::ModelType modelType = ai::providers::ModelType::ModelTypeClaudeOpus;

        //ai::providers::OpenAIProvider provider;
        ai::providers::AnthropicProvider provider(modelType);
        std::vector<SmokeStepResult> results;
        std::optional<std::string> architectureReview;
        std::optional<std::string> codeReview;

        const std::string model1 = "claude-opus-4-8";
        auto result1 = runStep(provider, steps[0], model1, architecturePrompt(projectContext), outputDirectory);
        if (result1.success) {
            architectureReview = readTextLimited(result1.outputPath, 5000);
        }
        results.push_back(result1);

        const std::string model2 = modelForStep(steps[1]);
        auto result2 = runStep(provider, steps[1], model2, codeReviewPrompt(projectContext, architectureReview), outputDirectory);
        if (result2.success) {
            codeReview = readTextLimited(result2.outputPath, 5000);
        }
        results.push_back(result2);

        //const std::string model3 = modelForStep(steps[2]);
        //auto result3 = runStep(
        //    provider,
        //    steps[2],
        //    model3,
        //    documentationPrompt(projectContext, architectureReview, codeReview),
        //    outputDirectory);
        //results.push_back(result3);

        const bool overallSuccess = results[0].success && results[1].success; // results[2].success;
        const fs::path summaryPath = outputDirectory / "smoke_test_summary.txt";
        if (!writeTextFile(summaryPath, summaryText(startedAt, results, overallSuccess))) {
            std::cerr << "Failed to write smoke test summary: " << summaryPath.string() << '\n';
            return 1;
        }

        if (overallSuccess) {
            std::cout << "Smoke test completed successfully.\n";
            std::cout << "Summary: " << summaryPath.generic_string() << '\n';
            return 0;
        }

        std::cout << "Smoke test completed with failures.\n";
        std::cout << "Summary: " << summaryPath.generic_string() << '\n';
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Smoke test error: " << redactSensitive(ex.what()) << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Smoke test error: unknown exception\n";
        return 1;
    }
}
