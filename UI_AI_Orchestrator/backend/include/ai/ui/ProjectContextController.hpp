#pragma once

#include <ai/application/ProjectContextSnapshotService.hpp>

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <optional>
#include <string>

/**
 * PC-1D: the Qt boundary of the Project Context Snapshot preview.
 *
 * A snapshot is built only when the user presses Preview (preview()), on a worker thread, because the
 * workspace capture hashes the whole policy-visible tree and the SQLite reads take the shared database
 * mutex. The service is synchronous, read-only and provider-free: this controller never writes or calls a
 * provider. It keeps the most recently previewed snapshot in memory so Chat can attach those exact reviewed bytes;
 * clear/project change drops it. The Qt projection is created on the UI thread after the worker
 * finishes; the worker touches no QObject, QVariant or QML.
 *
 * QML receives METADATA only: the fingerprint, the token budget, and one row per section (category,
 * authority, tokens, included / omitted items, truncation). It never receives the rendered text, the Work
 * Ledger prose, Project Memory text or any filesystem path.
 *
 * Rules, all pinned by tests:
 *  - one build at a time: preview() returns 0 while one is running (the UI disables the button), exactly
 *    like the other asynchronous controllers; a refused call changes nothing;
 *  - an accepted preview first drops the previous snapshot, so a stale result is never shown as current,
 *    and a failure leaves NO snapshot, only errorCode / errorText;
 *  - clear() invalidates a running build too: its result is discarded when it arrives (no signal, no state);
 *  - the destructor waits for a running build: neither the SQLite reads nor the workspace capture can be
 *    interrupted today, so closing the application can wait for as long as one capture takes. The service
 *    must outlive this controller (BackendRuntime is destroyed after every controller).
 *
 * Chat attachment remains explicit: merely previewing sends nothing. ChatController can request the full value by
 * exact project + fingerprint only after QML asks to quote/attach it; QML itself still receives metadata only.
 */
class ProjectContextController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasSnapshot READ hasSnapshot NOTIFY snapshotChanged)
    Q_PROPERTY(QString projectId READ projectId NOTIFY snapshotChanged)
    Q_PROPERTY(QString selectedRunId READ selectedRunId NOTIFY snapshotChanged)
    Q_PROPERTY(bool includesProjectMemory READ includesProjectMemory NOTIFY snapshotChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY snapshotChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY snapshotChanged)
    Q_PROPERTY(QString policyId READ policyId NOTIFY snapshotChanged)
    Q_PROPERTY(QString fingerprint READ fingerprint NOTIFY snapshotChanged)
    Q_PROPERTY(QString shortFingerprint READ shortFingerprint NOTIFY snapshotChanged)
    Q_PROPERTY(QString workspaceFingerprint READ workspaceFingerprint NOTIFY snapshotChanged)
    Q_PROPERTY(int usedTokens READ usedTokens NOTIFY snapshotChanged)
    Q_PROPERTY(int totalTokens READ totalTokens NOTIFY snapshotChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY snapshotChanged)
    Q_PROPERTY(int sourceCount READ sourceCount NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY snapshotChanged)

public:
    explicit ProjectContextController(
        ai::application::ProjectContextSnapshotService& service, QObject* parent = nullptr);
    ~ProjectContextController() override;

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool hasSnapshot() const noexcept { return hasSnapshot_; }
    [[nodiscard]] QString projectId() const { return projectId_; }
    [[nodiscard]] QString selectedRunId() const { return selectedRunId_; }
    [[nodiscard]] bool includesProjectMemory() const noexcept { return includesProjectMemory_; }
    [[nodiscard]] QString errorCode() const { return errorCode_; }
    [[nodiscard]] QString errorText() const { return errorText_; }
    [[nodiscard]] QString policyId() const { return policyId_; }
    [[nodiscard]] QString fingerprint() const { return fingerprint_; }
    [[nodiscard]] QString shortFingerprint() const { return shortFingerprint_; }
    [[nodiscard]] QString workspaceFingerprint() const { return workspaceFingerprint_; }
    [[nodiscard]] int usedTokens() const noexcept { return usedTokens_; }
    [[nodiscard]] int totalTokens() const noexcept { return totalTokens_; }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] int sourceCount() const noexcept { return sourceCount_; }
    [[nodiscard]] QVariantList sections() const { return sections_; }

    // Starts a preview and returns its request id, or 0 when refused (empty project id, or a build is
    // already running). selectedRunId may be empty (a project-only snapshot). The previous snapshot is
    // dropped as soon as the call is accepted. The result arrives through previewFinished().
    Q_INVOKABLE quint64 preview(
        const QString& projectId, const QString& selectedRunId, bool includeDerivedProjectMemory);

    // Drops the current snapshot or error and invalidates a running build (its result is discarded).
    Q_INVOKABLE void clear();

    // C++ controller boundary only; never exposed through a Q_PROPERTY/Q_INVOKABLE. Returns a copy only when the
    // exact current preview is named, preventing a stale or another project's snapshot from being attached.
    [[nodiscard]] ai::common::Result<ai::application::ProjectContextSnapshot> snapshotForAttachment(
        const std::string& projectId, const std::string& fingerprint) const;

signals:
    void busyChanged();
    void snapshotChanged();
    // Emitted once per accepted preview that was not invalidated by clear(). requestId is the id preview()
    // returned; success false means errorCode / errorText describe the failure and there is no snapshot.
    void previewFinished(quint64 requestId, bool success);

private:
    struct Outcome {
        quint64 requestId {0};
        quint64 generation {0};
        bool success {false};
        bool includesProjectMemory {false};
        QString projectId;
        QString selectedRunId;
        QString errorCode;
        QString errorText;
        ai::application::ProjectContextSnapshot snapshot;
    };

    void finishPreview();
    void resetState();
    void setBusy(bool value);

    ai::application::ProjectContextSnapshotService& service_;
    QFutureWatcher<Outcome> watcher_;
    quint64 nextRequestId_ {0};
    quint64 generation_ {0};
    bool busy_ {false};

    bool hasSnapshot_ {false};
    QString projectId_;
    QString selectedRunId_;
    bool includesProjectMemory_ {false};
    QString errorCode_;
    QString errorText_;
    QString policyId_;
    QString fingerprint_;
    QString shortFingerprint_;
    QString workspaceFingerprint_;
    int usedTokens_ {0};
    int totalTokens_ {0};
    bool truncated_ {false};
    int sourceCount_ {0};
    QVariantList sections_;
    std::optional<ai::application::ProjectContextSnapshot> snapshot_;
};
