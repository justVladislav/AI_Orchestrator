#pragma once

#include "ai/common/Error.hpp"

#include <optional>
#include <utility>

namespace ai::common {

template <typename T>
class Result {
public:
    static Result ok(T value)
    {
        Result result;
        result.value_ = std::move(value);
        return result;
    }

    static Result failure(Error error)
    {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    [[nodiscard]] bool success() const noexcept
    {
        return value_.has_value();
    }

    [[nodiscard]] const T& value() const
    {
        return value_.value();
    }

    // Non-const overload so a move-only T (e.g. std::unique_ptr) can be
    // extracted with std::move(result.value()) from a non-const Result
    // without a const_cast; the const overload above remains the one
    // ordinary callers bind to.
    [[nodiscard]] T& value()
    {
        return value_.value();
    }

    [[nodiscard]] const std::optional<Error>& error() const noexcept
    {
        return error_;
    }

private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

// Command-style application operations need success/failure without a payload.
// Keep the same Result API instead of using sentinel bool values at boundaries.
template <>
class Result<void> {
public:
    static Result ok()
    {
        Result result;
        result.success_ = true;
        return result;
    }

    static Result failure(Error error)
    {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    [[nodiscard]] bool success() const noexcept
    {
        return success_;
    }

    [[nodiscard]] const std::optional<Error>& error() const noexcept
    {
        return error_;
    }

private:
    bool success_ {false};
    std::optional<Error> error_;
};

} // namespace ai::common
