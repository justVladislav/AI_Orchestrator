#pragma once

#include <ai/application/AgentToolReadModelService.hpp>
#include <ai/application/proposals/ProposalReviewTypes.hpp>

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <vector>

// One row per change proposal (ProposedChangeSet header) produced by the
// selected agent tool session. relativePath-only file changes and
// validation findings for the currently selected proposal are exposed by
// AgentSessionController as a QVariantMap (selectedProposalDetail), not by
// this model, so per-file content stays lazily loaded.
//
// AG-07 reviewer-rejection status is a separate, parallel enrichment (see
// setReviewRejections()) rather than a field of AgentProposalReadModel: that
// type belongs to the unrelated-ownership AgentToolReadModelService, and
// ProposalReviewTypes.hpp/ProposalReviewService deliberately keep the human
// review decision out of the validator-truth read model it is not part of.
class AgentProposalListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ChangeSetIdRole = Qt::UserRole + 1,
        StateRole,
        SummaryRole,
        AgentRoleRole,
        ChangeCountRole,
        FindingCountRole,
        CreatedAtTextRole,
        ReviewRejectedRole,
        ReviewRejectedReasonRole,
        ReviewRejectedAtTextRole,
        SupersedesChangeSetIdRole,
        SupersededByChangeSetIdRole,
        SupersededRole
    };
    Q_ENUM(Role)

    explicit AgentProposalListModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ai::application::AgentProposalReadModel> rows);
    [[nodiscard]] const std::vector<ai::application::AgentProposalReadModel>& rows() const noexcept
    {
        return rows_;
    }

    // Replaces the reviewer-rejection enrichment keyed by changeSetId.
    // A changeSetId absent from this map has no reviewer rejection. Emits
    // dataChanged for every row so ReviewRejected* roles refresh even when
    // replace() itself is not also called (e.g. right after a fresh reject
    // command, before the next full session reload).
    void setReviewRejections(
        QHash<QString, ai::application::ProposalReviewRejection> rejections);

private:
    std::vector<ai::application::AgentProposalReadModel> rows_;
    QHash<QString, ai::application::ProposalReviewRejection> rejections_;
};
