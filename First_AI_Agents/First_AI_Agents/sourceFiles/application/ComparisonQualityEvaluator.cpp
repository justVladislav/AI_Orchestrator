#include "ai/application/ComparisonQualityEvaluator.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>

namespace ai::application {
namespace {

std::unordered_set<std::string> terms(std::string_view text)
{
    std::unordered_set<std::string> result;
    std::string current;
    const auto flush = [&] {
        if (current.size() >= 4) result.insert(current);
        current.clear();
    };
    for (const unsigned char value : text) {
        if (std::isalnum(value) != 0) {
            current.push_back(static_cast<char>(std::tolower(value)));
        } else {
            flush();
        }
    }
    flush();
    return result;
}

std::uint32_t boundedScore(std::uint64_t numerator, std::uint64_t denominator)
{
    if (denominator == 0) return 0;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(10'000, numerator * 10'000 / denominator));
}

std::string evidence(std::string_view label, std::uint64_t first, std::uint64_t second)
{
    std::ostringstream out;
    out << label << ": " << first << '/' << second;
    return out.str();
}

} // namespace

CreateDeterministicQualityEvaluationRequest ComparisonQualityEvaluator::evaluate(
    const ModelExecution& execution,
    std::string_view prompt) const
{
    const std::string_view output = execution.outputContent.empty()
        ? std::string_view(execution.outputPreview)
        : std::string_view(execution.outputContent);

    const auto promptTerms = terms(prompt);
    const auto outputTerms = terms(output);
    std::uint64_t covered = 0;
    for (const auto& term : promptTerms) {
        if (outputTerms.contains(term)) ++covered;
    }
    const std::uint64_t relevantTerms = std::min<std::uint64_t>(promptTerms.size(), 16);
    const std::uint32_t relevance = promptTerms.empty()
        ? (output.empty() ? 0U : 7'500U)
        : boundedScore(std::min<std::uint64_t>(covered, relevantTerms), relevantTerms);

    const std::uint64_t targetLength = std::max<std::uint64_t>(
        400, std::min<std::uint64_t>(4'000, prompt.size() * 2));
    const std::uint32_t completeness = boundedScore(output.size(), targetLength);

    std::uint32_t structure = output.empty() ? 0U : 2'000U;
    if (output.find("\n\n") != std::string_view::npos) structure += 2'000;
    if (output.find("\n- ") != std::string_view::npos
        || output.find("\n* ") != std::string_view::npos
        || output.find("\n1.") != std::string_view::npos) structure += 2'500;
    if (output.find("\x60\x60\x60") != std::string_view::npos
        || output.find("\n#") != std::string_view::npos) structure += 2'000;
    const auto sentenceCount = static_cast<std::uint64_t>(
        std::count_if(output.begin(), output.end(), [](char value) {
            return value == '.' || value == '!' || value == '?';
        }));
    if (sentenceCount >= 3) structure += 1'500;
    structure = std::min<std::uint32_t>(10'000, structure);

    const auto nonSpace = static_cast<std::uint64_t>(
        std::count_if(output.begin(), output.end(), [](unsigned char value) {
            return std::isspace(value) == 0;
        }));
    std::uint32_t clarity = output.empty() ? 0U
        : boundedScore(nonSpace, std::max<std::uint64_t>(1, output.size()));
    // Text naturally contains spaces; map the useful 0.60-1.00 density band
    // onto a practical score while keeping the calculation deterministic.
    clarity = output.empty() ? 0U
        : std::min<std::uint32_t>(10'000, 4'000U + clarity * 6U / 10U);
    if (sentenceCount == 0 && output.size() > 240)
        clarity = clarity > 2'000 ? clarity - 2'000 : 0;

    CreateDeterministicQualityEvaluationRequest request;
    request.executionId = execution.id;
    request.evaluatorKind = QualityEvaluatorKind::Automated;
    request.rubricVersion = std::string(rubricVersion());
    request.rubric = {
        {"relevance", 3'500},
        {"completeness", 3'000},
        {"structure", 2'000},
        {"clarity", 1'500}};
    request.criteria = {
        {"relevance", relevance, evidence("prompt terms covered", covered, relevantTerms)},
        {"completeness", completeness, evidence("response bytes against target", output.size(), targetLength)},
        {"structure", structure, evidence("sentences detected", sentenceCount, 3)},
        {"clarity", clarity, evidence("non-space bytes", nonSpace, output.size())}};
    request.explanation =
        "Versioned deterministic baseline computed from persisted prompt and full response";
    return request;
}

} // namespace ai::application
