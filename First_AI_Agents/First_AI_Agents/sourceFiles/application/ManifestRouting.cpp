#include "ai/application/ManifestRouting.hpp"

#include "ai/application/AgentToolTurnLoopRunner.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace ai::application {

namespace {

constexpr std::size_t kMaximumRoutingMembers = 64;
constexpr std::size_t kMaximumMemberIdBytes = kManifestRoutingMaxMemberIdBytes;
constexpr std::size_t kMaximumModelLabelBytes = 160;
constexpr std::size_t kMaximumRosterCatalogBytes = 128 * 1024;
constexpr std::size_t kMaximumIdentityBytes = 128;
constexpr std::size_t kMaximumRoleBytes = 160;

void appendField(std::ostringstream& out, std::string_view name, std::string_view value)
{
    out << name.size() << ':' << name << '=' << value.size() << ':' << value << '\n';
}

bool isLowerHexSha256(const std::string& value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

ExecutionManifestFinding finding(const char* code, std::string message, const std::string& taskId)
{
    return {code, std::move(message), taskId};
}

bool hasControlCharacter(std::string_view value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

// Prompt text must stay on its line: control characters (including newlines)
// become spaces.
std::string singleLine(std::string_view value)
{
    std::string result(value);
    for (auto& ch : result) {
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7f) ch = ' ';
    }
    return result;
}

} // namespace

std::string ManifestRouting::instructionsHash(std::string_view instructions)
{
    return sha256Hex(instructions);
}

std::string ManifestRouting::effectiveRole(const StageModelAssignment& assignment)
{
    return assignment.role.empty() ? assignment.stageType : assignment.role;
}

std::uint64_t ManifestRouting::effectiveTimeoutMs(const StageModelAssignment& assignment)
{
    return assignment.timeout.count() > 0
        ? static_cast<std::uint64_t>(assignment.timeout.count())
        : static_cast<std::uint64_t>(kAgentToolTurnLoopDefaultTurnTimeout.count());
}

ManifestRoutingRecord ManifestRouting::fromAssignments(
    const std::vector<StageModelAssignment>& assignments)
{
    ManifestRoutingRecord record;
    record.mode = assignments.size() == 1 ? ManifestRoutingMode::SingleAssignment
                                          : ManifestRoutingMode::Roster;
    record.members.reserve(assignments.size());
    for (const auto& assignment : assignments) {
        ManifestRoutingMember member;
        member.memberId = assignment.stageType;
        member.providerId = assignment.providerId;
        member.modelId = assignment.modelId;
        member.role = effectiveRole(assignment);
        member.leadArchitect = record.mode == ManifestRoutingMode::SingleAssignment
            ? true : assignment.leadArchitect;
        member.workspaceAccess = assignment.workspaceAccess;
        member.instructionsHash = instructionsHash(assignment.instructions);
        member.position = assignment.position;
        member.timeoutMs = effectiveTimeoutMs(assignment);
        record.members.push_back(std::move(member));
    }
    record.fingerprint = computeFingerprint(record);
    return record;
}

std::string ManifestRouting::computeFingerprint(const ManifestRoutingRecord& record)
{
    std::ostringstream material;
    appendField(material, "schema", record.schemaVersion);
    appendField(material, "mode", toString(record.mode));
    for (const auto& member : record.members) {
        appendField(material, "member-id", member.memberId);
        appendField(material, "provider-id", member.providerId);
        appendField(material, "model-id", member.modelId);
        appendField(material, "role", member.role);
        appendField(material, "lead", member.leadArchitect ? "1" : "0");
        appendField(material, "workspace", member.workspaceAccess ? "1" : "0");
        appendField(material, "instructions-sha256", member.instructionsHash);
        appendField(material, "position", std::to_string(member.position));
        appendField(material, "timeout-ms", std::to_string(member.timeoutMs));
    }
    return sha256Hex(material.str());
}

std::optional<std::string> ManifestRouting::recordProblem(const ManifestRoutingRecord& record)
{
    if (record.schemaVersion != kManifestRoutingSchemaVersion) {
        return std::string("The routing record schema version is not supported");
    }
    if (record.members.empty() || record.members.size() > kMaximumRoutingMembers) {
        return std::string("The routing record member count is outside the supported range");
    }
    const auto leads = static_cast<std::size_t>(std::count_if(record.members.begin(),
        record.members.end(), [](const ManifestRoutingMember& member) { return member.leadArchitect; }));
    if (record.mode == ManifestRoutingMode::SingleAssignment) {
        if (record.members.size() != 1 || leads != 1) {
            return std::string("A single-assignment routing record must hold exactly one member, "
                               "the lead Architect");
        }
    } else {
        if (record.members.size() < 2) {
            return std::string("A roster routing record needs a lead Architect and at least one worker");
        }
        if (leads != 1) {
            return std::string("A roster routing record must have exactly one lead Architect");
        }
    }
    std::set<std::string> ids;
    std::set<std::uint32_t> positions;
    for (const auto& member : record.members) {
        if (member.memberId.empty() || member.memberId.size() > kMaximumMemberIdBytes
            || member.providerId.empty() || member.providerId.size() > kMaximumIdentityBytes
            || member.modelId.empty() || member.modelId.size() > kMaximumIdentityBytes
            || member.role.empty() || member.role.size() > kMaximumRoleBytes) {
            return std::string("A routing member has an empty or unbounded identity field");
        }
        if (member.timeoutMs == 0) {
            return std::string("A routing member must have a positive timeout");
        }
        if (!ids.insert(member.memberId).second) {
            return std::string("Routing member ids must be unique");
        }
        if (!positions.insert(member.position).second) {
            return std::string("Routing member positions must be unique");
        }
        if (!isLowerHexSha256(member.instructionsHash)) {
            return std::string("A routing member's instruction hash is not a SHA-256 digest");
        }
    }
    if (!isLowerHexSha256(record.fingerprint) || record.fingerprint != computeFingerprint(record)) {
        return std::string("The routing record fingerprint does not match its members");
    }
    return std::nullopt;
}

const ManifestRoutingMember* ManifestRouting::findMember(
    const ManifestRoutingRecord& record, std::string_view memberId)
{
    for (const auto& member : record.members) {
        if (member.memberId == memberId) return &member;
    }
    return nullptr;
}

std::vector<std::string> ManifestRouting::assignableMemberIds(const ManifestRoutingRecord& record)
{
    std::vector<std::string> ids;
    if (record.mode != ManifestRoutingMode::Roster) return ids;
    for (const auto& member : record.members) {
        if (!member.leadArchitect && member.workspaceAccess) ids.push_back(member.memberId);
    }
    return ids;
}

std::optional<std::string> ManifestRouting::rosterCatalogProblem(
    const ManifestRoutingRecord& record,
    const std::vector<ManifestRosterMemberInfo>& catalog)
{
    if (record.mode == ManifestRoutingMode::SingleAssignment) {
        if (!catalog.empty()) {
            return std::string("A single-assignment Manifest has no roster catalog");
        }
        return std::nullopt;
    }
    if (catalog.size() != record.members.size()) {
        return std::string("The roster catalog must describe exactly the routing record's members");
    }
    std::set<std::string> seen;
    std::size_t totalBytes = 0;
    for (const auto& entry : catalog) {
        if (!seen.insert(entry.memberId).second) {
            return std::string("The roster catalog lists a member twice");
        }
        const auto* member = findMember(record, entry.memberId);
        if (member == nullptr) {
            return std::string("The roster catalog lists a member that is not in the routing record");
        }
        if (instructionsHash(entry.instructions) != member->instructionsHash) {
            return std::string("A roster catalog entry's instructions do not match the routing record");
        }
        if (entry.modelLabel.size() > kMaximumModelLabelBytes || hasControlCharacter(entry.modelLabel)) {
            return std::string("A roster catalog entry has an unbounded or multi-line model label");
        }
        totalBytes += entry.instructions.size() + entry.modelLabel.size() + entry.memberId.size();
    }
    // The rendered section also quotes memberId and role, which become prompt
    // text: refuse anything that could break out of its line or its quotes.
    for (const auto& member : record.members) {
        if (member.memberId.find_first_of("\"\\") != std::string::npos
            || hasControlCharacter(member.memberId)) {
            return std::string("A routing member id cannot be shown in the roster catalog");
        }
        totalBytes += member.role.size();
    }
    if (totalBytes > kMaximumRosterCatalogBytes) {
        return std::string("The roster catalog exceeds the bounded prompt size");
    }
    return std::nullopt;
}

std::string ManifestRouting::renderRosterCatalog(
    const ManifestRoutingRecord& record,
    const std::vector<ManifestRosterMemberInfo>& catalog)
{
    const auto assignable = assignableMemberIds(record);
    if (assignable.empty()) return {};

    std::ostringstream out;
    out << "# Trusted roster (multi-agent Run)\n"
           "This Run executes your task graph with the roster below. You are the lead "
           "Architect: you plan and never execute a task yourself. Every workTasks entry "
           "MUST include assigneeMemberId, set to exactly one memberId copied verbatim "
           "from the assignable members listed below - never an id you invent, never "
           "the lead, never a provider or model name. For this Run only, that makes a "
           "workTasks entry fourteen fields instead of thirteen: the fields listed "
           "earlier plus assigneeMemberId, none omitted. A coordinationTasks entry executes "
           "nothing and never has an assignee (it has no such field). Several tasks may "
           "share one member. Choose each task's assignee from the member's role and "
           "instructions, and keep the ownership of every path with exactly one task. Do "
           "not create filler, audit-only or report-only tasks merely to give a member "
           "something to do: a member without a task is simply not used in this Run. The "
           "Backend, not you, chooses each member's provider and model.\n\n"
           "Assignable members:\n";
    for (const auto& id : assignable) {
        const auto* member = findMember(record, id);
        const ManifestRosterMemberInfo* info = nullptr;
        for (const auto& entry : catalog) {
            if (entry.memberId == id) { info = &entry; break; }
        }
        out << "- memberId: \"" << id << "\"\n"
            << "  role: " << singleLine(member->role) << "\n";
        if (info != nullptr && !info->modelLabel.empty()) {
            out << "  model: " << info->modelLabel << "\n";
        }
        if (info == nullptr || info->instructions.empty()) {
            out << "  instructions: (none)\n";
        } else {
            out << "  instructions:\n";
            std::istringstream lines(info->instructions);
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                out << "    " << line << "\n";
            }
        }
    }
    return out.str();
}


const ManifestRoutingMember* ManifestRouting::assigneeOf(
    const ManifestRoutingRecord& record, const ExecutionManifestTask& task)
{
    if (task.requestedCapability == WorkspaceCapability::None) return nullptr;
    if (task.assigneeMemberId.has_value()) return findMember(record, *task.assigneeMemberId);
    if (record.mode == ManifestRoutingMode::SingleAssignment && record.members.size() == 1) {
        return &record.members.front();
    }
    return nullptr;
}

std::vector<ExecutionManifestFinding> ManifestRouting::findings(const ExecutionManifest& manifest)
{
    std::vector<ExecutionManifestFinding> result;
    if (!manifest.routing.has_value()) {
        for (const auto& task : manifest.tasks) {
            if (task.assigneeMemberId.has_value()) {
                result.push_back(finding("manifest.routing_assignee_unknown",
                    "Task '" + task.id + "' names an assignee but this Manifest carries no "
                    "routing record", task.id));
            }
        }
        return result;
    }
    const auto& record = *manifest.routing;
    if (const auto problem = recordProblem(record)) {
        // A defect of the Backend-owned record, not of any task: reported once.
        result.push_back({"manifest.routing_invalid", *problem, std::nullopt});
        return result;
    }
    for (const auto& task : manifest.tasks) {
        if (task.requestedCapability == WorkspaceCapability::None) {
            if (task.assigneeMemberId.has_value()) {
                result.push_back(finding("manifest.routing_assignee_forbidden",
                    "Coordination task '" + task.id + "' executes no provider work and must "
                    "not name an assignee", task.id));
            }
            continue;
        }
        const ManifestRoutingMember* assignee = nullptr;
        if (task.assigneeMemberId.has_value()) {
            assignee = findMember(record, *task.assigneeMemberId);
            if (assignee == nullptr) {
                result.push_back(finding("manifest.routing_assignee_unknown",
                    "Task '" + task.id + "' names an assignee that is not a member of the "
                    "routing record", task.id));
                continue;
            }
        }
        if (record.mode == ManifestRoutingMode::SingleAssignment) continue; // sole member, implicit tools
        if (assignee == nullptr) {
            result.push_back(finding("manifest.routing_assignee_missing",
                "Task '" + task.id + "' needs an explicit assignee in a roster Manifest", task.id));
        } else if (assignee->leadArchitect) {
            result.push_back(finding("manifest.routing_assignee_is_lead",
                "Task '" + task.id + "' is assigned to the lead Architect, who only plans in a "
                "roster Manifest", task.id));
        } else if (!assignee->workspaceAccess) {
            result.push_back(finding("manifest.routing_assignee_capability_mismatch",
                "Task '" + task.id + "' requests workspace capability '"
                    + toString(task.requestedCapability) + "' but its assignee has no workspace "
                    "access", task.id));
        }
    }
    return result;
}

} // namespace ai::application
