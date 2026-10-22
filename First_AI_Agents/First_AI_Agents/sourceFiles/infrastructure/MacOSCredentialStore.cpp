#include "ai/infrastructure/MacOSCredentialStore.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {

namespace {

constexpr const char* kServiceName = "com.aiorchestrator.credentials";

common::Error error(std::string code, const char* operation, OSStatus status)
{
    return {
        std::move(code),
        std::string("macOS Keychain ") + operation + " failed (status "
            + std::to_string(static_cast<long long>(status)) + ")"};
}

CFStringRef makeString(const std::string& value)
{
    return CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.data()),
        static_cast<CFIndex>(value.size()),
        kCFStringEncodingUTF8,
        false);
}

CFMutableDictionaryRef makeQuery(const std::string& reference)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (query == nullptr) {
        return nullptr;
    }

    CFStringRef service = makeString(kServiceName);
    CFStringRef account = makeString(reference);
    if (service == nullptr || account == nullptr) {
        if (service != nullptr) {
            CFRelease(service);
        }
        if (account != nullptr) {
            CFRelease(account);
        }
        CFRelease(query);
        return nullptr;
    }

    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return query;
}

common::Result<void> invalidReference()
{
    return common::Result<void>::failure(
        {"credential.invalid_reference", "Credential reference cannot be empty"});
}

} // namespace

common::Result<void> MacOSCredentialStore::store(
    const std::string& reference,
    const std::string& secret)
{
    if (reference.empty()) {
        return invalidReference();
    }

    CFMutableDictionaryRef query = makeQuery(reference);
    if (query == nullptr) {
        return common::Result<void>::failure(
            {"credential.store_failed", "macOS Keychain request allocation failed"});
    }

    std::vector<UInt8> secretBuffer(secret.begin(), secret.end());
    CFDataRef secretData = CFDataCreate(
        kCFAllocatorDefault,
        secretBuffer.data(),
        static_cast<CFIndex>(secretBuffer.size()));
    if (secretData == nullptr) {
        std::fill(secretBuffer.begin(), secretBuffer.end(), UInt8 {0});
        CFRelease(query);
        return common::Result<void>::failure(
            {"credential.store_failed", "macOS Keychain secret allocation failed"});
    }

    CFMutableDictionaryRef attributes = CFDictionaryCreateMutableCopy(
        kCFAllocatorDefault, 0, query);
    if (attributes == nullptr) {
        std::fill(secretBuffer.begin(), secretBuffer.end(), UInt8 {0});
        CFRelease(secretData);
        CFRelease(query);
        return common::Result<void>::failure(
            {"credential.store_failed", "macOS Keychain request allocation failed"});
    }
    CFDictionarySetValue(attributes, kSecValueData, secretData);
    CFDictionarySetValue(attributes, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);

    OSStatus status = SecItemAdd(attributes, nullptr);
    if (status == errSecDuplicateItem) {
        CFMutableDictionaryRef update = CFDictionaryCreateMutable(
            kCFAllocatorDefault,
            0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        if (update != nullptr) {
            CFDictionarySetValue(update, kSecValueData, secretData);
            CFDictionarySetValue(update, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
            status = SecItemUpdate(query, update);
            CFRelease(update);
        } else {
            status = errSecAllocate;
        }
    }

    std::fill(secretBuffer.begin(), secretBuffer.end(), UInt8 {0});
    CFRelease(attributes);
    CFRelease(secretData);
    CFRelease(query);

    if (status != errSecSuccess) {
        return common::Result<void>::failure(
            error("credential.store_failed", "write", status));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<std::string>> MacOSCredentialStore::load(
    const std::string& reference) const
{
    if (reference.empty()) {
        return common::Result<std::optional<std::string>>::failure(
            {"credential.invalid_reference", "Credential reference cannot be empty"});
    }

    CFMutableDictionaryRef query = makeQuery(reference);
    if (query == nullptr) {
        return common::Result<std::optional<std::string>>::failure(
            {"credential.load_failed", "macOS Keychain request allocation failed"});
    }
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status == errSecItemNotFound) {
        return common::Result<std::optional<std::string>>::ok(std::nullopt);
    }
    if (status != errSecSuccess || result == nullptr
        || CFGetTypeID(result) != CFDataGetTypeID()) {
        if (result != nullptr) {
            CFRelease(result);
        }
        return common::Result<std::optional<std::string>>::failure(
            error("credential.load_failed", "read", status));
    }

    auto* data = static_cast<CFDataRef>(result);
    const auto* bytes = CFDataGetBytePtr(data);
    const auto size = static_cast<std::size_t>(CFDataGetLength(data));
    std::string secret(reinterpret_cast<const char*>(bytes), size);
    CFRelease(result);
    return common::Result<std::optional<std::string>>::ok(std::move(secret));
}

common::Result<void> MacOSCredentialStore::remove(const std::string& reference)
{
    if (reference.empty()) {
        return invalidReference();
    }

    CFMutableDictionaryRef query = makeQuery(reference);
    if (query == nullptr) {
        return common::Result<void>::failure(
            {"credential.remove_failed", "macOS Keychain request allocation failed"});
    }
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    if (status == errSecSuccess || status == errSecItemNotFound) {
        return common::Result<void>::ok();
    }
    return common::Result<void>::failure(
        error("credential.remove_failed", "delete", status));
}

} // namespace ai::infrastructure
