#include "ai/application/IdGenerator.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>

namespace ai::application {

std::string generateUuid()
{
    // One engine per calling thread avoids a shared generator lock. UUID random
    // bits are independent from the run worker and never encode local paths/time.
    thread_local std::mt19937_64 engine {std::random_device {}()};
    std::array<std::uint8_t, 16> bytes {};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(engine() & 0xffU);
    }

    // RFC 4122 version and variant marker bits.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

std::string generateShortToken()
{
    // Same per-thread-engine reasoning as generateUuid() above - 8 random
    // bytes (64 bits of entropy) rendered as 16 lowercase hex characters,
    // no RFC 4122 version/variant bits (this is deliberately not a UUID,
    // just a short unpredictable path-safe token).
    thread_local std::mt19937_64 engine {std::random_device {}()};
    std::array<std::uint8_t, 8> bytes {};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(engine() & 0xffU);
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

} // namespace ai::application
