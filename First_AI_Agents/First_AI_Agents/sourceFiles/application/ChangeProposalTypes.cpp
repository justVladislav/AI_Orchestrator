#include "ai/application/ChangeProposalTypes.hpp"

#include <stdexcept>

namespace ai::application {

const char* toString(FileChangeOperation op) noexcept
{
    switch (op) {
    case FileChangeOperation::Create: return "create";
    case FileChangeOperation::Modify: return "modify";
    case FileChangeOperation::Delete: return "delete";
    case FileChangeOperation::Rename: return "rename";
    }
    return "unknown";
}

const char* toString(ChangeSetState state) noexcept
{
    switch (state) {
    case ChangeSetState::Received:       return "received";
    case ChangeSetState::Validated:      return "validated";
    case ChangeSetState::Rejected:        return "rejected";
    case ChangeSetState::ReadyForReview:  return "ready_for_review";
    case ChangeSetState::NoChangesNeeded: return "no_changes_needed";
    case ChangeSetState::Applied:         return "applied";
    case ChangeSetState::ApplyFailed:     return "apply_failed";
    }
    return "unknown";
}

FileChangeOperation fileChangeOperationFromString(std::string_view s)
{
    if (s == "create") return FileChangeOperation::Create;
    if (s == "modify") return FileChangeOperation::Modify;
    if (s == "delete") return FileChangeOperation::Delete;
    if (s == "rename") return FileChangeOperation::Rename;
    throw std::invalid_argument("Unknown FileChangeOperation: " + std::string(s));
}

ChangeSetState changeSetStateFromString(std::string_view s)
{
    if (s == "received")         return ChangeSetState::Received;
    if (s == "validated")        return ChangeSetState::Validated;
    if (s == "rejected")          return ChangeSetState::Rejected;
    if (s == "ready_for_review")  return ChangeSetState::ReadyForReview;
    if (s == "no_changes_needed") return ChangeSetState::NoChangesNeeded;
    if (s == "applied")           return ChangeSetState::Applied;
    if (s == "apply_failed")      return ChangeSetState::ApplyFailed;
    throw std::invalid_argument("Unknown ChangeSetState: " + std::string(s));
}

} // namespace ai::application
