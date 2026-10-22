// ============================================================================
// BackendRuntime.cpp — Comparison delta
//
// This file is NOT a standalone translation unit. It documents only the
// additions that must be merged into the original BackendRuntime.cpp.
// See IMPLEMENTATION_NOTES.md for exact insertion points.
// ============================================================================

// ── 1. New #include at the top (after existing infrastructure includes) ──────

#include "ai/application/ComparisonService.hpp"
#include "ai/application/IdGenerator.hpp"          // for application::generateUuid
#include "ai/infrastructure/SqliteComparisonRepository.hpp"
// SqliteDatabase_Impl.hpp is NOT included here; SqliteComparisonRepository.cpp
// includes it internally. BackendRuntime.cpp only needs the repository header.

// ── 2. UnavailableComparisonRepository (add after UnavailableChatRepository) ─

class UnavailableComparisonRepository final : public application::IComparisonRepository {
public:
    explicit UnavailableComparisonRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::ComparisonSession> insertSession(
        application::ComparisonSession) override { return fail<application::ComparisonSession>(); }
    common::Result<application::ComparisonSession> updateSession(
        application::ComparisonSession) override { return fail<application::ComparisonSession>(); }
    [[nodiscard]] common::Result<std::optional<application::ComparisonSession>> findSessionById(
        const application::ComparisonId&) const override
    { return fail<std::optional<application::ComparisonSession>>(); }
    [[nodiscard]] common::Result<std::vector<application::ComparisonSession>> listSessionsByProject(
        const application::ProjectId&) const override
    { return fail<std::vector<application::ComparisonSession>>(); }
    common::Result<application::ComparisonResult> insertResult(
        application::ComparisonResult) override { return fail<application::ComparisonResult>(); }
    common::Result<application::ComparisonResult> updateResult(
        application::ComparisonResult) override { return fail<application::ComparisonResult>(); }
    [[nodiscard]] common::Result<std::optional<application::ComparisonResult>> findResultById(
        const application::ComparisonResultId&) const override
    { return fail<std::optional<application::ComparisonResult>>(); }
    [[nodiscard]] common::Result<std::vector<application::ComparisonResult>> listResultsBySession(
        const application::ComparisonId&) const override
    { return fail<std::vector<application::ComparisonResult>>(); }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

// ── 3. StorageBundle — add comparisonRepository field ────────────────────────
//
// Inside BackendRuntime::Impl::StorageBundle, add:
//
//     std::unique_ptr<application::IComparisonRepository> comparisonRepository;
//
// After the existing chatRepository field.

// ── 4. openStorage() — wire SqliteComparisonRepository ──────────────────────
//
// In the failure branch (opened.error()), append to the StorageBundle
// construction:
//
//     std::make_unique<UnavailableComparisonRepository>(reason),
//     opened.error()};
//                 ^
//  (This replaces the existing `opened.error()};` line — just add the new
//   repository unique_ptr before it.)
//
// In the success branch, append:
//
//     std::make_unique<infrastructure::SqliteComparisonRepository>(database),
//     std::nullopt};
//                 ^
//  (Same: before the existing `std::nullopt};`.)

// ── 5. BackendRuntime::Impl member list — add comparisonService ──────────────
//
// In the Impl member list, after `application::ProjectService projectService;`,
// add:
//
//     application::ComparisonService comparisonService;
//
// And in the Impl constructor initializer list, after `, projectService(…)`,
// add:
//
//     , comparisonService(
//           *storage.comparisonRepository,
//           *storage.projectRepository,
//           application::generateUuid)
//
// (generateUuid is declared in ai/application/IdGenerator.hpp which is already
//  included transitively; if not, add #include "ai/application/IdGenerator.hpp".)

// ── 6. BackendRuntime accessor — add comparisonService() ────────────────────
//
// After the existing BackendRuntime::chatService() definition, add:
//
application::ComparisonService& BackendRuntime::comparisonService() noexcept
{
    return impl_->comparisonService;
}
