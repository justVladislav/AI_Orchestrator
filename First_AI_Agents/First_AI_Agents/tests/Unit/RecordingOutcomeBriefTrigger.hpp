#pragma once

// PC-4C: a recording IOutcomeBriefTrigger for the tests of the producers (RunManager, review / apply, continuation, manual handoff).

#include "ai/application/IOutcomeBriefTrigger.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace outcomebrief_hooks {

class RecordingTrigger final : public ai::application::IOutcomeBriefTrigger {
public:
    void runMayHaveChanged(const std::string& runId) noexcept override
    {
        std::lock_guard lock(mutex_);
        runs_.push_back(runId);
        if (onRun) onRun(runId);
    }
    void proposalResolved(const std::string& changeSetId) noexcept override
    {
        std::lock_guard lock(mutex_);
        proposals_.push_back(changeSetId);
    }

    [[nodiscard]] std::vector<std::string> runs() const
    {
        std::lock_guard lock(mutex_);
        return runs_;
    }
    [[nodiscard]] std::vector<std::string> proposals() const
    {
        std::lock_guard lock(mutex_);
        return proposals_;
    }
    [[nodiscard]] std::size_t runCalls(const std::string& runId) const
    {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count(runs_.begin(), runs_.end(), runId));
    }
    [[nodiscard]] std::size_t proposalCalls(const std::string& changeSetId) const
    {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count(proposals_.begin(), proposals_.end(), changeSetId));
    }
    void clear()
    {
        std::lock_guard lock(mutex_);
        runs_.clear();
        proposals_.clear();
    }

    // Polls until `predicate` holds (a nudge from a worker thread arrives slightly after the status it follows).
    bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return predicate();
    }

    // Called (under the recorder's lock, so keep it tiny) with the Run id of every runMayHaveChanged call.
    std::function<void(const std::string&)> onRun;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> runs_;
    std::vector<std::string> proposals_;
};

} // namespace outcomebrief_hooks
