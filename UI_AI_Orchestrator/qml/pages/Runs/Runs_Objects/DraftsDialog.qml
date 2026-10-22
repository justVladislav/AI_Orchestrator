pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var hostWindow
    required property var colors
    // Plain JS snapshot built by RunsPage from backend.runDrafts.drafts,
    // already ordered most-recently-updated first by the repository.
    required property var draftsModel
    required property var projectsSnapshot
    // Reads one saved draft completely (RunsPage: backend.runDrafts.openDraft). Called only when a card is expanded, so
    // the list itself stays a cheap snapshot.
    property var detailsFor: function (draftId) { return ({}) }

    signal openRequested(string draftId)
    signal deleteRequested(string draftId)

    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(880),
                    hostWindow.dp(880))
    height: Math.min(parent ? parent.height - hostWindow.dp(48) : hostWindow.dp(720),
                     hostWindow.dp(760))
    modal: true
    standardButtons: Dialog.NoButton

    // A small rounded label: where a plan draft came from, its order, what it is suggested to follow, whether it was
    // edited. Presentation only - every value comes from the Backend's projection of the draft.
    component Badge: Rectangle {
        id: badge
        property string label: ""
        property color tone: dialog.colors.textSecondary
        property color fill: dialog.colors.surfaceAlternative
        implicitWidth: badgeText.implicitWidth + dialog.hostWindow.dp(14)
        implicitHeight: badgeText.implicitHeight + dialog.hostWindow.dp(6)
        radius: dialog.colors.cornerRadiusSmall
        color: badge.fill
        border.width: 1
        border.color: badge.tone
        Text {
            id: badgeText
            anchors.centerIn: parent
            text: badge.label
            color: badge.tone
            font.pixelSize: dialog.hostWindow.dp(9)
        }
    }

    // One "label  value" line of the settings block; the value wraps, nothing is elided.
    component SettingRow: RowLayout {
        id: settingRow
        property string label: ""
        property string value: ""
        property color valueColor: dialog.colors.textPrimary
        Layout.fillWidth: true
        spacing: dialog.hostWindow.dp(10)
        Text {
            Layout.preferredWidth: dialog.hostWindow.dp(140)
            Layout.alignment: Qt.AlignTop
            text: settingRow.label
            color: dialog.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(10)
        }
        Text {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            text: settingRow.value
            color: settingRow.valueColor
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(10.5)
        }
    }

    function projectNameFor(projectId) {
        for (let i = 0; i < dialog.projectsSnapshot.length; ++i) {
            if (dialog.projectsSnapshot[i].projectId === projectId)
                return dialog.projectsSnapshot[i].name
        }
        return projectId
    }

    function launchModeText(mode) {
        if (mode === "custom_roster")
            return qsTr("Custom roster")
        if (mode === "saved_execution_profile")
            return qsTr("Saved execution profile")
        return qsTr("Mock workflow")
    }

    // The one-line summary every card shows without being opened.
    function settingsSummary(draft) {
        const roster = draft.roster ? draft.roster.length : 0
        const parts = [dialog.launchModeText(draft.launchMode)]
        if (draft.launchMode === "custom_roster") {
            parts.push(roster === 0 ? qsTr("no models chosen yet")
                                    : (roster === 1 ? qsTr("1 agent") : qsTr("%1 agents").arg(roster)))
        }
        parts.push(qsTr("up to %1 in parallel").arg(draft.maxParallelism))
        return parts.join("  ·  ")
    }

    function rosterLines(roster) {
        const lines = []
        for (let i = 0; i < roster.length; ++i) {
            const agent = roster[i]
            lines.push(qsTr("%1 · %2%3 · %4")
                       .arg(agent.agentName && agent.agentName.length > 0 ? agent.agentName : agent.stageType)
                       .arg(agent.stageType)
                       .arg(agent.leadArchitect ? qsTr(" (lead)") : "")
                       .arg(agent.modelId && agent.modelId.length > 0 ? qsTr("model chosen") : qsTr("model not chosen")))
        }
        return lines
    }

    function budgetLines(budget) {
        const labels = [
            ["maxInputTokens", qsTr("Input tokens")],
            ["maxOutputTokens", qsTr("Output tokens")],
            ["maxTurns", qsTr("Turns")],
            ["maxToolCalls", qsTr("Tool calls")],
            ["maxToolResultBytes", qsTr("Tool result bytes")],
            ["maxWallTimeMs", qsTr("Wall time (ms)")],
            ["maxRequestTimeMs", qsTr("Request time (ms)")]
        ]
        const lines = []
        for (let i = 0; i < labels.length; ++i) {
            const value = budget ? budget[labels[i][0]] : undefined
            if (value !== undefined && value !== null)
                lines.push(qsTr("%1: %2").arg(labels[i][1]).arg(Number(value).toLocaleString(Qt.locale(), "f", 0)))
        }
        return lines
    }

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(54)
        color: dialog.colors.surfaceAlternative
        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(18)
            text: qsTr("Saved drafts")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(15)
            font.weight: Font.DemiBold
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: dialog.colors.separator
        }
    }

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(10)

        Text {
            Layout.fillWidth: true
            visible: dialog.draftsModel.length === 0
            text: qsTr("No saved drafts yet. Use \"Save as draft\" in New Run to keep an in-progress configuration without starting it - reopening a draft never deletes it, so a frequently reused one also works as a template.")
            color: dialog.colors.textSecondary
            font.pixelSize: dialog.hostWindow.dp(11)
            wrapMode: Text.WordWrap
        }

        Flickable {
            id: draftsFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.draftsModel.length > 0
            contentWidth: width
            contentHeight: draftsColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { id: draftsScrollBar }

            ColumnLayout {
                id: draftsColumn
                // Leave the scroll bar its own strip: it must never be drawn over a card or its buttons.
                width: draftsFlick.width - draftsScrollBar.width - dialog.hostWindow.dp(6)
                spacing: dialog.hostWindow.dp(10)

                Repeater {
                    model: dialog.draftsModel

                    delegate: Rectangle {
                        id: draftRow
                        objectName: "draftCard"
                        required property var modelData
                        property bool deleteConfirming: false
                        property bool expanded: false
                        // backend.runDrafts.openDraft(): read when the card is expanded, never for the list.
                        property var details: ({})
                        readonly property bool detailsLoaded: draftRow.details !== undefined
                                                              && draftRow.details.draftId !== undefined

                        onExpandedChanged: {
                            if (draftRow.expanded)
                                draftRow.details = dialog.detailsFor(draftRow.modelData.draftId)
                        }

                        Layout.fillWidth: true
                        implicitHeight: cardColumn.implicitHeight + dialog.hostWindow.dp(24)
                        radius: dialog.colors.cornerRadiusSmall
                        color: dialog.colors.surface
                        border.width: 1
                        border.color: dialog.colors.border

                        ColumnLayout {
                            id: cardColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: dialog.hostWindow.dp(12)
                            spacing: dialog.hostWindow.dp(6)

                            Text {
                                objectName: "draftName"
                                Layout.fillWidth: true
                                text: draftRow.modelData.name
                                color: dialog.colors.textPrimary
                                font.pixelSize: dialog.hostWindow.dp(13)
                                font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                            }

                            Text {
                                Layout.fillWidth: true
                                text: qsTr("%1  ·  Updated %2")
                                      .arg(dialog.projectNameFor(draftRow.modelData.projectId))
                                      .arg(Qt.formatDateTime(draftRow.modelData.updatedAt, "yyyy-MM-dd HH:mm"))
                                color: dialog.colors.textMuted
                                font.pixelSize: dialog.hostWindow.dp(9.5)
                                wrapMode: Text.WordWrap
                            }

                            // PC-3D: provenance of a plan draft (a manual draft shows none of it).
                            Flow {
                                objectName: "draftPlanBadges"
                                Layout.fillWidth: true
                                visible: draftRow.modelData.groupId.length > 0
                                spacing: dialog.hostWindow.dp(5)

                                Badge {
                                    objectName: "draftGroupBadge"
                                    // The position is the draft's own number in the plan; the count is how many
                                    // drafts of the plan are still saved (a deleted draft is not renumbered).
                                    label: qsTr("Plan “%1” · Draft #%2 · %3")
                                           .arg(draftRow.modelData.groupTitle)
                                           .arg(draftRow.modelData.groupOrdinal + 1)
                                           .arg(draftRow.modelData.groupDraftCount === 1
                                                ? qsTr("1 draft remaining")
                                                : qsTr("%1 drafts remaining").arg(draftRow.modelData.groupDraftCount))
                                    tone: dialog.colors.accent
                                    fill: dialog.colors.accentSoft
                                }
                                Badge {
                                    objectName: "draftOriginBadge"
                                    label: draftRow.modelData.groupOrigin === "chat_message"
                                           ? qsTr("From a chat reply") : qsTr("Pasted plan")
                                }
                                Badge {
                                    objectName: "draftDependencyBadge"
                                    visible: draftRow.modelData.dependsOnTitles.length > 0
                                    label: qsTr("Suggested after: %1").arg(draftRow.modelData.dependsOnTitles.join(", "))
                                }
                                Badge {
                                    objectName: "draftSourceRunsBadge"
                                    visible: draftRow.modelData.sourceRunCount > 0
                                    label: qsTr("%1 source Run(s)").arg(draftRow.modelData.sourceRunCount)
                                }
                                Badge {
                                    objectName: "draftEditedBadge"
                                    visible: draftRow.modelData.edited
                                    label: qsTr("Edited since created")
                                    tone: dialog.colors.warning
                                    fill: dialog.colors.warningSoft
                                }
                            }

                            // The task: a few lines when collapsed (ending in "..."), the whole text when expanded.
                            Text {
                                objectName: "draftTaskPreview"
                                Layout.fillWidth: true
                                visible: !draftRow.expanded
                                text: draftRow.modelData.task
                                color: dialog.colors.textSecondary
                                font.pixelSize: dialog.hostWindow.dp(10.5)
                                wrapMode: Text.WordWrap
                                maximumLineCount: 4
                                elide: Text.ElideRight
                            }

                            Text {
                                objectName: "draftSettingsSummary"
                                Layout.fillWidth: true
                                text: dialog.settingsSummary(draftRow.modelData)
                                color: dialog.colors.textMuted
                                font.pixelSize: dialog.hostWindow.dp(9.5)
                                wrapMode: Text.WordWrap
                            }

                            // ---- everything about the draft, on demand -----------------------------------------------
                            ColumnLayout {
                                objectName: "draftDetails"
                                Layout.fillWidth: true
                                visible: draftRow.expanded && draftRow.detailsLoaded
                                spacing: dialog.hostWindow.dp(6)

                                Text {
                                    text: qsTr("Task")
                                    color: dialog.colors.textSecondary
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                    font.weight: Font.Medium
                                }

                                // The whole task, selectable, never truncated.
                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: taskText.implicitHeight + dialog.hostWindow.dp(14)
                                    radius: dialog.colors.cornerRadiusSmall
                                    color: dialog.colors.inputBackground
                                    border.width: 1
                                    border.color: dialog.colors.border

                                    TextEdit {
                                        id: taskText
                                        objectName: "draftTaskText"
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.margins: dialog.hostWindow.dp(7)
                                        readOnly: true
                                        selectByMouse: true
                                        wrapMode: TextEdit.Wrap
                                        text: draftRow.detailsLoaded ? draftRow.details.task : ""
                                        color: dialog.colors.textPrimary
                                        selectionColor: dialog.colors.accent
                                        selectedTextColor: dialog.colors.textOnAccent
                                        font.pixelSize: dialog.hostWindow.dp(10.5)
                                    }
                                }

                                Text {
                                    Layout.topMargin: dialog.hostWindow.dp(4)
                                    text: qsTr("Settings")
                                    color: dialog.colors.textSecondary
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                    font.weight: Font.Medium
                                }

                                SettingRow {
                                    label: qsTr("Launch mode")
                                    value: draftRow.detailsLoaded ? dialog.launchModeText(draftRow.details.launchMode) : ""
                                }
                                SettingRow {
                                    label: qsTr("Workflow")
                                    value: draftRow.detailsLoaded
                                           ? (draftRow.details.workflowId === "execution-manifest-v1"
                                              ? qsTr("Execution Manifest") : qsTr("Default"))
                                           : ""
                                }
                                SettingRow {
                                    objectName: "draftRosterRow"
                                    label: qsTr("Roster")
                                    value: {
                                        if (!draftRow.detailsLoaded)
                                            return ""
                                        const lines = dialog.rosterLines(draftRow.details.roster)
                                        return lines.length === 0
                                               ? qsTr("Empty - choose the models in New Run")
                                               : lines.join("\n")
                                    }
                                }
                                SettingRow {
                                    label: qsTr("Max parallelism")
                                    value: draftRow.detailsLoaded ? String(draftRow.details.maxParallelism) : ""
                                }
                                SettingRow {
                                    objectName: "draftRepairRow"
                                    label: qsTr("Automatic verification repair")
                                    value: draftRow.detailsLoaded
                                           ? (draftRow.details.automaticVerificationRepair
                                              ? qsTr("On - one bounded paid repair wave after a failed verification")
                                              : qsTr("Off"))
                                           : ""
                                    valueColor: draftRow.detailsLoaded && draftRow.details.automaticVerificationRepair
                                                ? dialog.colors.warning : dialog.colors.textPrimary
                                }
                                SettingRow {
                                    objectName: "draftLimitsRow"
                                    label: qsTr("Provider limits")
                                    value: {
                                        if (!draftRow.detailsLoaded)
                                            return ""
                                        const lines = dialog.budgetLines(draftRow.details.providerBudget)
                                        return lines.length === 0 ? qsTr("Not set - the Run defaults apply") : lines.join("\n")
                                    }
                                }
                                SettingRow {
                                    label: qsTr("Workspace")
                                    value: draftRow.detailsLoaded
                                           ? (draftRow.details.workspacePath && draftRow.details.workspacePath.length > 0
                                              ? draftRow.details.workspacePath : qsTr("The project's own workspace"))
                                           : ""
                                }

                                Text {
                                    Layout.topMargin: dialog.hostWindow.dp(4)
                                    text: draftRow.detailsLoaded
                                          ? qsTr("Interface contracts and constraints (%1)").arg(draftRow.details.interfaceContracts.length)
                                          : ""
                                    color: dialog.colors.textSecondary
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                    font.weight: Font.Medium
                                }

                                Repeater {
                                    model: draftRow.detailsLoaded ? draftRow.details.interfaceContracts : []

                                    delegate: ColumnLayout {
                                        id: contractBlock
                                        required property var modelData
                                        objectName: "draftContract"
                                        Layout.fillWidth: true
                                        spacing: dialog.hostWindow.dp(3)

                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("%1 · %2 · %3").arg(contractBlock.modelData.id)
                                                  .arg(contractBlock.modelData.kind === "constraint"
                                                       ? qsTr("constraint") : qsTr("interface contract"))
                                                  .arg(contractBlock.modelData.title)
                                            color: dialog.colors.textPrimary
                                            font.pixelSize: dialog.hostWindow.dp(10.5)
                                            font.weight: Font.Medium
                                            wrapMode: Text.WordWrap
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            text: contractBlock.modelData.appliesToPaths.length === 0
                                                  ? qsTr("Applies to every worker of the Run")
                                                  : qsTr("Applies to: %1").arg(contractBlock.modelData.appliesToPaths.join(", "))
                                            color: dialog.colors.textMuted
                                            font.pixelSize: dialog.hostWindow.dp(9.5)
                                            wrapMode: Text.WrapAnywhere
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            implicitHeight: contractText.implicitHeight + dialog.hostWindow.dp(14)
                                            radius: dialog.colors.cornerRadiusSmall
                                            color: dialog.colors.inputBackground
                                            border.width: 1
                                            border.color: dialog.colors.border

                                            TextEdit {
                                                id: contractText
                                                objectName: "draftContractText"
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.top: parent.top
                                                anchors.margins: dialog.hostWindow.dp(7)
                                                readOnly: true
                                                selectByMouse: true
                                                wrapMode: TextEdit.Wrap
                                                text: contractBlock.modelData.text
                                                color: dialog.colors.textPrimary
                                                selectionColor: dialog.colors.accent
                                                selectedTextColor: dialog.colors.textOnAccent
                                                font.family: "Consolas"
                                                font.pixelSize: dialog.hostWindow.dp(10)
                                            }
                                        }
                                    }
                                }
                            }

                            // ---- the actions: their own row under the content, never over it ----------------------
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: dialog.hostWindow.dp(2)
                                spacing: dialog.hostWindow.dp(8)

                                Item { Layout.fillWidth: true }

                                RunActionButton {
                                    objectName: "draftDetailsButton"
                                    hostWindow: dialog.hostWindow
                                    colors: dialog.colors
                                    text: draftRow.expanded ? qsTr("Hide details") : qsTr("Show details")
                                    onClicked: draftRow.expanded = !draftRow.expanded
                                }

                                RunActionButton {
                                    objectName: "draftOpenButton"
                                    hostWindow: dialog.hostWindow
                                    colors: dialog.colors
                                    text: qsTr("Open")
                                    primary: true
                                    onClicked: {
                                        dialog.openRequested(draftRow.modelData.draftId)
                                        dialog.close()
                                    }
                                }

                                RunActionButton {
                                    objectName: "draftDeleteButton"
                                    hostWindow: dialog.hostWindow
                                    colors: dialog.colors
                                    danger: true
                                    text: draftRow.deleteConfirming ? qsTr("Confirm?") : qsTr("Delete")
                                    onClicked: {
                                        if (draftRow.deleteConfirming) {
                                            dialog.deleteRequested(draftRow.modelData.draftId)
                                            draftRow.deleteConfirming = false
                                        } else {
                                            draftRow.deleteConfirming = true
                                            draftDeleteResetTimer.restart()
                                        }
                                    }

                                    Timer {
                                        id: draftDeleteResetTimer
                                        interval: 3000
                                        onTriggered: draftRow.deleteConfirming = false
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            RunActionButton {
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Close")
                onClicked: dialog.close()
            }
        }
    }
}
