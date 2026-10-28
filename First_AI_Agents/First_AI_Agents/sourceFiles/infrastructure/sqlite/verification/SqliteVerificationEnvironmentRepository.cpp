#include "ai/infrastructure/SqliteRepositories.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"
#include "../roster/SqliteRosterMapping.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"
#include "../planning/SqliteRunDraftGroupStorage.hpp"

#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/CacheDurationAccounting.hpp"
#include "ai/application/conversations/ChatProjectContext.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

// Native SQLite C API only. This backend must build and link without the Qt
// SDK (no Qt6::Sql/QSqlDatabase); see CODEX_CONTEXT.md for why an earlier
// Qt SQL based revision of this file was reverted.
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ai::infrastructure {

namespace {

using application::Artifact;
using application::ArtifactKind;
using application::ApplyHashEntry;
using application::ApprovalRecord;
using application::ChangeSetState;
using application::CacheCreationDurationUsageProblem;
using application::cacheCreationDurationUsageProblemCode;
using application::cacheCreationDurationUsageProblemMessage;
using application::checkCacheCreationDurationUsage;
using application::ChangeId;
using application::ChangeSetId;
using application::FileChangeOperation;
using application::LongContextPricing;
using application::LongContextPricingProblem;
using application::ProjectMemoryCompactionRecord;
using application::ProjectMemoryCompactionStatus;
using application::ProposalReviewRejection;
using application::ProposedChangeSet;
using application::ProposedFileChange;
using application::ValidationFinding;
using application::ChatConversation;
using application::ChatMemorySegment;
using application::ChatMessage;
using application::ChatProjectContextAttachment;
using application::PreparedChatTurn;
using application::ExecutionProfile;
using application::RunDraft;
using application::RunDraftId;
using application::RunLaunchMode;
using application::Model;
using application::ModelExecution;
using application::QualityEvaluation;
using application::Project;
using application::ProjectMemorySegment;
using application::ProjectTask;
using application::ProjectStatus;
using application::Provider;
using application::ProviderExecutionBudgetOverrides;
using application::ProviderExecutionBudgetPolicy;
using application::ProviderExecutionBudgetUsage;
using application::Run;
using application::RunStage;
using application::RunStatus;
using application::StageModelAssignment;
using application::StageStatus;
using application::TaskPriority;
using application::TaskStatus;

using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindNullableText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::bindOptionalTime;
using sqlite_detail::optionalTime;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;
using sqlite_detail::pathToUtf8;
using sqlite_detail::pathFromUtf8;
using sqlite_detail::providerBudgetOverridesJson;
using sqlite_detail::parseProviderBudgetOverrides;
using sqlite_detail::readAssignment;
using sqlite_detail::serializeStringVector;
using sqlite_detail::parseStringVector;
using sqlite_detail::readRunAssignment;
using sqlite_detail::runStatusFromString;
using sqlite_detail::insertRunDraftGroupRows;
using sqlite_detail::findRunDraftGroupWhere;
using sqlite_detail::readRunDraftGroupDrafts;



bool safeVerificationTarget(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

bool safeVerificationRelativePath(std::string_view value)
{
    if (value.empty()
        || value.size() > application::kVerificationEnvironmentMaxRelativePathBytes
        || value.front() == '/' || value.back() == '/'
        || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos
        || value.find('$') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(start,
            (end == std::string_view::npos ? value.size() : end) - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (std::any_of(part.begin(), part.end(), [](unsigned char ch) {
                return ch < 0x20 || ch == 0x7f || ch == '*' || ch == '?' || ch == '['
                    || ch == ']' || ch == '{' || ch == '}';
            })) {
            return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

nlohmann::json buildPresetToJson(const application::VerificationBuildPresetDescriptor& preset)
{
    return nlohmann::json {
        {"name", preset.name},
        {"displayName", preset.displayName},
        {"source", application::toString(preset.source)},
        {"declaredConfigurePreset", preset.declaredConfigurePreset},
        {"declaredTargets", preset.declaredTargets},
        {"allowedTargetsConfirmed", preset.allowedTargetsConfirmed},
        {"allowedTargets", preset.allowedTargets},
        {"allowedTargetsComplete", preset.allowedTargetsComplete}
    };
}

common::Result<application::VerificationBuildPresetDescriptor> buildPresetFromJson(
    const nlohmann::json& j)
{
    auto corrupt = [] {
        return common::Result<application::VerificationBuildPresetDescriptor>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt verification environment build preset"));
    };
    const bool hasAllowedTargetsConfirmed = j.contains("allowedTargetsConfirmed");
    const bool hasAllowedTargets = j.contains("allowedTargets");
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()
        || !j.contains("source") || !j["source"].is_string()
        || !j.contains("declaredTargets") || !j["declaredTargets"].is_array()
        || j["declaredTargets"].size() > application::kVerificationEnvironmentMaxTargetsPerPreset
        || hasAllowedTargetsConfirmed != hasAllowedTargets
        || (hasAllowedTargetsConfirmed && !j["allowedTargetsConfirmed"].is_boolean())
        || (j.contains("allowedTargetsComplete") && !j["allowedTargetsComplete"].is_boolean())
        || (hasAllowedTargets && (!j["allowedTargets"].is_array()
            || j["allowedTargets"].size() > application::kVerificationEnvironmentMaxTargetsPerPreset))) {
        return corrupt();
    }
    const auto source = application::verificationPresetFileSourceFromString(
        j["source"].get<std::string>());
    if (!source.has_value()) return corrupt();
    application::VerificationBuildPresetDescriptor preset;
    preset.name = j["name"].get<std::string>();
    if (j.contains("displayName") && j["displayName"].is_string())
        preset.displayName = j["displayName"].get<std::string>();
    preset.source = *source;
    if (j.contains("declaredConfigurePreset") && j["declaredConfigurePreset"].is_string())
        preset.declaredConfigurePreset = j["declaredConfigurePreset"].get<std::string>();
    for (const auto& target : j["declaredTargets"]) {
        if (!target.is_string()) return corrupt();
        preset.declaredTargets.push_back(target.get<std::string>());
    }
    // Rows written before the additive target-catalog fields existed remain
    // readable as an explicitly unconfirmed catalog. AG-18.2 must treat this
    // state as setup-required/blocked, never as an unrestricted allow-list.
    preset.allowedTargetsConfirmed = hasAllowedTargetsConfirmed
        && j["allowedTargetsConfirmed"].get<bool>();
    // AG-17.0: additive. A row written before the field existed carries no
    // completeness proof and reads as NOT complete (unknown), never complete.
    preset.allowedTargetsComplete = j.contains("allowedTargetsComplete")
        && j["allowedTargetsComplete"].get<bool>();
    if (preset.allowedTargetsComplete && !preset.allowedTargetsConfirmed) return corrupt();
    std::unordered_set<std::string> seenAllowedTargets;
    if (hasAllowedTargets) {
        for (const auto& target : j["allowedTargets"]) {
            if (!target.is_string() || !safeVerificationTarget(target.get<std::string>())) return corrupt();
            const auto value = target.get<std::string>();
            if (!seenAllowedTargets.insert(value).second) return corrupt();
            preset.allowedTargets.push_back(value);
        }
    }
    if (!preset.allowedTargetsConfirmed && !preset.allowedTargets.empty()) return corrupt();
    return common::Result<application::VerificationBuildPresetDescriptor>::ok(std::move(preset));
}

nlohmann::json buildPresetsToJson(
    const std::vector<application::VerificationBuildPresetDescriptor>& presets)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& preset : presets) array.push_back(buildPresetToJson(preset));
    return array;
}

common::Result<std::vector<application::VerificationBuildPresetDescriptor>>
buildPresetsFromJsonArray(const nlohmann::json& parsed, std::size_t maxCount)
{
    if (!parsed.is_array() || parsed.size() > maxCount) {
        return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt or out-of-bound verification environment build presets"));
    }
    std::vector<application::VerificationBuildPresetDescriptor> presets;
    presets.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto preset = buildPresetFromJson(item);
        if (!preset.success()) {
            return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
                *preset.error());
        }
        presets.push_back(std::move(preset.value()));
    }
    return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::ok(
        std::move(presets));
}

common::Result<std::vector<application::VerificationBuildPresetDescriptor>>
buildPresetsFromJson(const std::string& json, std::size_t maxCount)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded()) {
        return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt verification environment build presets column"));
    }
    return buildPresetsFromJsonArray(parsed, maxCount);
}

nlohmann::json profileToJson(const application::VerificationProfile& profile)
{
    return nlohmann::json {
        {"profileId", profile.profileId},
        {"configurePresetName", profile.configurePresetName},
        {"configureDisplayName", profile.configureDisplayName},
        {"configureSource", application::toString(profile.configureSource)},
        {"declaredBinaryDir", profile.declaredBinaryDir},
        {"resolvedBinaryDir", profile.resolvedBinaryDir},
        {"origin", application::toString(profile.origin)},
        {"buildPresetsConfirmed", profile.buildPresetsConfirmed},
        {"buildPresets", buildPresetsToJson(profile.buildPresets)}
    };
}

common::Result<application::VerificationProfile> profileFromJson(const nlohmann::json& j)
{
    auto corrupt = [] {
        return common::Result<application::VerificationProfile>::failure(
            error("verification_environment.corrupt_entry", "Corrupt verification environment profile"));
    };
    if (!j.is_object()
        || !j.contains("profileId") || !j["profileId"].is_string() || j["profileId"].get<std::string>().empty()
        || !j.contains("configurePresetName") || !j["configurePresetName"].is_string()
        || j["configurePresetName"].get<std::string>().empty()
        || !j.contains("configureSource") || !j["configureSource"].is_string()
        || !j.contains("origin") || !j["origin"].is_string()
        || !j.contains("buildPresetsConfirmed") || !j["buildPresetsConfirmed"].is_boolean()
        || !j.contains("buildPresets") || !j["buildPresets"].is_array()
        || j["buildPresets"].size() > application::kVerificationEnvironmentMaxBuildPresetsPerProfile) {
        return corrupt();
    }
    const auto source = application::verificationPresetFileSourceFromString(
        j["configureSource"].get<std::string>());
    const auto origin = application::verificationProfileOriginFromString(j["origin"].get<std::string>());
    if (!source.has_value() || !origin.has_value()) return corrupt();
    application::VerificationProfile profile;
    profile.profileId = j["profileId"].get<std::string>();
    profile.configurePresetName = j["configurePresetName"].get<std::string>();
    if (j.contains("configureDisplayName") && j["configureDisplayName"].is_string())
        profile.configureDisplayName = j["configureDisplayName"].get<std::string>();
    profile.configureSource = *source;
    if (j.contains("declaredBinaryDir") && j["declaredBinaryDir"].is_string())
        profile.declaredBinaryDir = j["declaredBinaryDir"].get<std::string>();
    if (j.contains("resolvedBinaryDir") && j["resolvedBinaryDir"].is_string())
        profile.resolvedBinaryDir = j["resolvedBinaryDir"].get<std::string>();
    if (!profile.resolvedBinaryDir.empty()
        && !safeVerificationRelativePath(profile.resolvedBinaryDir)) {
        return corrupt();
    }
    profile.origin = *origin;
    profile.buildPresetsConfirmed = j["buildPresetsConfirmed"].get<bool>();
    auto buildPresets = buildPresetsFromJsonArray(
        j["buildPresets"], application::kVerificationEnvironmentMaxBuildPresetsPerProfile);
    if (!buildPresets.success()) {
        return common::Result<application::VerificationProfile>::failure(*buildPresets.error());
    }
    profile.buildPresets = std::move(buildPresets.value());
    return common::Result<application::VerificationProfile>::ok(std::move(profile));
}

std::string profilesToJson(const std::vector<application::VerificationProfile>& profiles)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& profile : profiles) array.push_back(profileToJson(profile));
    return array.dump();
}

common::Result<std::vector<application::VerificationProfile>> profilesFromJson(const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()
        || parsed.size() > application::kVerificationEnvironmentMaxProfiles) {
        return common::Result<std::vector<application::VerificationProfile>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt or out-of-bound verification environment profiles column"));
    }
    std::vector<application::VerificationProfile> profiles;
    profiles.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto profile = profileFromJson(item);
        if (!profile.success()) {
            return common::Result<std::vector<application::VerificationProfile>>::failure(*profile.error());
        }
        profiles.push_back(std::move(profile.value()));
    }
    return common::Result<std::vector<application::VerificationProfile>>::ok(std::move(profiles));
}

} // namespace

SqliteVerificationEnvironmentRepository::SqliteVerificationEnvironmentRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteVerificationEnvironmentRepository::recordSnapshot(
    application::VerificationEnvironmentSnapshot snapshot)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_environment_snapshots "
        "(run_id,project_id,schema_version,baseline_fingerprint,state,"
        "profiles_json,unlinked_build_presets_json,diagnostic,discovered_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, snapshot.runId),
        bindText(handle, statement.get(), 2, snapshot.projectId),
        bindText(handle, statement.get(), 3, snapshot.schemaVersion),
        bindText(handle, statement.get(), 4, snapshot.baselineFingerprint),
        bindText(handle, statement.get(), 5, application::toString(snapshot.state)),
        bindText(handle, statement.get(), 6, profilesToJson(snapshot.profiles)),
        bindText(handle, statement.get(), 7, buildPresetsToJson(snapshot.unlinkedBuildPresets).dump()),
        bindText(handle, statement.get(), 8, snapshot.diagnostic),
        bindInt64(handle, statement.get(), 9, toMilliseconds(snapshot.discoveredAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "verification_environment.already_recorded",
                "A verification environment snapshot already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not record verification environment snapshot"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::VerificationEnvironmentSnapshot>>
SqliteVerificationEnvironmentRepository::findForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT project_id,schema_version,baseline_fingerprint,state,"
        "profiles_json,unlinked_build_presets_json,diagnostic,discovered_at_ms "
        "FROM verification_environment_snapshots WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::ok(
            std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            databaseError(handle, "verification_environment.read_failed",
                "Could not read verification environment snapshot"));
    }
    auto corrupt = [] {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            error("verification_environment.corrupt_entry", "Corrupt verification environment snapshot row"));
    };
    application::VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = columnText(statement.get(), 0);
    snapshot.schemaVersion = columnText(statement.get(), 1);
    // Fail closed on identity: a snapshot from an unrecognized schema
    // version, or with a blank required identity field, must never be
    // silently accepted as if it were today's contract.
    if (snapshot.schemaVersion != application::kVerificationEnvironmentSnapshotSchemaVersion
        || snapshot.projectId.empty()) {
        return corrupt();
    }
    snapshot.baselineFingerprint = columnText(statement.get(), 2);
    const auto state = application::verificationEnvironmentStateFromString(columnText(statement.get(), 3));
    if (!state.has_value()) return corrupt();
    snapshot.state = *state;
    auto profiles = profilesFromJson(columnText(statement.get(), 4));
    if (!profiles.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *profiles.error());
    }
    snapshot.profiles = std::move(profiles.value());
    auto unlinkedBuildPresets = buildPresetsFromJson(
        columnText(statement.get(), 5), application::kVerificationEnvironmentMaxUnlinkedBuildPresets);
    if (!unlinkedBuildPresets.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *unlinkedBuildPresets.error());
    }
    snapshot.unlinkedBuildPresets = std::move(unlinkedBuildPresets.value());
    snapshot.diagnostic = columnText(statement.get(), 6);
    snapshot.discoveredAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 7));
    // Fail closed on state/content consistency: a stored row claiming
    // ExistingProfile with no profiles, or a non-ExistingProfile state that
    // somehow carries profiles, is corrupt data, not a value this reader
    // silently trusts.
    const bool hasProfiles = !snapshot.profiles.empty();
    const bool claimsExistingProfile =
        snapshot.state == application::VerificationEnvironmentState::ExistingProfile;
    if (claimsExistingProfile != hasProfiles) return corrupt();
    return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::ok(
        std::move(snapshot));
}

} // namespace ai::infrastructure
