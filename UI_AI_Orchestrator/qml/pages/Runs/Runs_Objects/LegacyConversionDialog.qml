pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// AG-17.7C-2: the conversion mode of Edit & rerun for a LEGACY roster Run.
//
// A legacy roster Run (the fixed-stage runner) can be re-launched on the Manifest workflow, but the
// conversion deliberately does not honour some of its settings. The Backend decides everything:
// whether a conversion exists, the LOCKED target roster, the list of settings that are dropped (an
// instructions entry is a LENGTH, never text) and an OPAQUE confirmation fingerprint bound to this
// source Run and this exact decision. This dialog only RENDERS that preview and lets the user edit
// the few fields that are theirs - title, task, workspace, parallelism and the two run policies.
//
// What it must never do: build or edit a roster, pick a workflow, name a parent Run, or assert on
// the user's behalf that a decision was made. Pressing the button hands the Backend the preview's
// fingerprint unchanged; the Backend recomputes the plan from the immutable source Run and refuses
// a mismatch.
Dialog {
    id: dialog
    objectName: "legacyConversionDialog"

    required property var hostWindow
    required property var colors
    // RunController.rerunDraft().conversion / RunController.legacyConversionPreview(): a Backend
    // map. Replaced (not edited) when the Backend reports a stale confirmation.
    property var preview: null
    // Shown until the next attempt so a rejected command never closes the dialog.
    property string errorText: ""
    // Empty keeps the source Run's own workspace; non-empty is a folder picked via Browse….
    property string workspaceOverridePath: ""

    readonly property bool hasPreview: dialog.preview !== null && dialog.preview !== undefined
                                       && String(dialog.preview.sourceRunId || "").length > 0
    // The Backend alone says whether this Run can be converted.
    readonly property bool convertible: dialog.hasPreview && Boolean(dialog.preview.available)
                                        && String(dialog.preview.confirmationFingerprint || "").length > 0
    readonly property var droppedSettings: dialog.hasPreview ? (dialog.preview.dropped || []) : []
    readonly property var targetRoster: dialog.hasPreview ? (dialog.preview.roster || []) : []

    // The only thing that leaves this dialog. `expectedConfirmationFingerprint` is the preview's
    // fingerprint, passed back verbatim - there is no approval flag and no roster or workflow.
    signal convertRequested(string sourceRunId, string title, string task, string workspacePath,
                            int maxParallelism, bool allowEvidenceBackedNoChanges,
                            bool automaticVerificationRepair, string expectedConfirmationFingerprint)

    // Presentation only: the Backend's setting names in words.
    function settingLabel(setting) {
        switch (setting) {
        case "maxAttempts": return qsTr("Attempts per task")
        case "fallback": return qsTr("Provider fallback")
        case "dependencies": return qsTr("Declared dependencies")
        case "instructions": return qsTr("Instructions")
        default: return String(setting)
        }
    }

    function memberLabel(memberId) {
        for (let index = 0; index < dialog.targetRoster.length; ++index) {
            const member = dialog.targetRoster[index]
            if (member.memberId === memberId)
                return member.agentName && member.agentName.length > 0 ? member.agentName : member.memberId
        }
        return String(memberId)
    }

    function memberRole(member) {
        if (member.leadArchitect)
            return member.workspaceAccess ? qsTr("Plans and executes") : qsTr("Plans")
        return qsTr("Available worker")
    }

    function submit() {
        if (!dialog.convertible || titleField.text.trim().length === 0
                || taskField.text.trim().length === 0)
            return
        dialog.errorText = ""
        dialog.convertRequested(String(dialog.preview.sourceRunId),
                                titleField.text.trim(), taskField.text.trim(),
                                dialog.workspaceOverridePath,
                                maxParallelismSpin.value,
                                allowNoChangesCheck.checked,
                                automaticVerificationRepairCheck.checked,
                                String(dialog.preview.confirmationFingerprint))
    }

    anchors.centerIn: parent
    width: Math.min(hostWindow.dp(720),
                    Math.max(hostWindow.dp(360),
                             hostWindow.width - hostWindow.dp(32)))
    height: Math.min(hostWindow.dp(700),
                     hostWindow.height - hostWindow.dp(32))
    modal: true
    title: qsTr("Convert to a Manifest Run")
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(13)

    // The fields are initialised once, when the dialog opens. A reloaded preview (after a stale
    // confirmation) replaces only the summary and the fingerprint, never what the user typed.
    onOpened: {
        dialog.errorText = ""
        const defaults = dialog.hasPreview ? (dialog.preview.defaults || {}) : {}
        titleField.text = String(defaults.title || "")
        taskField.text = String(defaults.task || "")
        dialog.workspaceOverridePath = String(defaults.workspacePath || "")
        maxParallelismSpin.value = Math.max(1, Math.min(32, Number(defaults.maxParallelism) || 4))
        allowNoChangesCheck.checked = Boolean(defaults.allowEvidenceBackedNoChanges || false)
        automaticVerificationRepairCheck.checked = defaults.automaticVerificationRepair === undefined
                ? true : Boolean(defaults.automaticVerificationRepair)
        titleField.forceActiveFocus()
    }

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(50)
        color: dialog.colors.surfaceAlternative

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(18)
            text: dialog.title
            color: dialog.colors.textPrimary
            elide: Text.ElideRight
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

    contentItem: ScrollView {
        id: formScroll
        implicitHeight: Math.min(formColumn.implicitHeight,
                                 Math.max(dialog.hostWindow.dp(300),
                                          dialog.hostWindow.height - dialog.hostWindow.dp(150)))
        contentWidth: availableWidth
        contentHeight: formColumn.implicitHeight
        clip: true
        rightPadding: dialog.hostWindow.dp(12)
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: formColumn
            width: formScroll.availableWidth
            spacing: dialog.hostWindow.dp(12)

            // ── What this is, in the Backend's own words ──────────────────────────────
            Text {
                objectName: "conversionIntro"
                Layout.fillWidth: true
                text: qsTr("This Run used the legacy fixed-stage roster. Converting it creates a NEW Run on the Manifest workflow; the original Run is left exactly as it is.")
                color: dialog.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(13)
            }

            Text {
                objectName: "conversionReason"
                Layout.fillWidth: true
                visible: dialog.hasPreview && String(dialog.preview.reasonMessage || "").length > 0
                text: dialog.hasPreview ? String(dialog.preview.reasonMessage || "") : ""
                textFormat: Text.PlainText
                color: dialog.convertible ? dialog.colors.textPrimary : dialog.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(12)
            }

            Text {
                objectName: "conversionNotAvailable"
                Layout.fillWidth: true
                visible: dialog.hasPreview && !dialog.convertible
                text: qsTr("This Run cannot be converted, and the legacy runner cannot execute it either. Nothing will be created from here; start a new Run instead.")
                color: dialog.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(12)
            }

            RowLayout {
                Layout.fillWidth: true
                visible: dialog.hasPreview
                spacing: dialog.hostWindow.dp(8)
                Text {
                    objectName: "conversionWorkflows"
                    Layout.fillWidth: true
                    text: dialog.hasPreview
                          ? qsTr("Workflow: %1  →  %2").arg(String(dialog.preview.sourceWorkflowId || ""))
                                .arg(String(dialog.preview.targetWorkflowId || ""))
                          : ""
                    textFormat: Text.PlainText
                    color: dialog.colors.textMuted
                    elide: Text.ElideRight
                    font.pixelSize: dialog.hostWindow.dp(10)
                }
            }

            Text {
                Layout.fillWidth: true
                visible: dialog.convertible
                text: qsTr("A Manifest Run works differently: the lead plans a frozen task list, the roster is a pool of available workers (a worker may get no task), every task gets one attempt with no provider fallback, and the verification gate applies.")
                color: dialog.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(10)
            }

            // ── The target roster: shown, never editable ─────────────────────────────
            Text {
                visible: dialog.convertible
                text: qsTr("Roster (locked)")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.Medium
            }

            Repeater {
                id: rosterRepeater
                objectName: "conversionRoster"
                model: dialog.convertible ? dialog.targetRoster : []
                delegate: Rectangle {
                    id: rosterRow
                    required property var modelData
                    objectName: "conversionRosterRow"
                    Layout.fillWidth: true
                    Layout.preferredHeight: rosterColumn.implicitHeight + dialog.hostWindow.dp(14)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.surface
                    border.width: 1
                    border.color: dialog.colors.border

                    ColumnLayout {
                        id: rosterColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: dialog.hostWindow.dp(10)
                        anchors.rightMargin: dialog.hostWindow.dp(10)
                        spacing: dialog.hostWindow.dp(2)

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1  —  %2").arg(dialog.memberLabel(rosterRow.modelData.memberId))
                                  .arg(dialog.memberRole(rosterRow.modelData))
                            textFormat: Text.PlainText
                            color: dialog.colors.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: dialog.hostWindow.dp(12)
                            font.weight: Font.Medium
                        }
                        Text {
                            Layout.fillWidth: true
                            text: String(rosterRow.modelData.modelLabel || "")
                            textFormat: Text.PlainText
                            color: dialog.colors.textSecondary
                            elide: Text.ElideMiddle
                            font.pixelSize: dialog.hostWindow.dp(10)
                        }
                    }
                }
            }

            // ── What the conversion does NOT carry over ──────────────────────────────
            Text {
                visible: dialog.convertible
                text: dialog.droppedSettings.length > 0
                      ? qsTr("Settings that will not be carried over (%1)").arg(dialog.droppedSettings.length)
                      : qsTr("Nothing is dropped: every setting of this Run is carried over.")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.Medium
            }

            Rectangle {
                objectName: "conversionInstructionsWarning"
                Layout.fillWidth: true
                visible: dialog.convertible && dialog.hasPreview && Boolean(dialog.preview.hasDroppedInstructions)
                Layout.preferredHeight: warningText.implicitHeight + dialog.hostWindow.dp(16)
                radius: dialog.colors.cornerRadiusSmall
                color: dialog.colors.warningSoft
                border.width: 1
                border.color: dialog.colors.warning

                Text {
                    id: warningText
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: dialog.hostWindow.dp(10)
                    anchors.rightMargin: dialog.hostWindow.dp(10)
                    text: qsTr("The lead's instructions will NOT be used: the Manifest Planner does not read them. Only their length is shown below; the text itself stays in the original Run.")
                    color: dialog.colors.textPrimary
                    wrapMode: Text.WordWrap
                    font.pixelSize: dialog.hostWindow.dp(11)
                }
            }

            Repeater {
                id: droppedRepeater
                objectName: "conversionDropped"
                model: dialog.convertible ? dialog.droppedSettings : []
                delegate: ColumnLayout {
                    id: droppedRow
                    required property var modelData
                    objectName: "conversionDroppedRow"
                    Layout.fillWidth: true
                    spacing: dialog.hostWindow.dp(1)

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("%1 · %2: %3").arg(dialog.memberLabel(droppedRow.modelData.memberId))
                              .arg(dialog.settingLabel(droppedRow.modelData.setting))
                              .arg(String(droppedRow.modelData.legacyValue || ""))
                        textFormat: Text.PlainText
                        color: dialog.colors.textPrimary
                        wrapMode: Text.WrapAnywhere
                        font.pixelSize: dialog.hostWindow.dp(11)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: String(droppedRow.modelData.reason || "")
                        textFormat: Text.PlainText
                        color: dialog.colors.textMuted
                        wrapMode: Text.WordWrap
                        font.pixelSize: dialog.hostWindow.dp(10)
                    }
                }
            }

            // ── The fields that are the user's to edit ───────────────────────────────
            Text {
                text: qsTr("Title")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.Medium
            }

            TextField {
                id: titleField
                objectName: "conversionTitleField"
                Layout.fillWidth: true
                Layout.preferredHeight: dialog.hostWindow.dp(38)
                color: dialog.colors.textPrimary
                placeholderTextColor: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(13)
                selectByMouse: true
                verticalAlignment: TextInput.AlignVCenter
                background: Rectangle {
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: titleField.activeFocus ? 1 : 0
                    border.color: dialog.colors.inputBorderFocused
                }
            }

            Text {
                text: qsTr("Task")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.Medium
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: dialog.hostWindow.dp(120)
                radius: dialog.colors.cornerRadiusSmall
                color: dialog.colors.inputBackground
                border.width: taskField.activeFocus ? 1 : 0
                border.color: dialog.colors.inputBorderFocused
                clip: true

                Flickable {
                    id: taskFlickable
                    anchors.fill: parent
                    anchors.margins: dialog.hostWindow.dp(2)
                    clip: true
                    contentWidth: width
                    contentHeight: Math.max(taskField.implicitHeight, height)
                    boundsBehavior: Flickable.StopAtBounds
                    TextArea.flickable: taskField

                    TextArea {
                        id: taskField
                        objectName: "conversionTaskField"
                        width: taskFlickable.width
                        color: dialog.colors.textPrimary
                        placeholderTextColor: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(13)
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        background: null
                    }

                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                    }
                }
            }

            Text {
                text: qsTr("Workspace")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.Medium
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(6)

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: dialog.hostWindow.dp(34)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: 1
                    border.color: dialog.colors.inputBorder

                    Text {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: dialog.hostWindow.dp(10)
                        anchors.rightMargin: dialog.hostWindow.dp(10)
                        text: dialog.workspaceOverridePath.length > 0
                              ? dialog.workspaceOverridePath : qsTr("The Project's own folder")
                        textFormat: Text.PlainText
                        color: dialog.workspaceOverridePath.length > 0
                               ? dialog.colors.textPrimary : dialog.colors.textSecondary
                        elide: Text.ElideMiddle
                        font.pixelSize: dialog.hostWindow.dp(10)
                    }
                }

                RunActionButton {
                    text: qsTr("Browse…")
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    onClicked: workspaceFolderDialog.open()
                }
            }

            FolderDialog {
                id: workspaceFolderDialog
                title: qsTr("Select workspace folder for this run")

                onAccepted: {
                    // selectedFolder is a file:// URL; the Backend expects a plain local path
                    // (the same decoding the New Run dialog uses).
                    let path = workspaceFolderDialog.selectedFolder.toString()
                    if (path.startsWith("file://"))
                        path = path.substring(7)
                    if (/^\/[A-Za-z]:\//.test(path))
                        path = path.substring(1)
                    dialog.workspaceOverridePath = decodeURIComponent(path)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(8)
                Text {
                    text: qsTr("Parallel")
                    color: dialog.colors.textSecondary
                    font.pixelSize: dialog.hostWindow.dp(10)
                }
                SpinBox {
                    id: maxParallelismSpin
                    objectName: "conversionParallelismSpin"
                    from: 1
                    to: 32
                    value: 4
                    editable: true
                }
                Item { Layout.fillWidth: true }
            }

            CheckBox {
                id: allowNoChangesCheck
                objectName: "conversionAllowNoChangesCheck"
                Layout.fillWidth: true
                text: qsTr("Audit run: allow evidence-backed no changes")
                checked: false
                font.pixelSize: dialog.hostWindow.dp(10)
            }

            CheckBox {
                id: automaticVerificationRepairCheck
                objectName: "conversionAutomaticVerificationRepairCheck"
                Layout.fillWidth: true
                text: qsTr("Automatically repair failed verification once")
                checked: true
                font.pixelSize: dialog.hostWindow.dp(10)
            }

            Text {
                objectName: "conversionError"
                Layout.fillWidth: true
                visible: dialog.errorText.length > 0
                text: dialog.errorText
                textFormat: Text.PlainText
                color: dialog.colors.error
                wrapMode: Text.WordWrap
                font.pixelSize: dialog.hostWindow.dp(12)
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: dialog.hostWindow.dp(4)
                spacing: dialog.hostWindow.dp(8)

                Item { Layout.fillWidth: true }

                RunActionButton {
                    objectName: "conversionCancelButton"
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    text: qsTr("Cancel")
                    onClicked: dialog.reject()
                }

                // The one explicit act of confirmation. It is enabled only for a convertible
                // Run with a title and a task; nothing is created before it is pressed.
                RunActionButton {
                    objectName: "conversionConvertButton"
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    text: qsTr("Convert and create Run")
                    primary: true
                    enabled: dialog.convertible
                             && titleField.text.trim().length > 0
                             && taskField.text.trim().length > 0
                    onClicked: dialog.submit()
                }
            }
        }
    }
}
