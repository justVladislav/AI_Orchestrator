#pragma once

#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/common/Result.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// MW-P3A package 1: shared, provider-input-parsing primitives reused by
// every strict JSON submission parser in the Manifest workflow
// (ExecutionManifestParser and the new ExecutionManifestDraftParser) - one
// implementation, one set of rejection rules and byte/item ceilings, never
// duplicated per parser. Mirrors ExecutionManifestValidator.cpp's own
// AG-15 "one implementation... never duplicated per list" discipline,
// applied to the parsing boundary instead of the validation boundary.
namespace ai::application::manifest_parsing {

using Json = nlohmann::json;

inline constexpr std::size_t kMaximumStringBytes = 64ULL * 1024;
inline constexpr std::size_t kMaximumItemsPerField = 256;
// AG-15: excluded/deferred path reasons are genuinely Planner-authored
// prose explaining a deliberate decision - bounded like every other
// human/model-authored reason field in this codebase.
inline constexpr std::size_t kMaximumScopePathReasonBytes = 2000;

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

// location is a Backend-built structural path composed only of literal field
// names from a parser's own schema and numeric array indices (e.g.
// "tasks[1].acceptanceContract.requirements[0]") - never a submitted value,
// an unknown/unexpected key, a raw path, or any other provider-controlled
// text. Empty location means "the top-level submission object".
[[nodiscard]] std::string fieldLocation(std::string_view location, const char* field);

[[nodiscard]] common::Result<void> requireExactFields(
    const Json& value,
    std::string_view location,
    std::initializer_list<const char*> required,
    std::initializer_list<const char*> optional = {});

[[nodiscard]] common::Result<std::string> readString(const Json& object, const char* field);
[[nodiscard]] common::Result<std::uint64_t> readUnsigned(const Json& object, const char* field);
[[nodiscard]] common::Result<std::uint32_t> readUint32(const Json& object, const char* field);
[[nodiscard]] common::Result<std::vector<std::string>> readStringArray(
    const Json& object, const char* field);

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& value);

// Shared raw-string safety validator for every workspace-relative path list
// a submission may declare. `code` lets each list report its own stable,
// diagnosable error code while sharing the exact same safety logic.
[[nodiscard]] common::Result<std::string> validateRawScopePath(
    const std::string& value, std::string_view code);

// Parses a bounded array of {"path","reason"} objects. Fails the whole parse
// on any malformed element, matching this codebase's established
// fail-whole-parse-on-malformed-element discipline.
[[nodiscard]] common::Result<std::vector<ManifestExcludedOrDeferredPath>> parsePathReasonArray(
    const Json& object,
    const char* field,
    std::string_view invalidPathCode,
    const std::string& location);

// Shared execution-acceptance.v1 requirement-array parser, reused by every
// submission shape that embeds an acceptanceContract object (the legacy
// execution-manifest.v1 task and the MW-P3A execution-manifest-draft.v1 work
// task). Parses exactly the pre-binding draft shape (verificationArguments
// legitimately empty for cmake_build/qml_lint at this point) - see
// ExecutionAcceptanceContract.hpp's own comments on binding order.
[[nodiscard]] common::Result<ExecutionAcceptanceContract> parseAcceptanceContract(
    const Json& value, const std::string& location);

} // namespace ai::application::manifest_parsing
