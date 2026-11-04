#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ai::application {

// Text that comes from outside the application (the output of a build tool, an exception's what()) is
// raw bytes, not UTF-8: cl.exe and link.exe write in the console's OEM code page (CP866 on a Russian
// Windows), and a byte limit can cut a multi-byte character in half. Anything stored in the database,
// shown in the UI or serialised to JSON must be well-formed UTF-8, so such text passes through here first.
//
// This is for DIAGNOSTIC TEXT only. Identifiers, hashes and paths that take part in an identity are
// never "repaired": a malformed one is an error, not something to replace silently.

// True when every byte belongs to a well-formed UTF-8 sequence: no overlong forms, no surrogates and
// nothing above U+10FFFF.
[[nodiscard]] bool isWellFormedUtf8(std::string_view text) noexcept;

// Returns well-formed UTF-8 for `raw`, line by line (every '\n' is kept):
//   - a well-formed line is kept byte for byte;
//   - on Windows another line is decoded from the system OEM code page;
//   - whatever is still malformed has each offending byte replaced by U+FFFD.
// NUL and other control bytes are left to the caller's own sanitising. Never throws for bad input.
[[nodiscard]] std::string toValidUtf8Diagnostic(std::string_view raw);

// The longest length <= `maxBytes` at which `text` can be cut without splitting a UTF-8 character
// (`text` should be well-formed). Returns text.size() when it already fits.
[[nodiscard]] std::size_t utf8SafePrefixLength(std::string_view text, std::size_t maxBytes) noexcept;

} // namespace ai::application
