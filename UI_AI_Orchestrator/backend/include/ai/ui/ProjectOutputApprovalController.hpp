#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace ai::application {
class ProjectOutputApprovalService;
}

/**
 * P3d: the Qt adapter for the user's approvals of generated-output folders (ai::application::ProjectOutputApprovalService).
 * No QML yet (P3e). This class holds Qt state and converts results; every rule - the root taken from the project record, the path
 * checks, the protected storage, the two independent confirmations, the atomic commit - stays in the service.
 *
 * ALL FIVE operations run on a worker (QtConcurrent + one std::stop_source + one QFutureWatcher, like
 * ProjectVerificationSetupController::checkProjectSetup): the service reads the file system (list() canonicalises the root) and
 * SQLite may wait on its mutex, so nothing of it is called from the GUI thread. The worker captures values, the service pointer and
 * its own cancellation state only; it never touches this object. The result is delivered on the GUI thread.
 *
 * One operation at a time. Every accepted call returns an id > 0 at once and ends with EXACTLY ONE terminal signal that carries the
 * id, the project and the requested path / approval id it was started with - never the project that is selected when it ends. A call
 * that is refused returns 0 and says why in lastErrorCode()/lastErrorText().
 *
 * PREVIEW -> APPROVE. approve() is bound to a preview this controller itself kept after a successful, current previewApproval():
 * the id, the project, the backend's canonical root and path, and the context generation. The root and the path of the application
 * request come from that backend record - never from what the caller sends - and the service still checks everything again
 * (an earlier preview is information, not a pass). A preview of another project / path / generation, an unknown or cancelled id, or
 * one invalidated by setProjectContext(), invalidatePreview(), cancelInspection() or a newer preview is output_approval.preview_stale
 * and writes nothing. The two acknowledgements come from the caller's explicit action and are never set here. The author recorded is
 * the best-effort operating-system user name.
 *
 * LATE RESULTS. Every terminal map carries "stale" (the generation changed while the operation ran) and "cancelRequested". A stale
 * read-only result is still announced - with its original id and project, so a future QML can filter it - but it never becomes the
 * preview an approve may use.
 *
 * THE COMMIT BOUNDARY. A cancel is cooperative. Before the service reaches its short atomic commit it stops with
 * output_approval.cancelled and nothing is written; once the commit has begun it is never undone for the sake of the GUI: a record
 * that was written (Inserted / AlreadyApproved) or a row that revoke() really deleted is reported as it is, with approvalsChanged()
 * for the ORIGINAL project, even when the cancel or a project change arrived meanwhile.
 */
class ProjectOutputApprovalController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool inspecting READ inspecting NOTIFY busyChanged)
    Q_PROPERTY(quint64 activeRequestId READ activeRequestId NOTIFY busyChanged)
    Q_PROPERTY(QString activeProjectId READ activeProjectId NOTIFY busyChanged)
    Q_PROPERTY(QString activeOperation READ activeOperation NOTIFY busyChanged)
    Q_PROPERTY(QString projectContextId READ projectContextId NOTIFY projectContextChanged)
    Q_PROPERTY(QString lastErrorCode READ lastErrorCode NOTIFY lastErrorChanged)
    Q_PROPERTY(QString lastErrorText READ lastErrorText NOTIFY lastErrorChanged)

public:
    // `service` is the runtime-owned service and outlives this object; null (no durable storage) makes every operation refuse with
    // output_approval.unavailable - there is no stand-in.
    explicit ProjectOutputApprovalController(
        ai::application::ProjectOutputApprovalService* service, QObject* parent = nullptr);
    ~ProjectOutputApprovalController() override;

    [[nodiscard]] bool available() const noexcept { return service_ != nullptr; }
    [[nodiscard]] bool busy() const noexcept { return active_ != nullptr; }
    [[nodiscard]] bool inspecting() const noexcept;
    [[nodiscard]] quint64 activeRequestId() const noexcept { return activeRequestId_; }
    [[nodiscard]] QString activeProjectId() const { return activeProjectId_; }
    [[nodiscard]] QString activeOperation() const { return activeOperation_; }
    [[nodiscard]] QString projectContextId() const { return contextProjectId_; }
    [[nodiscard]] QString lastErrorCode() const { return lastErrorCode_; }
    [[nodiscard]] QString lastErrorText() const { return lastErrorText_; }

    // Read-only: walks the folder's metadata, creates nothing. The project's root comes from the project record, not from here.
    Q_INVOKABLE quint64 previewApproval(const QString& projectId, const QString& relativePath);

    // Writes the approval, bound to the preview named by previewRequestId (see above).
    Q_INVOKABLE quint64 approve(const QString& projectId, const QString& relativePath, quint64 previewRequestId,
        bool acknowledgeNestedSources, bool acknowledgeIncomplete);

    // Read-only inspection of an EXISTING approval, found by its id strictly inside this project. It changes and approves nothing.
    // The result keeps the stored record and today's inspection apart: the top level carries the CURRENT facts of the preview
    // (inspectionStatus, inspectionComplete, countsAreLowerBounds, the counts, the requires* flags) and none of them when the
    // inspection did not happen (an error); "approval" is the whole stored record as list() shows it, with ITS inspectionComplete,
    // also given as "storedInspectionComplete". The record in the database is never touched.
    Q_INVOKABLE quint64 recheckApproval(const QString& projectId, const QString& approvalId);

    Q_INVOKABLE quint64 listApprovals(const QString& projectId);
    Q_INVOKABLE quint64 revokeApproval(const QString& projectId, const QString& approvalId);

    // Asks the running preview / approve / recheck to stop and invalidates the preview. False when none runs or it was already asked.
    Q_INVOKABLE bool cancelInspection();

    // The selection of the panel: only an id and a generation, no root, no status, no file system, no SQL. A different project
    // cancels a running inspection and invalidates the preview.
    Q_INVOKABLE void setProjectContext(const QString& projectId);
    Q_INVOKABLE void invalidatePreview();

signals:
    void busyChanged();
    void projectContextChanged();
    void lastErrorChanged();
    void previewFinished(quint64 requestId, const QString& projectId, const QString& requestedPath, const QVariantMap& result);
    void approveFinished(quint64 requestId, const QString& projectId, const QString& requestedPath, const QVariantMap& result);
    void recheckFinished(quint64 requestId, const QString& projectId, const QString& approvalId, const QVariantMap& result);
    void listFinished(quint64 requestId, const QString& projectId, const QVariantMap& result);
    void revokeFinished(quint64 requestId, const QString& projectId, const QString& approvalId, const QVariantMap& result);
    // A record was really inserted or deleted for this project (never for AlreadyApproved or a revoke that found nothing).
    void approvalsChanged(const QString& projectId);

private:
    struct Cancellation {
        std::stop_source stop;
    };
    struct Outcome {
        QString operation;
        quint64 requestId {0};
        QString projectId;
        QString subject; // the requested path, or the approval id
        QVariantMap map;
        bool changed {false};
        // Kept for a successful preview: what the backend established.
        bool previewOk {false};
        std::string previewRoot;
        std::string previewPath;
    };
    struct CachedPreview {
        quint64 requestId {0};
        QString projectId;
        std::string root;
        std::string relativePath;
        quint64 generation {0};
    };

    quint64 refuse(const char* code, const QString& text);
    bool admit(const QString& projectId);
    std::shared_ptr<Cancellation> start(const QString& operation, const QString& projectId, bool cancellable, quint64& requestId,
        quint64 previewRequestId = 0, const QString& subject = {});
    void launch(std::function<Outcome()> work);
    void finish();
    void bumpGeneration();
    void invalidateCache();
    void setLastError(const QString& code, const QString& text);

    ai::application::ProjectOutputApprovalService* service_ {nullptr};
    QFutureWatcher<Outcome> watcher_;

    std::shared_ptr<Cancellation> active_;
    quint64 activeRequestId_ {0};
    QString activeProjectId_;
    QString activeOperation_;
    QString activeSubject_;
    // preview / approve / recheck can be cancelled and are "inspecting"; list / revoke finish on their own.
    bool activeCancellable_ {false};
    quint64 activeGeneration_ {0};
    quint64 activePreviewRequestId_ {0};
    quint64 nextRequestId_ {0};

    QString contextProjectId_;
    quint64 generation_ {1};
    std::optional<CachedPreview> cache_;

    QString lastErrorCode_;
    QString lastErrorText_;
};
