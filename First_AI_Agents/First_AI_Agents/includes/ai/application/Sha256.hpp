#pragma once

#include <string>
#include <string_view>

namespace ai::application {

// Self-contained SHA-256 (no external crypto dependency). Used only to
// produce a stable, verifiable content digest for stored artifacts; this is
// an integrity check, not a security boundary. Returns 64 lowercase hex
// characters.
[[nodiscard]] std::string sha256Hex(std::string_view data);

} // namespace ai::application
