#pragma once

#include "ai/common/Result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// PC-2 (gap MW-P9): the Run author's authoritative, structured part of a Run specification. A worker never
// sees the Run text - only the Planner does - so a data shape shared by a producer task and a consumer task
// (exact property, signal and field names) used to reach a worker only if the Planner happened to copy it into
// THAT task. A RunInterfaceContract is the Backend-owned alternative: it is validated once, frozen with the
// Run, fingerprinted, and delivered VERBATIM by the Backend to every provider-backed task it applies to. The
// Planner is never the carrier, so it cannot paraphrase, shorten or drop it.
//
// This header is the pure part (PC-2A): value types, strict normalization/validation, the canonical
// fingerprint, the deterministic task selection and the verbatim rendering. It reads no repository, no file
// and no clock, calls no provider and writes nothing; storage, the Planner prompt and the worker prompt are
// PC-2B/2C.
inline constexpr std::string_view kRunInterfaceContractsSchemaVersion = "run-interface-contracts.v1";

// Bounds. A contract is meant to be a short, exact interface description (names, shapes, signals), not a
// second Run text; the totals keep the cost of delivering every contract to every worker predictable.
inline constexpr std::size_t kRunInterfaceContractMaxCount = 16;
inline constexpr std::size_t kRunInterfaceContractMaxIdLength = 32;
inline constexpr std::size_t kRunInterfaceContractMaxTitleBytes = 120;
inline constexpr std::size_t kRunInterfaceContractMaxTextBytes = 4096;
inline constexpr std::size_t kRunInterfaceContractMaxTotalTextBytes = 16384;
inline constexpr std::size_t kRunInterfaceContractMaxScopesPerContract = 8;
inline constexpr std::size_t kRunInterfaceContractMaxScopeBytes = 240;

enum class RunContractKind {
    InterfaceContract,  // exact names/shapes shared between tasks: properties, signals, fields, file formats
    Constraint,         // a rule every affected worker must keep: limits, forbidden changes, conventions
};

[[nodiscard]] const char* toString(RunContractKind kind) noexcept;
[[nodiscard]] bool runContractKindFromString(std::string_view value, RunContractKind& out) noexcept;

struct RunInterfaceContract {
    // Stable author-chosen identity, `[A-Za-z][A-Za-z0-9_.-]{0,31}`, unique case-insensitively in one Run.
    std::string id;
    RunContractKind kind {RunContractKind::InterfaceContract};
    // One short line, shown in the UI and before the text in a prompt.
    std::string title;
    // The authoritative body, delivered byte for byte (only the line endings are normalized to LF).
    std::string text;
    // Relative path prefixes the contract applies to: a task is affected when any path it owns, will create,
    // must read or references equals a scope or lies below it. Empty = every provider-backed task. No
    // wildcards, absolute paths or ".."; no ',' or ';' and no leading space either, because the New Run editor
    // shows the scopes as one "a, b" text and a scope must come back from it unchanged (see
    // normalizeRunInterfaceContracts).
    std::vector<std::string> appliesToPaths;
};

// Validates `contracts` and returns them in the canonical stored form: authoring order kept, line endings
// of `text` normalized to LF, `title` trimmed, scopes normalized (forward slashes, no leading "./", no
// trailing "/", case-insensitive duplicates dropped). Nothing is repaired silently otherwise: any violation
// fails the whole list with a `run_contracts.*` code and a message that names the offending contract id
// (`invalid_id`, `invalid_kind` for an enum value no enumerator names, `invalid_title`, `invalid_text`,
// `invalid_scope`, ...).
[[nodiscard]] common::Result<std::vector<RunInterfaceContract>> normalizeRunInterfaceContracts(
    std::vector<RunInterfaceContract> contracts);

// Storage format shared by every repository (SQLite column `run_interface_contracts_json`, the in-memory
// repositories, tests): a JSON array of {id, kind, title, text, appliesToPaths}; an empty list is "[]".
// The parser is as strict as the normalizer: wrong shape, unknown or missing keys, a wrong type, an unknown
// kind, a list the normalizer would not leave unchanged, or invalid contracts all fail with a
// `persistence.invalid_data`-style code instead of being repaired, so a damaged row is reported, not trusted.
[[nodiscard]] std::string serializeRunInterfaceContracts(const std::vector<RunInterfaceContract>& contracts);
[[nodiscard]] common::Result<std::vector<RunInterfaceContract>> parseRunInterfaceContracts(std::string_view stored);

// sha256 of the canonical serialization (schema version + ordered contracts). Requires a list that already
// went through normalizeRunInterfaceContracts. An EMPTY list has an EMPTY fingerprint, so a Run without
// contracts keeps exactly the planning identity it had before PC-2.
[[nodiscard]] std::string runInterfaceContractsFingerprint(const std::vector<RunInterfaceContract>& contracts);

// The scope rule of selectRunContractsForTaskPaths for ONE scope and ONE path: true when `path` equals `scope` or
// lies below it. Separators and a leading "./" are normalized and ASCII case is ignored. It is the same two helpers,
// not a copy, so a consumer that needs the PC-2 prefix semantics (an advisory over a catalog) cannot drift from it.
[[nodiscard]] bool runContractScopeCoversPath(std::string_view scope, std::string_view path);

// The contracts that apply to a task with these relative paths, in authoring order. `taskPaths` is the union
// of everything the task owns, will create, must read or references; the caller (not this module) knows the
// Manifest task shape. A task with no paths still receives every contract that has no scope.
[[nodiscard]] std::vector<const RunInterfaceContract*> selectRunContractsForTaskPaths(
    const std::vector<RunInterfaceContract>& contracts, const std::vector<std::string>& taskPaths);

// Backend-rendered prompt text, empty when there is nothing to render. The worker text is authoritative and
// verbatim; the Planner text carries the same bodies plus the scope, so the Planner sees what the workers
// will be given without being asked to copy it.
[[nodiscard]] std::string renderRunContractsForWorker(
    const std::vector<const RunInterfaceContract*>& selected);
[[nodiscard]] std::string renderRunContractsForPlanner(const std::vector<RunInterfaceContract>& contracts);

} // namespace ai::application
