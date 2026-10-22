#include <ai/ui/ProjectContextController.hpp>

#include <QtConcurrent/QtConcurrentRun>
#include <QVariantMap>

#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace {

using ai::application::ProjectContextCategory;

// The section label without the Markdown heading marker, e.g. "Work Ledger".
QString sectionLabel(ProjectContextCategory category)
{
    std::string_view heading = ai::application::projectContextSectionHeading(category);
    if (heading.rfind("## ", 0) == 0) heading.remove_prefix(3);
    return QString::fromUtf8(heading.data(), static_cast<qsizetype>(heading.size()));
}

// METADATA ONLY: never the rendered text, never a path.
QVariantList projectSections(const ai::application::ProjectContextSnapshot& snapshot)
{
    QVariantList rows;
    for (const auto& report : snapshot.budget.sections) {
        QVariantMap row;
        row.insert(QStringLiteral("category"), QString::fromLatin1(ai::application::toString(report.category)));
        row.insert(QStringLiteral("label"), sectionLabel(report.category));
        row.insert(QStringLiteral("authority"), QString::fromLatin1(ai::application::toString(
            ai::application::projectContextAuthorityOf(report.category))));
        row.insert(QStringLiteral("limitTokens"), static_cast<qulonglong>(report.limitTokens));
        row.insert(QStringLiteral("tokens"), static_cast<qulonglong>(report.usedTokens));
        row.insert(QStringLiteral("omitted"), static_cast<uint>(report.omittedItems));
        row.insert(QStringLiteral("truncated"), report.truncated);
        uint included = 0;
        uint sources = 0;
        for (const auto& section : snapshot.sections) {
            if (section.category != report.category) continue;
            included = section.includedItems;
            sources = static_cast<uint>(section.sources.size());
        }
        row.insert(QStringLiteral("included"), included);
        row.insert(QStringLiteral("sources"), sources);
        rows.append(row);
    }
    return rows;
}

} // namespace

ProjectContextController::ProjectContextController(
    ai::application::ProjectContextSnapshotService& service, QObject* parent)
    : QObject(parent)
    , service_(service)
{
    connect(&watcher_, &QFutureWatcher<Outcome>::finished, this, &ProjectContextController::finishPreview);
}

ProjectContextController::~ProjectContextController()
{
    // Neither the SQLite reads nor the workspace capture can be interrupted, so a running build is waited
    // for. The service outlives this controller (BackendRuntime is destroyed after every controller).
    watcher_.waitForFinished();
}

void ProjectContextController::setBusy(bool value)
{
    if (busy_ == value) return;
    busy_ = value;
    emit busyChanged();
}

void ProjectContextController::resetState()
{
    hasSnapshot_ = false;
    projectId_.clear();
    selectedRunId_.clear();
    includesProjectMemory_ = false;
    errorCode_.clear();
    errorText_.clear();
    policyId_.clear();
    fingerprint_.clear();
    shortFingerprint_.clear();
    workspaceFingerprint_.clear();
    usedTokens_ = 0;
    totalTokens_ = 0;
    truncated_ = false;
    sourceCount_ = 0;
    sections_.clear();
    snapshot_.reset();
}

quint64 ProjectContextController::preview(
    const QString& projectId, const QString& selectedRunId, bool includeDerivedProjectMemory)
{
    if (projectId.isEmpty() || busy_) return 0;

    // A stale snapshot is never shown as current: it is dropped the moment a new preview is accepted.
    resetState();
    projectId_ = projectId;
    selectedRunId_ = selectedRunId;
    includesProjectMemory_ = includeDerivedProjectMemory;
    emit snapshotChanged();

    ai::application::ProjectContextSnapshotRequest request;
    request.projectId = projectId.toStdString();
    if (!selectedRunId.isEmpty()) request.selectedRunId = selectedRunId.toStdString();
    request.includeDerivedProjectMemory = includeDerivedProjectMemory;

    const quint64 requestId = ++nextRequestId_;
    const quint64 generation = generation_;
    auto* service = &service_;
    setBusy(true);
    watcher_.setFuture(QtConcurrent::run(
        [service, request, requestId, generation, projectId, selectedRunId, includeDerivedProjectMemory] {
            Outcome outcome;
            outcome.requestId = requestId;
            outcome.generation = generation;
            outcome.includesProjectMemory = includeDerivedProjectMemory;
            outcome.projectId = projectId;
            outcome.selectedRunId = selectedRunId;
            try {
                auto built = service->build(request);
                if (!built.success()) {
                    outcome.errorCode = QString::fromStdString(built.error()->code);
                    outcome.errorText = QString::fromStdString(built.error()->message);
                    return outcome;
                }
                outcome.success = true;
                outcome.snapshot = std::move(built.value());
            } catch (const std::exception& exception) {
                outcome.success = false;
                outcome.errorCode = QStringLiteral("project_context.unexpected_failure");
                outcome.errorText = QString::fromUtf8(exception.what());
            } catch (...) {
                outcome.success = false;
                outcome.errorCode = QStringLiteral("project_context.unexpected_failure");
                outcome.errorText = QStringLiteral("The project context could not be built.");
            }
            return outcome;
        }));
    return requestId;
}

void ProjectContextController::clear()
{
    // A running build is invalidated, not interrupted: its result is dropped when it arrives.
    ++generation_;
    const bool hadState = hasSnapshot_ || !errorCode_.isEmpty() || !projectId_.isEmpty();
    resetState();
    if (hadState) emit snapshotChanged();
}

void ProjectContextController::finishPreview()
{
    const Outcome outcome = watcher_.result();
    setBusy(false);
    if (outcome.generation != generation_) return;  // cleared while it ran: nothing to show, nothing to announce

    resetState();
    projectId_ = outcome.projectId;
    selectedRunId_ = outcome.selectedRunId;
    includesProjectMemory_ = outcome.includesProjectMemory;
    if (outcome.success) {
        snapshot_ = std::move(outcome.snapshot);
        const auto& snapshot = *snapshot_;
        hasSnapshot_ = true;
        policyId_ = QString::fromStdString(snapshot.policyId);
        fingerprint_ = QString::fromStdString(snapshot.fingerprint);
        shortFingerprint_ = QString::fromStdString(ai::application::shortProjectContextFingerprint(snapshot.fingerprint));
        workspaceFingerprint_ = QString::fromStdString(
            ai::application::shortProjectContextFingerprint(snapshot.workspaceFingerprint));
        usedTokens_ = static_cast<int>(snapshot.budget.usedTokens);
        totalTokens_ = static_cast<int>(snapshot.budget.totalLimitTokens);
        truncated_ = snapshot.budget.truncated;
        sourceCount_ = static_cast<int>(snapshot.sources.size());
        sections_ = projectSections(snapshot);
    } else {
        errorCode_ = outcome.errorCode;
        errorText_ = outcome.errorText;
    }
    emit snapshotChanged();
    emit previewFinished(outcome.requestId, outcome.success);
}

ai::common::Result<ai::application::ProjectContextSnapshot> ProjectContextController::snapshotForAttachment(
    const std::string& projectId, const std::string& fingerprint) const
{
    if (busy_ || !snapshot_.has_value()) {
        return ai::common::Result<ai::application::ProjectContextSnapshot>::failure(
            {"chat_context.preview_not_available", "Preview the Project Context Snapshot before attaching it"});
    }
    if (snapshot_->projectId != projectId || snapshot_->fingerprint != fingerprint) {
        return ai::common::Result<ai::application::ProjectContextSnapshot>::failure(
            {"chat_context.stale_preview", "The Project Context preview changed; preview it again"});
    }
    return ai::common::Result<ai::application::ProjectContextSnapshot>::ok(*snapshot_);
}
