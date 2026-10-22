#include "ai/infrastructure/WindowsCredentialStore.hpp"

// This translation unit is Windows-only (see CMakeLists.txt:
// AI_ORCHESTRATOR_BUILD_WINDOWS_CREDENTIALS is only ever true under WIN32).
// wincred.h/CredWriteW/CredReadW/CredDeleteW are the only Win32 surface used
// here; no Qt type is named anywhere in this file.
#include <windows.h>

#include <wincred.h>

#include <cstring>
#include <string>
#include <utility>

namespace ai::infrastructure {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

std::wstring toWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

// Every credential this store touches lives under one fixed prefix, derived
// only from the caller-supplied (non-secret) reference string, so this
// adapter can never collide with an unrelated Windows Credential Manager
// entry another application created.
std::wstring targetNameFor(const std::string& reference)
{
    return L"AIOrchestrator:" + toWide(reference);
}

} // namespace

common::Result<void> WindowsCredentialStore::store(const std::string& reference, const std::string& secret)
{
    if (reference.empty()) {
        return common::Result<void>::failure(
            error("credential.invalid_reference", "Credential reference cannot be empty"));
    }
    const std::wstring targetName = targetNameFor(reference);

    // A UTF-8 copy the Win32 API needs a writable pointer to. It is
    // explicitly zeroed below on every return path, not left to whatever
    // std::string's own deallocation happens to do.
    std::string secretBuffer = secret;

    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(targetName.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(secretBuffer.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(secretBuffer.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    const BOOL ok = CredWriteW(&credential, 0);
    const DWORD lastError = ok ? 0 : GetLastError();

    if (!secretBuffer.empty()) {
        SecureZeroMemory(secretBuffer.data(), secretBuffer.size());
    }

    if (!ok) {
        // The Win32 error code is safe to include (it never contains
        // secret bytes); the secret value itself never reaches this string.
        return common::Result<void>::failure(error(
            "credential.store_failed",
            "Windows Credential Manager write failed (error " + std::to_string(lastError) + ")"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<std::string>> WindowsCredentialStore::load(const std::string& reference) const
{
    if (reference.empty()) {
        return common::Result<std::optional<std::string>>::failure(
            error("credential.invalid_reference", "Credential reference cannot be empty"));
    }
    const std::wstring targetName = targetNameFor(reference);

    PCREDENTIALW credential = nullptr;
    const BOOL ok = CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &credential);
    if (!ok) {
        const DWORD lastError = GetLastError();
        if (lastError == ERROR_NOT_FOUND) {
            return common::Result<std::optional<std::string>>::ok(std::nullopt);
        }
        return common::Result<std::optional<std::string>>::failure(error(
            "credential.load_failed",
            "Windows Credential Manager read failed (error " + std::to_string(lastError) + ")"));
    }

    std::string secret(
        reinterpret_cast<const char*>(credential->CredentialBlob),
        credential->CredentialBlobSize);
    // Best-effort cleanup of the OS-owned buffer before CredFree releases
    // it - not strictly required (this process did not allocate it), but
    // consistent with never leaving a readable copy of the secret longer
    // than necessary.
    if (credential->CredentialBlobSize > 0) {
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    }
    CredFree(credential);

    return common::Result<std::optional<std::string>>::ok(std::move(secret));
}

common::Result<void> WindowsCredentialStore::remove(const std::string& reference)
{
    if (reference.empty()) {
        return common::Result<void>::failure(
            error("credential.invalid_reference", "Credential reference cannot be empty"));
    }
    const std::wstring targetName = targetNameFor(reference);

    const BOOL ok = CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0);
    if (!ok) {
        const DWORD lastError = GetLastError();
        if (lastError == ERROR_NOT_FOUND) {
            // Idempotent: nothing configured is not a failure.
            return common::Result<void>::ok();
        }
        return common::Result<void>::failure(error(
            "credential.remove_failed",
            "Windows Credential Manager delete failed (error " + std::to_string(lastError) + ")"));
    }
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
