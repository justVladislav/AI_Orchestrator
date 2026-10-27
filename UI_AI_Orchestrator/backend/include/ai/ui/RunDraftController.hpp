#pragma once

#include <ai/application/planning/RunDraftService.hpp>
#include <ai/ui/RunDraftListModel.hpp>

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

/** Qt command/read-model adapter for saved Run drafts. */
class RunDraftController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* drafts READ drafts CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit RunDraftController(
        ai::application::RunDraftService& service,
        QObject* parent = nullptr);

    [[nodiscard]] QAbstractItemModel* drafts() noexcept;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE QString createDraft(
        const QString& name,
        const QString& projectId,
        const QString& title,
        const QString& task,
        const QString& launchMode,
        const QString& executionProfileId,
        const QVariantList& roster,
        int maxParallelism,
        const QString& workspacePath,
        bool automaticVerificationRepair = true,
        // AG-17.6: "execution-manifest-v1" for a Manifest roster draft, empty otherwise.
        const QString& workflowId = {},
        // Explicit per-Run provider limits (custom roster only), the optional-key map of
        // RunController::startRunWithRoster; empty = the Backend defaults.
        const QVariantMap& providerBudget = {},
        // PC-2: interface contracts saved with the draft (see RunContractVariant.hpp).
        const QVariantList& interfaceContracts = {});
    Q_INVOKABLE bool updateDraft(
        const QString& draftId,
        const QString& name,
        const QString& projectId,
        const QString& title,
        const QString& task,
        const QString& launchMode,
        const QString& executionProfileId,
        const QVariantList& roster,
        int maxParallelism,
        const QString& workspacePath,
        bool automaticVerificationRepair = true,
        // AG-17.6: "execution-manifest-v1" for a Manifest roster draft, empty otherwise.
        const QString& workflowId = {},
        const QVariantMap& providerBudget = {},
        const QVariantList& interfaceContracts = {});
    Q_INVOKABLE bool removeDraft(const QString& draftId);
    // Returns the same QVariantMap shape RunController::rerunDraft() produces
    // (minus sourceRunId, plus isDraft:true) so NewRunDialog's existing
    // prefillRosterFrom()/onOpened prefill logic needs no changes to consume
    // it. An empty map signals failure (see lastError()).
    Q_INVOKABLE QVariantMap openDraft(const QString& draftId);
    Q_INVOKABLE void refresh();

signals:
    void draftsChanged();
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();

private:
    [[nodiscard]] static std::vector<ai::application::StageModelAssignment>
    parseRoster(const QVariantList& roster);
    void reportError(const ai::common::Error& error);
    void clearError();

    ai::application::RunDraftService& service_;
    RunDraftListModel drafts_;
    QString lastError_;
};
