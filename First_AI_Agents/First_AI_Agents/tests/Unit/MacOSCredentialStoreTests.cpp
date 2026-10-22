#include "ai/infrastructure/MacOSCredentialStore.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
void requireSuccess(const ai::common::Result<T>& result, const char* message)
{
    if (!result.success()) {
        const auto detail = result.error().has_value()
            ? result.error()->code + ": " + result.error()->message
            : std::string("missing typed error");
        throw std::runtime_error(std::string(message) + " (" + detail + ")");
    }
}

} // namespace

int main()
{
    using ai::infrastructure::MacOSCredentialStore;

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string reference = "macos-keychain-test-" + std::to_string(nonce);
    constexpr const char* firstSecret = "first-secret-value";
    constexpr const char* replacementSecret = "replacement-secret-value";

    MacOSCredentialStore store;
    // Make cleanup idempotent even if an earlier interrupted run happened to
    // reuse this reference.
    const auto initialCleanup = store.remove(reference);
    requireSuccess(initialCleanup, "Initial Keychain cleanup failed");

    const auto missing = store.load(reference);
    require(missing.success() && !missing.value().has_value(),
        "Missing Keychain item did not load as nullopt");

    const auto inserted = store.store(reference, firstSecret);
    requireSuccess(inserted, "Keychain insert failed");
    const auto loaded = store.load(reference);
    require(loaded.success() && loaded.value().value_or("") == firstSecret,
        "Keychain round-trip returned the wrong secret");

    const auto replaced = store.store(reference, replacementSecret);
    requireSuccess(replaced, "Keychain update failed");
    const auto updated = store.load(reference);
    require(updated.success() && updated.value().value_or("") == replacementSecret,
        "Keychain update did not replace the secret");

    const auto deleted = store.remove(reference);
    requireSuccess(deleted, "Keychain removal failed");
    const auto deletedAgain = store.remove(reference);
    requireSuccess(deletedAgain, "Keychain removal is not idempotent");
    const auto removed = store.load(reference);
    require(removed.success() && !removed.value().has_value(),
        "Removed Keychain item is still readable");

    const auto invalid = store.store("", "must-not-leak");
    require(!invalid.success() && invalid.error()->code == "credential.invalid_reference",
        "Empty reference did not return credential.invalid_reference");
    require(invalid.error()->message.find("must-not-leak") == std::string::npos,
        "Credential error leaked secret bytes");

    std::cout << "MacOSCredentialStore tests passed\n";
    return 0;
}
