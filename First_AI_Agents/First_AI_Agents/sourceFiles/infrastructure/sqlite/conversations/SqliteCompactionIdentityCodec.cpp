#include "SqliteCompactionIdentityCodec.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace ai::infrastructure::sqlite_detail {

std::string serializeSegmentIdList(const std::vector<application::ProjectMemorySegmentId>& ids)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& id : ids) array.push_back(id);
    return array.dump();
}

common::Result<std::vector<application::ProjectMemorySegmentId>> deserializeSegmentIdListStrict(const std::string& json)
{
    using ListResult = common::Result<std::vector<application::ProjectMemorySegmentId>>;
    const auto damaged = [] {
        return ListResult::failure({"persistence.read_failed",
            "The stored source segment ids of a project memory compaction record are not a JSON array of strings"});
    };
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return damaged();
    std::vector<application::ProjectMemorySegmentId> result;
    result.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string()) return damaged();
        result.push_back(item.get<std::string>());
    }
    return ListResult::ok(std::move(result));
}

common::Result<std::vector<application::ProjectMemorySegmentId>> deserializeCanonicalSegmentIdList(const std::string& json)
{
    using ListResult = common::Result<std::vector<application::ProjectMemorySegmentId>>;
    auto decoded = deserializeSegmentIdListStrict(json);
    if (!decoded.success()) return decoded;
    std::string canonical;
    try {
        canonical = serializeSegmentIdList(decoded.value());
    } catch (const std::exception&) {
        return ListResult::failure({"persistence.read_failed",
            "The stored source segment ids of a compaction intent cannot be put into their canonical form"});
    }
    if (canonical != json) {
        return ListResult::failure({"persistence.read_failed",
            "The stored source segment ids of a compaction intent are not in the canonical JSON form"});
    }
    return decoded;
}

} // namespace ai::infrastructure::sqlite_detail
