#include "ai/application/IRunWarningRepository.hpp"

namespace ai::application {

std::string boundedUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) return std::string(text);
    std::size_t end = maxBytes;
    // Step back over continuation bytes (10xxxxxx) so the cut falls on a character boundary.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return std::string(text.substr(0, end));
}

} // namespace ai::application
