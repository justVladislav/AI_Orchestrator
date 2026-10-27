pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: taskItem

    required property var hostWindow
    required property var colors
    required property var controller

    required property var modelData
    Layout.fillWidth: true
    Layout.preferredHeight: taskColumn.implicitHeight + taskItem.hostWindow.dp(14)
    radius: taskItem.colors.cornerRadiusSmall
    color: taskItem.colors.surface
    border.width: 1
    border.color: taskItem.statusColor(taskItem.modelData.status)

    ColumnLayout {
        id: taskColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: taskItem.hostWindow.dp(10)
        anchors.rightMargin: taskItem.hostWindow.dp(10)
        spacing: taskItem.hostWindow.dp(3)

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: taskItem.modelData.id + " · " + taskItem.modelData.role
                color: taskItem.colors.textPrimary
                font.pixelSize: taskItem.hostWindow.dp(10)
                font.weight: Font.DemiBold
            }
            Text {
                text: taskItem.modelData.requestedCapability
                color: taskItem.colors.textMuted
                font.pixelSize: taskItem.hostWindow.dp(8.5)
            }
            Text {
                text: taskItem.modelData.status
                    + (taskItem.modelData.attempts > 1
                        ? qsTranslate("ManifestPanel", " (attempt %1)").arg(taskItem.modelData.attempts) : "")
                color: taskItem.statusColor(taskItem.modelData.status)
                font.pixelSize: taskItem.hostWindow.dp(8.5)
                font.weight: Font.DemiBold
            }
        }
        // AG-17.6: task -> assignee -> provider/model, from the frozen routing
        // record (Backend projection). Shown only for a roster Run; a
        // single-assignment Run has one implicit executor and needs no line.
        Text {
            objectName: "taskAssigneeText"
            Layout.fillWidth: true
            visible: taskItem.controller !== null
                     && taskItem.controller.routingMode === "roster"
                     && taskItem.modelData.assigneeMemberId.length > 0
            text: qsTranslate("ManifestPanel", "Assigned to %1 (%2) · %3")
                      .arg(taskItem.modelData.assigneeMemberId)
                      .arg(taskItem.modelData.assigneeRole)
                      .arg(taskItem.modelData.assigneeLabel)
            color: taskItem.colors.accent
            wrapMode: Text.WordWrap
            font.pixelSize: taskItem.hostWindow.dp(8.5)
        }
        Text {
            Layout.fillWidth: true
            text: taskItem.modelData.objective
            color: taskItem.colors.textPrimary
            font.pixelSize: taskItem.hostWindow.dp(9)
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: taskItem.modelData.dependencies.length > 0
            text: qsTranslate("ManifestPanel", "Depends on: %1").arg(taskItem.modelData.dependencies.join(", "))
            color: taskItem.colors.textMuted
            font.pixelSize: taskItem.hostWindow.dp(8.5)
        }

        // AG-05: what this task's acceptance requirements actually
        // demand - previously only their pass/fail *results* were
        // ever shown (in the separate manifest-verification.md
        // report), never the requirement definitions themselves.
        ColumnLayout {
            Layout.fillWidth: true
            visible: taskItem.modelData.hasAcceptanceContract
            spacing: taskItem.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: qsTranslate("ManifestPanel", "Acceptance requirements")
                color: taskItem.colors.textMuted
                font.pixelSize: taskItem.hostWindow.dp(8)
                font.weight: Font.DemiBold
            }
            Repeater {
                model: taskItem.modelData.acceptanceRequirements
                delegate: Text {
                    id: requirementText
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: taskItem.hostWindow.dp(6)
                    text: qsTranslate("ManifestPanel", "· %1%2: %3 (%4%5)")
                        .arg(requirementText.modelData.id)
                        .arg(requirementText.modelData.required ? "" : qsTranslate("ManifestPanel", " [optional]"))
                        .arg(requirementText.modelData.description)
                        .arg(requirementText.modelData.verificationKind)
                        .arg(requirementText.modelData.requiredOutputs.length > 0
                             ? qsTranslate("ManifestPanel", ", outputs: %1").arg(requirementText.modelData.requiredOutputs.join(", "))
                             : "")
                    color: taskItem.colors.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: taskItem.hostWindow.dp(8)
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: taskItem.modelData.ownedPaths.length > 0
            text: qsTranslate("ManifestPanel", "Owns: %1").arg(taskItem.modelData.ownedPaths.join(", "))
            color: taskItem.colors.textMuted
            font.pixelSize: taskItem.hostWindow.dp(8.5)
        }

        // AG-15.3: frozen workspace scope + actually-inspected +
        // proposed paths for this task - all Backend truth via
        // ManifestController, no role/category ever computed here.
        ColumnLayout {
            Layout.fillWidth: true
            visible: taskItem.modelData.scopeEntries.length > 0
                || taskItem.modelData.scopeInspectedPaths.length > 0
                || taskItem.modelData.proposedCreatedPaths.length > 0
                || taskItem.modelData.proposedModifiedPaths.length > 0
            spacing: taskItem.hostWindow.dp(1)

            Text {
                Layout.fillWidth: true
                visible: taskItem.modelData.scopeEntries.length > 0
                text: qsTranslate("ManifestPanel", "Scope: %1").arg(taskItem.scopeSummaryText(taskItem.modelData.scopeEntries))
                color: taskItem.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: taskItem.hostWindow.dp(8)
            }
            Text {
                Layout.fillWidth: true
                visible: taskItem.modelData.scopeInspectedPaths.length > 0
                text: qsTranslate("ManifestPanel", "Inspected: %1").arg(taskItem.modelData.scopeInspectedPaths.join(", "))
                color: taskItem.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: taskItem.hostWindow.dp(8)
            }
            Text {
                Layout.fillWidth: true
                visible: taskItem.modelData.proposedCreatedPaths.length > 0
                    || taskItem.modelData.proposedModifiedPaths.length > 0
                text: qsTranslate("ManifestPanel", "Proposed: %1").arg(
                    [taskItem.modelData.proposedCreatedPaths.length > 0
                        ? qsTranslate("ManifestPanel", "created %1").arg(taskItem.modelData.proposedCreatedPaths.join(", ")) : "",
                     taskItem.modelData.proposedModifiedPaths.length > 0
                        ? qsTranslate("ManifestPanel", "modified %1").arg(taskItem.modelData.proposedModifiedPaths.join(", ")) : ""]
                        .filter(function(part) { return part.length > 0 }).join(" · "))
                color: taskItem.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: taskItem.hostWindow.dp(8)
            }
        }
        Text {
            Layout.fillWidth: true
            visible: taskItem.modelData.errorCode.length > 0
            text: taskItem.modelData.errorCode
                + (taskItem.modelData.errorMessage.length > 0
                    ? ": " + taskItem.modelData.errorMessage : "")
            color: taskItem.modelData.status === "failed"
                   ? taskItem.colors.error : taskItem.colors.warning
            font.pixelSize: taskItem.hostWindow.dp(8.5)
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: taskItem.modelData.changeSetId.length > 0
            text: qsTranslate("ManifestPanel", "Proposal: %1").arg(taskItem.modelData.changeSetId)
            color: taskItem.colors.textMuted
            font.pixelSize: taskItem.hostWindow.dp(8.5)
        }

        // AG-04: this task's worker resolved as no-op - show why,
        // not just that it did. Every string here already went
        // through ManifestWorkerNoChanges's Backend validation; QML
        // never sees raw model JSON.
        ColumnLayout {
            Layout.fillWidth: true
            visible: taskItem.modelData.hasNoChangesEvidence
            spacing: taskItem.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: qsTranslate("ManifestPanel", "No changes needed (%1)").arg(taskItem.modelData.noChangesReason)
                color: taskItem.colors.warning
                font.pixelSize: taskItem.hostWindow.dp(8.5)
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                visible: taskItem.modelData.noChangesSummary.length > 0
                text: taskItem.modelData.noChangesSummary
                color: taskItem.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: taskItem.hostWindow.dp(8.5)
            }
            Repeater {
                model: taskItem.modelData.noChangesCriterionEvidence
                delegate: Text {
                    id: criterionText
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: taskItem.hostWindow.dp(6)
                    text: qsTranslate("ManifestPanel", "· %1 (%2): %3")
                        .arg(criterionText.modelData.requirementId)
                        .arg(criterionText.modelData.inspectedPath)
                        .arg(criterionText.modelData.observation)
                    color: taskItem.colors.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: taskItem.hostWindow.dp(8)
                }
            }
            Text {
                Layout.fillWidth: true
                visible: taskItem.modelData.noChangesInspectedPaths.length > 0
                text: qsTranslate("ManifestPanel", "Inspected: %1").arg(taskItem.modelData.noChangesInspectedPaths.join(", "))
                color: taskItem.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: taskItem.hostWindow.dp(8)
            }
        }
    }

    function statusColor(status) {
        if (status === "completed") return colors.success
        if (status === "failed") return colors.error
        if (status === "skipped") return colors.warning
        return colors.accent
    }

    function scopeSummaryText(entries) {
        var byRole = {}
        var order = []
        for (var i = 0; i < entries.length; ++i) {
            var entry = entries[i]
            if (!(entry.role in byRole)) { byRole[entry.role] = []; order.push(entry.role) }
            byRole[entry.role].push(entry.relativePath + (entry.isExpansion ? qsTranslate("ManifestPanel", " (expansion)") : ""))
        }
        var parts = []
        for (var j = 0; j < order.length; ++j) parts.push(order[j] + ": " + byRole[order[j]].join(", "))
        return parts.join(" · ")
    }
}
