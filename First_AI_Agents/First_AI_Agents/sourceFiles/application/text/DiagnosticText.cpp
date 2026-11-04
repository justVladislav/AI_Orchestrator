#include "ai/application/text/DiagnosticText.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <limits>

namespace ai::application {

namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

bool isContinuation(unsigned char byte) noexcept
{
    return (byte & 0xC0) == 0x80;
}

// Length of the well-formed sequence that starts at `at`, or 0 when it is malformed or truncated.
std::size_t sequenceLength(std::string_view text, std::size_t at) noexcept
{
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80) return 1;
    const auto byteAt = [&](std::size_t offset) { return static_cast<unsigned char>(text[at + offset]); };
    const std::size_t remaining = text.size() - at;
    if (lead >= 0xC2 && lead <= 0xDF) {
        return remaining >= 2 && isContinuation(byteAt(1)) ? 2 : 0;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (remaining < 3 || !isContinuation(byteAt(1)) || !isContinuation(byteAt(2))) return 0;
        if (lead == 0xE0 && byteAt(1) < 0xA0) return 0; // overlong
        if (lead == 0xED && byteAt(1) > 0x9F) return 0; // surrogate
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (remaining < 4 || !isContinuation(byteAt(1)) || !isContinuation(byteAt(2))
            || !isContinuation(byteAt(3))) {
            return 0;
        }
        if (lead == 0xF0 && byteAt(1) < 0x90) return 0; // overlong
        if (lead == 0xF4 && byteAt(1) > 0x8F) return 0; // above U+10FFFF
        return 4;
    }
    return 0;
}

std::string replaceMalformedBytes(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t length = sequenceLength(text, at);
        if (length == 0) {
            result += kReplacementCharacter;
            ++at;
        } else {
            result.append(text.substr(at, length));
            at += length;
        }
    }
    return result;
}

#ifdef _WIN32
// The system OEM code page maps every byte to a character, so a successful conversion is always possible
// for a non-empty line; an empty result tells the caller to fall back to replacement.
std::string decodeOemLine(std::string_view line)
{
    if (line.empty() || line.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
    const int wideCount = MultiByteToWideChar(
        CP_OEMCP, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
    if (wideCount <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
    if (MultiByteToWideChar(CP_OEMCP, 0, line.data(), static_cast<int>(line.size()),
            wide.data(), wideCount) <= 0) {
        return {};
    }
    const int utf8Count = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), wideCount, nullptr, 0, nullptr, nullptr);
    if (utf8Count <= 0) return {};
    std::string converted(static_cast<std::size_t>(utf8Count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount, converted.data(), utf8Count,
            nullptr, nullptr) <= 0) {
        return {};
    }
    return converted;
}
#endif

std::string toValidUtf8Line(std::string_view line)
{
    if (isWellFormedUtf8(line)) return std::string(line);
#ifdef _WIN32
    const std::string decoded = decodeOemLine(line);
    if (!decoded.empty() && isWellFormedUtf8(decoded)) return decoded;
#endif
    return replaceMalformedBytes(line);
}

} // namespace

bool isWellFormedUtf8(std::string_view text) noexcept
{
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t length = sequenceLength(text, at);
        if (length == 0) return false;
        at += length;
    }
    return true;
}

std::string toValidUtf8Diagnostic(std::string_view raw)
{
    if (isWellFormedUtf8(raw)) return std::string(raw);
    std::string result;
    result.reserve(raw.size());
    std::size_t start = 0;
    while (start <= raw.size()) {
        const std::size_t newline = raw.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? raw.size() : newline;
        result += toValidUtf8Line(raw.substr(start, end - start));
        if (newline == std::string_view::npos) break;
        result.push_back('\n');
        start = newline + 1;
    }
    return result;
}

std::size_t utf8SafePrefixLength(std::string_view text, std::size_t maxBytes) noexcept
{
    if (text.size() <= maxBytes) return text.size();
    std::size_t cut = maxBytes;
    // Step back over continuation bytes to the lead byte of the character the cut would split.
    while (cut > 0 && isContinuation(static_cast<unsigned char>(text[cut]))) --cut;
    return cut;
}

} // namespace ai::application
