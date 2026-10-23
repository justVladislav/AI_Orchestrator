#pragma once

// Run draft group storage that the run draft repository and the follow-up plan confirm code (still in the
// aggregate) both use. The caller owns the database lock, and for the insert the transaction as well.
#include "ai/application/RunDraftTypes.hpp"
#include "ai/common/Result.hpp"

#include <sqlite3.h>

#include <optional>
#include <string>
#include <vector>

namespace ai::infrastructure::sqlite_detail {

common::Result<void> insertRunDraftGroupRows(
    sqlite3* handle, const application::RunDraftGroup& group, const std::vector<application::RunDraft>& drafts);
common::Result<std::optional<application::RunDraftGroup>> findRunDraftGroupWhere(
    sqlite3* handle, const char* whereClause, const std::string& value);
common::Result<std::vector<application::RunDraft>> readRunDraftGroupDrafts(
    sqlite3* handle, const application::RunDraftGroupId& id);

} // namespace ai::infrastructure::sqlite_detail
