#pragma once

// Persistence-private. The ONE canonical text of a list of source segment ids, shared by every writer and reader of compaction
// provenance (the final record and the intent). The final record's text is what the intent's guard compares byte for byte, so there is
// exactly one serializer; a second copy would silently turn into a second format.

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <string>
#include <vector>

namespace ai::infrastructure::sqlite_detail {

// A compact JSON array of strings: order and duplicates preserved, no whitespace. nlohmann::json::dump() throws on a string that is
// not valid UTF-8; the caller of a write must catch that and report persistence.write_failed.
[[nodiscard]] std::string serializeSegmentIdList(const std::vector<application::ProjectMemorySegmentId>& ids);

// Exactly a JSON array of strings, otherwise persistence.read_failed (a damaged row is never repaired). Whitespace and escape
// variants of a valid array are accepted here, as the record lookup always did.
[[nodiscard]] common::Result<std::vector<application::ProjectMemorySegmentId>> deserializeSegmentIdListStrict(const std::string& json);

// The stricter reading of an intent: the text must be a JSON array of strings AND exactly the text serializeSegmentIdList produces
// for it. A variant (whitespace, other escapes) is not ours and is refused, so the storage guard (byte comparison) and the
// application comparator (parsed vectors) can never disagree about a stored intent.
[[nodiscard]] common::Result<std::vector<application::ProjectMemorySegmentId>> deserializeCanonicalSegmentIdList(const std::string& json);

} // namespace ai::infrastructure::sqlite_detail
