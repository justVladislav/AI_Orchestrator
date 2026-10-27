pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel

    required property var hostWindow
    required property var colors
    required property var controller
    required property string diffChangeId
    required property string originalText
    required property string proposedText
    required property bool confirmApplyDialogOpen
    required property bool confirmRejectDialogOpen

    signal memberSelectionRequested(string proposalId)
    signal diffToggleRequested(string changeId)
    signal largeDiffRequested(string relativePath)
    signal applyRequested()
    signal rejectRequested()

    objectName: "manualHandoffReviewSection"
    Layout.fillWidth: true
    spacing: panel.hostWindow.dp(5)
    visible: panel.controller && panel.controller.reviewMembers.length > 0

    Text {
        Layout.fillWidth: true
        text: qsTranslate("ManualHandoffPanel", "Verified proposals")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(11)
        font.weight: Font.DemiBold
    }

    Repeater {
        id: reviewMembersRepeater
        objectName: "manualHandoffReviewMembersRepeater"
        model: panel.controller ? panel.controller.reviewMembers : []
        delegate: Rectangle {
            id: memberItem
            required property var modelData
            objectName: "manualHandoffReviewMemberItem_" + memberItem.modelData.proposalId
            Layout.fillWidth: true
            Layout.preferredHeight: memberColumn.implicitHeight + panel.hostWindow.dp(12)
            radius: panel.colors.cornerRadiusSmall
            color: panel.controller
                   && panel.controller.selectedMemberProposalId
                      === memberItem.modelData.proposalId
                   ? panel.colors.accentSoft : panel.colors.surface
            border.width: 1
            border.color: memberItem.modelData.reviewRejected === true
                          ? panel.colors.error
                          : panel.proposalStateColor(memberItem.modelData.state)

            ColumnLayout {
                id: memberColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: panel.hostWindow.dp(9)
                anchors.rightMargin: panel.hostWindow.dp(9)
                spacing: panel.hostWindow.dp(2)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(6)
                    Text {
                        text: memberItem.modelData.alreadyApplied ? qsTranslate("ManualHandoffPanel", "applied")
                              : memberItem.modelData.state
                        color: panel.proposalStateColor(memberItem.modelData.state)
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: memberItem.modelData.reviewRejected === true
                        text: qsTranslate("ManualHandoffPanel", "rejected by reviewer")
                        color: panel.colors.error
                        font.pixelSize: panel.hostWindow.dp(8)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTranslate("ManualHandoffPanel", "%1 file(s) · %2 finding(s)")
                              .arg(memberItem.modelData.changes.length)
                              .arg(memberItem.modelData.findings.length)
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: memberItem.modelData.summary
                    color: panel.colors.textSecondary
                    font.pixelSize: panel.hostWindow.dp(8.5)
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }

            MouseArea {
                objectName: "manualHandoffReviewMemberRowArea_" + memberItem.modelData.proposalId
                // Plain zero-arg signal, invokable directly via
                // QMetaObject::invokeMethod(area, "activated") from a
                // real-QML click-through test - MouseArea's own
                // built-in clicked(mouse) signal takes a MouseEvent
                // argument invokeMethod cannot easily construct. A
                // real mouse click reaches the same onActivated
                // handler via onClicked emitting this signal, so
                // both paths run identical logic.
                signal activated()
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: activated()
                onActivated: panel.memberSelectionRequested(memberItem.modelData.proposalId)
            }
        }
    }

    // ── Selected member detail: findings + Apply/Reject ─────────
    ColumnLayout {
        id: selectedMemberColumn
        Layout.fillWidth: true
        spacing: panel.hostWindow.dp(6)
        visible: panel.controller
            && panel.controller.selectedMemberProposalId.length > 0

        Repeater {
            model: panel.controller
                ? (panel.controller.selectedMemberDetail.findings || []) : []
            delegate: RowLayout {
                id: findingRow
                required property var modelData
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(5)
                Text { text: "⚠"; color: panel.colors.error; font.pixelSize: panel.hostWindow.dp(9) }
                Text {
                    Layout.fillWidth: true
                    text: findingRow.modelData.code + ": " + findingRow.modelData.message
                    color: panel.colors.error
                    font.pixelSize: panel.hostWindow.dp(9)
                    wrapMode: Text.WordWrap
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: {
                const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                return Boolean(detail && detail.reviewRejected === true)
            }
            text: {
                const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                const reason = detail && detail.reviewRejectedReason
                return reason && reason.length > 0
                    ? qsTranslate("ManualHandoffPanel", "Rejected by reviewer: %1").arg(reason)
                    : qsTranslate("ManualHandoffPanel", "Rejected by reviewer.")
            }
            color: panel.colors.error
            font.pixelSize: panel.hostWindow.dp(9)
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: {
                const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                return Boolean(detail && detail.alreadyApplied === true)
            }
            text: qsTranslate("ManualHandoffPanel", "Already applied to the workspace.")
            color: panel.colors.success
            font.pixelSize: panel.hostWindow.dp(9)
            font.weight: Font.DemiBold
        }

        RowLayout {
            objectName: "manualHandoffReviewApplyStatusRow"
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(5)
            visible: {
                const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                return Boolean(detail && detail.alreadyApplied !== true
                    && detail.reviewRejected !== true
                    && typeof detail.applyAllowed === "boolean")
            }
            Text {
                text: {
                    const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                    if (!detail) return ""
                    return detail.applyAllowed === true ? "✓" : "⚠"
                }
                color: {
                    const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                    if (!detail) return panel.colors.textMuted
                    if (detail.applyAllowed !== true) return panel.colors.error
                    return detail.applyBlockCode === "manifest_apply.eligible"
                        ? panel.colors.success : panel.colors.textMuted
                }
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: {
                    const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                    if (!detail) return ""
                    return panel.applyBlockLabel(detail.applyBlockCode, detail.applyBlockMessage)
                }
                color: {
                    const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                    if (!detail) return panel.colors.textMuted
                    if (detail.applyAllowed !== true) return panel.colors.error
                    return detail.applyBlockCode === "manifest_apply.eligible"
                        ? panel.colors.success : panel.colors.textMuted
                }
                font.pixelSize: panel.hostWindow.dp(9)
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(7)
            visible: {
                const detail = panel.controller ? panel.controller.selectedMemberDetail : {}
                return Boolean(detail && detail.alreadyApplied !== true
                    && detail.reviewRejected !== true)
            }

            RunActionButton {
                objectName: "manualHandoffReviewApplyButton"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTranslate("ManualHandoffPanel", "Apply to workspace")
                primary: true
                enabled: !panel.confirmApplyDialogOpen && !panel.confirmRejectDialogOpen
                    && panel.controller
                    && !panel.controller.busy
                    && panel.controller.selectedMemberDetail.applyAllowed === true
                onClicked: panel.applyRequested()
            }
            RunActionButton {
                objectName: "manualHandoffReviewRejectButton"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTranslate("ManualHandoffPanel", "Reject / discard")
                danger: true
                enabled: !panel.confirmApplyDialogOpen && !panel.confirmRejectDialogOpen
                    && panel.controller && !panel.controller.busy
                onClicked: panel.rejectRequested()
            }
            Item { Layout.fillWidth: true }
        }

        Repeater {
            objectName: "manualHandoffReviewSelectedChangesRepeater"
            model: panel.controller
                ? (panel.controller.selectedMemberDetail.changes || []) : []
            delegate: ColumnLayout {
                id: reviewChangeItem
                required property var modelData
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(4)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(6)
                    Text { text: reviewChangeItem.modelData.operation; color: panel.colors.accent; font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                    Text {
                        Layout.fillWidth: true
                        text: reviewChangeItem.modelData.relativePath
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(9)
                        elide: Text.ElideMiddle
                    }
                    Text {
                        text: panel.diffChangeId === reviewChangeItem.modelData.changeId
                              ? qsTranslate("ManualHandoffPanel", "Hide diff") : qsTranslate("ManualHandoffPanel", "View diff")
                        color: reviewDiffToggleArea.containsMouse
                               ? panel.colors.textPrimary : panel.colors.accent
                        font.pixelSize: panel.hostWindow.dp(9)
                        MouseArea {
                            id: reviewDiffToggleArea
                            objectName: "manualHandoffReviewDiffToggle_"
                                + reviewChangeItem.modelData.changeId
                            // Plain zero-arg signal, invokable
                            // directly via QMetaObject::invokeMethod
                            // from a real-QML click-through test -
                            // see manualHandoffReviewMemberRowArea's
                            // own matching doc comment above. A real
                            // mouse click reaches the same
                            // onActivated handler via onClicked
                            // emitting this signal.
                            signal activated()
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: activated()
                            onActivated: panel.diffToggleRequested(reviewChangeItem.modelData.changeId)
                        }
                    }
                    Text {
                        visible: panel.diffChangeId === reviewChangeItem.modelData.changeId
                        text: qsTranslate("ManualHandoffPanel", "Open large view")
                        color: reviewDiffLargeArea.containsMouse
                               ? panel.colors.textPrimary : panel.colors.accent
                        font.pixelSize: panel.hostWindow.dp(9)
                        MouseArea {
                            id: reviewDiffLargeArea
                            objectName: "manualHandoffReviewDiffLarge_"
                                + reviewChangeItem.modelData.changeId
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.largeDiffRequested(reviewChangeItem.modelData.relativePath)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: panel.diffChangeId === reviewChangeItem.modelData.changeId
                    spacing: panel.hostWindow.dp(6)

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(2)
                        Text { text: qsTranslate("ManualHandoffPanel", "Original"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8) }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: panel.hostWindow.dp(140)
                            color: panel.colors.surface
                            border.width: 1
                            border.color: panel.colors.border
                            Flickable {
                                anchors.fill: parent
                                anchors.margins: panel.hostWindow.dp(6)
                                contentWidth: width
                                contentHeight: reviewOriginalText.implicitHeight
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                TextEdit {
                                    id: reviewOriginalText
                                    width: parent.width
                                    readOnly: true
                                    selectByMouse: true
                                    textFormat: TextEdit.PlainText
                                    text: panel.originalText.length > 0
                                          ? panel.originalText : qsTranslate("ManualHandoffPanel", "(no original content)")
                                    color: panel.colors.textSecondary
                                    selectionColor: panel.colors.accent
                                    selectedTextColor: panel.colors.textOnAccent
                                    font.pixelSize: panel.hostWindow.dp(8.5)
                                    font.family: "monospace"
                                    wrapMode: TextEdit.WrapAnywhere
                                }
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(2)
                        Text { text: qsTranslate("ManualHandoffPanel", "Proposed"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8) }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: panel.hostWindow.dp(140)
                            color: panel.colors.surface
                            border.width: 1
                            border.color: panel.colors.border
                            Flickable {
                                anchors.fill: parent
                                anchors.margins: panel.hostWindow.dp(6)
                                contentWidth: width
                                contentHeight: reviewProposedText.implicitHeight
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                TextEdit {
                                    id: reviewProposedText
                                    width: parent.width
                                    readOnly: true
                                    selectByMouse: true
                                    textFormat: TextEdit.PlainText
                                    text: panel.proposedText.length > 0
                                          ? panel.proposedText : qsTranslate("ManualHandoffPanel", "(no proposed content)")
                                    color: panel.colors.textSecondary
                                    selectionColor: panel.colors.accent
                                    selectedTextColor: panel.colors.textOnAccent
                                    font.pixelSize: panel.hostWindow.dp(8.5)
                                    font.family: "monospace"
                                    wrapMode: TextEdit.WrapAnywhere
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    function proposalStateColor(state) {
        if (state === "ready_for_review" || state === "applied"
                || state === "no_changes_needed") return panel.colors.success
        if (state === "rejected" || state === "apply_failed") return panel.colors.error
        return panel.colors.accent
    }

    function applyBlockLabel(code, message) {
        if (!code) return ""
        if (code === "manifest_apply.gate_not_applicable")
            return qsTranslate("ManualHandoffPanel", "Manifest verification gate not applicable")
        if (code === "manifest_apply.eligible")
            return qsTranslate("ManualHandoffPanel", "Verified")
        return message && message.length > 0 ? code + ": " + message : code
    }
}
