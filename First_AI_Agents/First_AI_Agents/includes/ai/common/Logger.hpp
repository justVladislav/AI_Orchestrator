#pragma once

#include <iostream>
#include <ostream>
#include <string_view>

namespace ai::common {

class Logger {
public:
    explicit Logger(std::ostream& output = std::cout) noexcept
        : output_(&output)
    {
    }

    void info(std::string_view message) const
    {
        write("info", message);
    }

    void warn(std::string_view message) const
    {
        write("warn", message);
    }

    void error(std::string_view message) const
    {
        write("error", message);
    }

private:
    void write(std::string_view level, std::string_view message) const
    {
        (*output_) << '[' << level << "] " << message << '\n';
    }

    std::ostream* output_;
};

} // namespace ai::common
