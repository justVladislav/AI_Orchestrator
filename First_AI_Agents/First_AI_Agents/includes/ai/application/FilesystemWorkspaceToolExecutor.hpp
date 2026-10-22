#pragma once

// Read-only filesystem implementation of IWorkspaceToolExecutor.
//
// Implements all six tools registered by ToolRegistry::createDefaultReadOnly():
//   workspace_stat_path     — path metadata (exists, type, size, mtime)
//   workspace_hash_file     — SHA-256 of a bounded file
//   workspace_read_file     — UTF-8 text content, truncated at policy limit
//   workspace_list_directory — bounded, sorted, non-recursive directory listing
//   workspace_find_files    — recursive glob search, bounded by policy
//   workspace_search_text   — literal substring search, bounded by policy
//
// Security model (all enforced inside execute()):
//   1. Strict JSON argument parsing — unknown keys rejected, type errors rejected
//   2. WorkspacePathGuard validation — all six steps, called immediately before I/O
//   3. Binary rejection — NUL bytes detected in first 512 bytes
//   4. UTF-8 validation — rejected on workspace_read_file
//   5. Secret scanning — files whose content contains credential markers are
//      refused for read and silently skipped during search
//   6. Size limits — file content truncated; entry / match counts capped; total
//      result JSON checked against policy.maxResultBytes
//   7. No absolute paths — all result paths are relative to workspaceRoot
//   8. No exceptions — all std::exception descendants caught; internal errors
//      are surfaced as a failed AgentToolResult with code workspace.internal_error

#include "ai/application/IWorkspaceToolExecutor.hpp"

namespace ai::application {

class FilesystemWorkspaceToolExecutor final : public IWorkspaceToolExecutor {
public:
    [[nodiscard]] common::Result<AgentToolResult> execute(
        const AgentToolCall& call,
        const WorkspaceReadPolicy& policy) override;
};

} // namespace ai::application
