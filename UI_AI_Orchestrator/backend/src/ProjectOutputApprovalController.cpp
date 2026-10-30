#include <ai/ui/ProjectOutputApprovalController.hpp>

#include <ai/application/workspace/FilesystemPathUtils.hpp>
#include <ai/application/workspace/ProjectOutputApprovalService.hpp>

#include <QDateTime>
#include <QStringList>
#include <QVariantList>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <exception>
#include <utility>

namespace {

using namespace ai::application;

constexpr const char* kUnavailableCode = "output_approval.unavailable";
constexpr const char* kInvalidRequestCode = "output_approval.invalid_request";
constexpr const char* kBusyCode = "output_approval.busy";
constexpr const char* kPreviewStaleCode = "output_approval.preview_stale";
constexpr const char* kNotFoundCode = "output_approval.not_found";
constexpr const char* kRootChangedCode = "output_approval.root_changed";
constexpr const char* kInternalCode = "output_approval.internal_error";

// Names the backend gave, as UTF-8 text - never through the ANSI code page.
QString text(const std::string& utf8)
{
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

std::string utf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString isoTime(std::chrono::system_clock::time_point time)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC).toString(Qt::ISODateWithMs);
}

QVariantMap errorMap(const std::string& code, const std::string& message)
{
    QVariantMap map;
    map.insert(QStringLiteral("success"), false);
    map.insert(QStringLiteral("errorCode"), text(code));
    map.insert(QStringLiteral("errorText"), text(message));
    map.insert(QStringLiteral("cancelled"), code == kOutputApprovalCancelledCode);
    map.insert(QStringLiteral("changed"), false);
    return map;
}

QVariantMap errorMap(const ai::common::Error& error)
{
    return errorMap(error.code, error.message);
}

QVariantMap approvalMap(const ProjectOutputApproval& approval, std::optional<bool> appliesToCurrentRoot)
{
    QVariantMap map;
    map.insert(QStringLiteral("approvalId"), text(approval.id));
    map.insert(QStringLiteral("projectId"), text(approval.projectId));
    map.insert(QStringLiteral("relativePath"), text(approval.relativePath));
    map.insert(QStringLiteral("workspaceRoot"), text(approval.workspaceRoot));
    if (appliesToCurrentRoot.has_value()) map.insert(QStringLiteral("appliesToCurrentRoot"), *appliesToCurrentRoot);
    map.insert(QStringLiteral("inspectionComplete"), approval.inspectionComplete);
    map.insert(QStringLiteral("nestedSourcesAcknowledged"), approval.nestedSourcesAcknowledged);
    map.insert(QStringLiteral("approvedBy"), text(approval.approvedBy));
    map.insert(QStringLiteral("approvedAtMs"), static_cast<qlonglong>(std::chrono::duration_cast<std::chrono::milliseconds>(
        approval.approvedAt.time_since_epoch()).count()));
    map.insert(QStringLiteral("approvedAt"), isoTime(approval.approvedAt));
    return map;
}

// Everything the user is asked to confirm, as the backend established it. Counts are exact only when countsAreLowerBounds is false.
QVariantMap previewMap(const OutputApprovalPreview& preview)
{
    QVariantMap map;
    map.insert(QStringLiteral("success"), true);
    map.insert(QStringLiteral("workspaceRoot"), text(preview.workspaceRoot));
    map.insert(QStringLiteral("relativePath"), text(preview.relativePath));
    map.insert(QStringLiteral("exists"), preview.exists);
    map.insert(QStringLiteral("existingPrefix"), text(preview.existingPrefix));
    const auto& inspection = preview.inspection;
    QString status = QStringLiteral("not_inspected");
    if (preview.exists) {
        switch (inspection.status) {
        case OutputInspectionStatus::Complete: status = QStringLiteral("complete"); break;
        case OutputInspectionStatus::Incomplete: status = QStringLiteral("incomplete"); break;
        case OutputInspectionStatus::Failed: status = QStringLiteral("failed"); break;
        case OutputInspectionStatus::Cancelled: status = QStringLiteral("cancelled"); break;
        }
    }
    map.insert(QStringLiteral("inspectionStatus"), status);
    map.insert(QStringLiteral("inspectionComplete"), !preview.requiresIncompleteConfirmation);
    map.insert(QStringLiteral("countsAreLowerBounds"), preview.exists && inspection.status == OutputInspectionStatus::Incomplete);
    map.insert(QStringLiteral("entries"), static_cast<qulonglong>(inspection.entries));
    map.insert(QStringLiteral("files"), static_cast<qulonglong>(inspection.files));
    map.insert(QStringLiteral("bytes"), static_cast<qulonglong>(inspection.bytes));
    map.insert(QStringLiteral("nestedSourceCount"), static_cast<qulonglong>(inspection.nestedSourceCount));
    QStringList samples;
    for (const auto& sample : inspection.nestedSourceSamples) {
        if (samples.size() >= 10) break;
        samples.push_back(text(sample));
    }
    map.insert(QStringLiteral("nestedSourceSamples"), samples);
    map.insert(QStringLiteral("skippedLinks"), static_cast<qulonglong>(inspection.skippedLinks));
    map.insert(QStringLiteral("hitEntryLimit"), inspection.hitEntryLimit);
    map.insert(QStringLiteral("hitDepthLimit"), inspection.hitDepthLimit);
    map.insert(QStringLiteral("requiresNestedConfirmation"), preview.requiresNestedConfirmation);
    map.insert(QStringLiteral("requiresIncompleteConfirmation"), preview.requiresIncompleteConfirmation);
    map.insert(QStringLiteral("alreadyApproved"), preview.alreadyApproved);
    return map;
}

QString osUser()
{
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) user = qEnvironmentVariable("USER");
    return user;
}

} // namespace

ProjectOutputApprovalController::ProjectOutputApprovalController(ProjectOutputApprovalService* service, QObject* parent)
    : QObject(parent)
    , service_(service)
{
    connect(&watcher_, &QFutureWatcher<Outcome>::finished, this, &ProjectOutputApprovalController::finish);
}

ProjectOutputApprovalController::~ProjectOutputApprovalController()
{
    // Stop first, then wait: the worker only holds values, the service (which outlives this object) and its own cancellation state.
    if (active_) active_->stop.request_stop();
    watcher_.waitForFinished();
}

bool ProjectOutputApprovalController::inspecting() const noexcept
{
    return active_ != nullptr && activeCancellable_;
}

void ProjectOutputApprovalController::setLastError(const QString& code, const QString& textValue)
{
    if (lastErrorCode_ == code && lastErrorText_ == textValue) return;
    lastErrorCode_ = code;
    lastErrorText_ = textValue;
    emit lastErrorChanged();
}

quint64 ProjectOutputApprovalController::refuse(const char* code, const QString& textValue)
{
    setLastError(QString::fromLatin1(code), textValue);
    return 0;
}

void ProjectOutputApprovalController::bumpGeneration()
{
    ++generation_;
}

void ProjectOutputApprovalController::invalidateCache()
{
    cache_.reset();
}

// Common admission of every operation: a service, a project, and no other operation running. A request for ANOTHER project while an
// inspection runs also asks for that inspection to stop and drops its preview - but never starts a second operation, and never
// touches the data of the first.
bool ProjectOutputApprovalController::admit(const QString& projectId)
{
    if (service_ == nullptr) {
        refuse(kUnavailableCode, QStringLiteral("Output approvals are not available: there is no durable storage."));
        return false;
    }
    if (projectId.isEmpty()) {
        refuse(kInvalidRequestCode, QStringLiteral("A project is required."));
        return false;
    }
    if (active_ != nullptr) {
        if (projectId != activeProjectId_ && activeCancellable_ && !active_->stop.stop_requested()) {
            active_->stop.request_stop();
            invalidateCache();
            bumpGeneration();
        }
        refuse(kBusyCode, QStringLiteral("Another output-approval operation is still running."));
        return false;
    }
    return true;
}

std::shared_ptr<ProjectOutputApprovalController::Cancellation> ProjectOutputApprovalController::start(
    const QString& operation, const QString& projectId, bool cancellable, quint64& requestId, quint64 previewRequestId,
    const QString& subject)
{
    requestId = ++nextRequestId_;
    active_ = std::make_shared<Cancellation>();
    activeRequestId_ = requestId;
    activeProjectId_ = projectId;
    activeOperation_ = operation;
    activeCancellable_ = cancellable;
    activeSubject_ = subject;
    activeGeneration_ = generation_;
    activePreviewRequestId_ = previewRequestId;
    setLastError(QString(), QString());
    emit busyChanged();
    return active_;
}

void ProjectOutputApprovalController::launch(std::function<Outcome()> work)
{
    watcher_.setFuture(QtConcurrent::run([work = std::move(work)]() -> Outcome {
        try {
            return work();
        } catch (const std::exception& exception) {
            Outcome outcome;
            outcome.map = errorMap(kInternalCode, std::string("The operation failed unexpectedly: ") + exception.what());
            return outcome;
        } catch (...) {
            Outcome outcome;
            outcome.map = errorMap(kInternalCode, "The operation failed unexpectedly.");
            return outcome;
        }
    }));
}

quint64 ProjectOutputApprovalController::previewApproval(const QString& projectId, const QString& relativePath)
{
    if (!admit(projectId)) return 0;
    if (relativePath.isEmpty()) return refuse(kInvalidRequestCode, QStringLiteral("A folder path is required."));

    // A new preview replaces whatever was shown before: the older one can no longer be approved.
    invalidateCache();
    bumpGeneration();
    quint64 requestId = 0;
    const auto cancellation = start(QStringLiteral("preview"), projectId, /*cancellable=*/true, requestId, 0, relativePath);
    auto* service = service_;
    const std::string project = utf8(projectId);
    const std::string path = utf8(relativePath);
    launch([service, cancellation, project, path, requestId, projectId, relativePath] {
        Outcome outcome;
        outcome.operation = QStringLiteral("preview");
        outcome.requestId = requestId;
        outcome.projectId = projectId;
        outcome.subject = relativePath;
        const std::stop_token token = cancellation->stop.get_token();
        if (token.stop_requested()) {
            outcome.map = errorMap(kOutputApprovalCancelledCode, "The inspection was cancelled before it started");
            return outcome;
        }
        const auto previewed = service->preview(project, path, token);
        if (!previewed.success()) {
            outcome.map = errorMap(*previewed.error());
            return outcome;
        }
        outcome.map = previewMap(previewed.value());
        outcome.previewOk = true;
        outcome.previewRoot = previewed.value().workspaceRoot;
        outcome.previewPath = previewed.value().relativePath;
        return outcome;
    });
    return requestId;
}

quint64 ProjectOutputApprovalController::approve(const QString& projectId, const QString& relativePath, quint64 previewRequestId,
    bool acknowledgeNestedSources, bool acknowledgeIncomplete)
{
    if (!admit(projectId)) return 0;
    if (relativePath.isEmpty()) return refuse(kInvalidRequestCode, QStringLiteral("A folder path is required."));
    // The authority is the preview this controller kept - not anything the caller says about what was checked.
    if (previewRequestId == 0 || !cache_.has_value() || cache_->requestId != previewRequestId || cache_->projectId != projectId
        || cache_->generation != generation_
        || outputApprovalPathKey(utf8(relativePath)) != outputApprovalPathKey(cache_->relativePath)) {
        return refuse(kPreviewStaleCode,
            QStringLiteral("There is no current preview of this folder for this project; preview it again before approving."));
    }
    OutputApprovalRequest request;
    request.projectId = utf8(projectId);
    request.relativePath = cache_->relativePath;
    request.expectedWorkspaceRoot = cache_->root;
    request.expectedRelativePath = cache_->relativePath;
    request.acknowledgeNestedSources = acknowledgeNestedSources;
    request.acknowledgeIncomplete = acknowledgeIncomplete;
    request.approvedBy = utf8(osUser());

    quint64 requestId = 0;
    const auto cancellation = start(QStringLiteral("approve"), projectId, /*cancellable=*/true, requestId, previewRequestId, relativePath);
    auto* service = service_;
    launch([service, cancellation, request = std::move(request), requestId, projectId, relativePath] {
        Outcome outcome;
        outcome.operation = QStringLiteral("approve");
        outcome.requestId = requestId;
        outcome.projectId = projectId;
        outcome.subject = relativePath;
        const std::stop_token token = cancellation->stop.get_token();
        if (token.stop_requested()) {
            outcome.map = errorMap(kOutputApprovalCancelledCode, "The approval was cancelled before it started");
            return outcome;
        }
        // The service checks the token again right before its short atomic commit; from there on nothing is undone.
        const auto approved = service->approve(request, token);
        if (!approved.success()) {
            outcome.map = errorMap(*approved.error());
            return outcome;
        }
        const bool inserted = approved.value().outcome == ProjectOutputApprovalCommitOutcome::Inserted;
        QVariantMap map = approvalMap(approved.value().approval, std::nullopt);
        map.insert(QStringLiteral("success"), true);
        map.insert(QStringLiteral("changed"), inserted);
        map.insert(QStringLiteral("alreadyApproved"), !inserted);
        map.insert(QStringLiteral("outcome"), inserted ? QStringLiteral("inserted") : QStringLiteral("already_approved"));
        outcome.map = std::move(map);
        outcome.changed = inserted;
        return outcome;
    });
    return requestId;
}

quint64 ProjectOutputApprovalController::recheckApproval(const QString& projectId, const QString& approvalId)
{
    if (!admit(projectId)) return 0;
    if (approvalId.isEmpty()) return refuse(kInvalidRequestCode, QStringLiteral("An approval id is required."));
    quint64 requestId = 0;
    const auto cancellation = start(QStringLiteral("recheck"), projectId, /*cancellable=*/true, requestId, 0, approvalId);
    auto* service = service_;
    const std::string project = utf8(projectId);
    const std::string id = utf8(approvalId);
    launch([service, cancellation, project, id, requestId, projectId, approvalId] {
        Outcome outcome;
        outcome.operation = QStringLiteral("recheck");
        outcome.requestId = requestId;
        outcome.projectId = projectId;
        outcome.subject = approvalId;
        const std::stop_token token = cancellation->stop.get_token();
        if (token.stop_requested()) {
            outcome.map = errorMap(kOutputApprovalCancelledCode, "The inspection was cancelled before it started");
            return outcome;
        }
        const auto findRecord = [&]() -> ai::common::Result<std::optional<OutputApprovalView>> {
            using Result = ai::common::Result<std::optional<OutputApprovalView>>;
            const auto listed = service->list(project);
            if (!listed.success()) return Result::failure(*listed.error());
            for (const auto& view : listed.value()) {
                if (view.approval.id == id && view.approval.projectId == project) return Result::ok(std::optional<OutputApprovalView>(view));
            }
            return Result::ok(std::nullopt);
        };
        const auto found = findRecord();
        if (!found.success()) {
            outcome.map = errorMap(*found.error());
            return outcome;
        }
        if (!found.value().has_value()) {
            outcome.map = errorMap(kNotFoundCode, "This project has no such approval.");
            return outcome;
        }
        const ProjectOutputApproval record = found.value()->approval;
        // The stored record is told apart from today's inspection: the whole record sits under "approval", its completeness is
        // "storedInspectionComplete", and the top level keeps the CURRENT facts (inspectionComplete among them) or none when the
        // inspection did not happen. The record only fills in the identity fields that the outcome lacks.
        const auto withRecord = [&record](QVariantMap map, std::optional<bool> applies) {
            const QVariantMap facts = approvalMap(record, applies);
            for (auto it = facts.constBegin(); it != facts.constEnd(); ++it) {
                if (it.key() == QLatin1String("inspectionComplete")) continue;
                if (!map.contains(it.key())) map.insert(it.key(), it.value());
            }
            map.insert(QStringLiteral("storedInspectionComplete"), record.inspectionComplete);
            map.insert(QStringLiteral("approval"), facts);
            return map;
        };
        // A dormant approval belongs to an EARLIER root of the project: it is not silently checked against today's folder.
        if (!found.value()->appliesToCurrentRoot) {
            outcome.map = withRecord(errorMap(kRootChangedCode, "This approval was made for an earlier folder of the project; it is kept and can be revoked."), false);
            return outcome;
        }
        const auto previewed = service->preview(project, record.relativePath, token);
        if (!previewed.success()) {
            outcome.map = withRecord(errorMap(*previewed.error()), true);
            return outcome;
        }
        const OutputApprovalPreview& preview = previewed.value();
        if (outputApprovalWorkspaceKey(detail::pathFromUtf8(preview.workspaceRoot)) != record.workspaceKey
            || outputApprovalPathKey(preview.relativePath) != record.pathKey) {
            outcome.map = withRecord(errorMap(kRootChangedCode, "The project's folder is no longer the one this approval was made for."), false);
            return outcome;
        }
        // The record may have been revoked, or the root switched, while the folder was inspected: do not confirm a record that is gone.
        const auto again = findRecord();
        if (!again.success()) {
            outcome.map = withRecord(errorMap(*again.error()), true);
            return outcome;
        }
        if (!again.value().has_value() || !again.value()->appliesToCurrentRoot) {
            outcome.map = withRecord(errorMap(kOutputApprovalStaleCode, "The approval changed while the folder was inspected."), false);
            return outcome;
        }
        outcome.map = withRecord(previewMap(preview), true);
        return outcome;
    });
    return requestId;
}

quint64 ProjectOutputApprovalController::listApprovals(const QString& projectId)
{
    if (!admit(projectId)) return 0;
    quint64 requestId = 0;
    const auto cancellation = start(QStringLiteral("list"), projectId, /*cancellable=*/false, requestId);
    auto* service = service_;
    const std::string project = utf8(projectId);
    launch([service, cancellation, project, requestId, projectId] {
        Outcome outcome;
        outcome.operation = QStringLiteral("list");
        outcome.requestId = requestId;
        outcome.projectId = projectId;
        if (cancellation->stop.stop_requested()) {
            outcome.map = errorMap(kOutputApprovalCancelledCode, "The listing was cancelled before it started");
            return outcome;
        }
        const auto listed = service->list(project);
        if (!listed.success()) {
            outcome.map = errorMap(*listed.error());
            return outcome;
        }
        QVariantList approvals;
        for (const auto& view : listed.value()) approvals.push_back(approvalMap(view.approval, view.appliesToCurrentRoot));
        outcome.map.insert(QStringLiteral("success"), true);
        outcome.map.insert(QStringLiteral("approvals"), approvals);
        outcome.map.insert(QStringLiteral("changed"), false);
        return outcome;
    });
    return requestId;
}

quint64 ProjectOutputApprovalController::revokeApproval(const QString& projectId, const QString& approvalId)
{
    if (!admit(projectId)) return 0;
    if (approvalId.isEmpty()) return refuse(kInvalidRequestCode, QStringLiteral("An approval id is required."));
    quint64 requestId = 0;
    const auto cancellation = start(QStringLiteral("revoke"), projectId, /*cancellable=*/false, requestId, 0, approvalId);
    auto* service = service_;
    const std::string project = utf8(projectId);
    const std::string id = utf8(approvalId);
    launch([service, cancellation, project, id, requestId, projectId, approvalId] {
        Outcome outcome;
        outcome.operation = QStringLiteral("revoke");
        outcome.requestId = requestId;
        outcome.projectId = projectId;
        outcome.subject = approvalId;
        // Before the command a cancel is honoured; the short DELETE itself is never interrupted.
        if (cancellation->stop.stop_requested()) {
            outcome.map = errorMap(kOutputApprovalCancelledCode, "The revocation was cancelled before it started");
            return outcome;
        }
        const auto revoked = service->revoke(project, id);
        if (!revoked.success()) {
            outcome.map = errorMap(*revoked.error());
            return outcome;
        }
        outcome.map.insert(QStringLiteral("success"), true);
        outcome.map.insert(QStringLiteral("approvalId"), approvalId);
        outcome.map.insert(QStringLiteral("removed"), revoked.value());
        outcome.map.insert(QStringLiteral("changed"), revoked.value());
        outcome.changed = revoked.value();
        return outcome;
    });
    return requestId;
}

bool ProjectOutputApprovalController::cancelInspection()
{
    if (active_ == nullptr || !activeCancellable_ || active_->stop.stop_requested()) return false;
    active_->stop.request_stop();
    invalidateCache();
    bumpGeneration();
    return true;
}

void ProjectOutputApprovalController::setProjectContext(const QString& projectId)
{
    if (projectId == contextProjectId_) return;
    contextProjectId_ = projectId;
    invalidateCache();
    bumpGeneration();
    if (active_ != nullptr && activeCancellable_) active_->stop.request_stop();
    emit projectContextChanged();
}

void ProjectOutputApprovalController::invalidatePreview()
{
    invalidateCache();
    bumpGeneration();
}

void ProjectOutputApprovalController::finish()
{
    Outcome outcome = watcher_.result();
    if (outcome.operation.isEmpty()) {   // the worker threw: the identity is the one the call was accepted with
        outcome.operation = activeOperation_;
        outcome.requestId = activeRequestId_;
        outcome.projectId = activeProjectId_;
        outcome.subject = activeSubject_;
    }
    const bool stale = activeGeneration_ != generation_;
    const bool cancelRequested = active_ != nullptr && active_->stop.stop_requested();
    const quint64 previewRequestId = activePreviewRequestId_;
    const bool success = outcome.map.value(QStringLiteral("success")).toBool();

    outcome.map.insert(QStringLiteral("operation"), outcome.operation);
    outcome.map.insert(QStringLiteral("requestId"), outcome.requestId);
    outcome.map.insert(QStringLiteral("projectId"), outcome.projectId);
    outcome.map.insert(QStringLiteral("stale"), stale);
    outcome.map.insert(QStringLiteral("cancelRequested"), cancelRequested);
    if (outcome.operation == QLatin1String("preview") || outcome.operation == QLatin1String("approve")) {
        outcome.map.insert(QStringLiteral("requestedPath"), outcome.subject);
    }

    // A read-only preview becomes the one an approve may use only when it is current: not cancelled, and nothing invalidated it.
    if (outcome.operation == QLatin1String("preview") && outcome.previewOk && !stale && !cancelRequested) {
        cache_ = CachedPreview {outcome.requestId, outcome.projectId, outcome.previewRoot, outcome.previewPath, generation_};
    }
    if (outcome.operation == QLatin1String("approve")) {
        const bool cancelled = outcome.map.value(QStringLiteral("cancelled")).toBool();
        // Written (or already there): that preview is spent. Cancelled: it is invalid. A refusal (a missing confirmation, say) keeps
        // it, so the user can confirm and try again without inspecting the folder twice.
        if ((success || cancelled) && cache_.has_value() && cache_->requestId == previewRequestId) invalidateCache();
    }

    active_.reset();
    activeRequestId_ = 0;
    activeProjectId_.clear();
    activeOperation_.clear();
    activeSubject_.clear();
    activeCancellable_ = false;
    activePreviewRequestId_ = 0;
    emit busyChanged();

    if (outcome.operation == QLatin1String("preview")) {
        emit previewFinished(outcome.requestId, outcome.projectId, outcome.subject, outcome.map);
    } else if (outcome.operation == QLatin1String("approve")) {
        emit approveFinished(outcome.requestId, outcome.projectId, outcome.subject, outcome.map);
    } else if (outcome.operation == QLatin1String("recheck")) {
        emit recheckFinished(outcome.requestId, outcome.projectId, outcome.subject, outcome.map);
    } else if (outcome.operation == QLatin1String("list")) {
        emit listFinished(outcome.requestId, outcome.projectId, outcome.map);
    } else if (outcome.operation == QLatin1String("revoke")) {
        emit revokeFinished(outcome.requestId, outcome.projectId, outcome.subject, outcome.map);
    }
    // For the ORIGINAL project, also when a cancel or another selection arrived meanwhile.
    if (outcome.changed) emit approvalsChanged(outcome.projectId);
}
