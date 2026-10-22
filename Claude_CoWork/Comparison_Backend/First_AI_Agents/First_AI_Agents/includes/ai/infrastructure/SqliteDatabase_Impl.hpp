#pragma once

// Internal implementation header — include ONLY from SqliteRepositories.cpp
// and SqliteComparisonRepository.cpp. Never include from application headers.
//
// MERGE INSTRUCTION: Replace the inline `struct SqliteDatabase::Impl { ... };`
// block in SqliteRepositories.cpp (~line 782-795 in v4) with:
//   #include "ai/infrastructure/SqliteDatabase_Impl.hpp"
// This satisfies ODR: the definition is identical in both translation units
// that include this header.

#include <filesystem>
#include <mutex>
#include <sqlite3.h>

// Full definition of the pimpl for SqliteDatabase. Repository .cpp files that
// are declared `friend class` in SqliteDatabase can access these fields.
struct ai::infrastructure::SqliteDatabase::Impl {
    sqlite3* handle {nullptr};
    std::filesystem::path databasePath;
    mutable std::mutex mutex;

    ~Impl()
    {
        if (handle != nullptr) {
            sqlite3_close_v2(handle);
        }
    }
};
