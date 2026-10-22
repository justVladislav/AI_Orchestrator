pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// PC-3D: the Prompt Workshop dialog. It turns a model-authored `next-run-plan.v1` plan into reviewed Run Drafts.
//
// The Backend parses and validates everything (QML parses nothing and never reads the message text). This dialog
// only RENDERS the Backend's preview - every task and every merged contract in full and selectable - and relays
// the user's explicit choices: an optional roster template, optional source Runs, and the one confirmation. What
// is saved is exactly what the preview showed: the confirmation carries only the opaque preview token. Nothing
// starts: each draft is opened, completed and started by the user in New Run.
//
// ChatsPage owns every Backend call; this dialog gets the results as properties.
//
// PC-5D: the SAME dialog reviews a plan the follow-up composer stored (mode "follow_up"): the Backend's preview map has the same shape
// (its `previewToken` is the request id), the Run the plan follows from is `lockedRunId` - always one of the group's source Runs, shown
// ticked and never removable - and the picker may only ADD further Runs. FollowUpPlanPanel owns those Backend calls.
Dialog {
    id: dialog
    objectName: "nextRunPlanDialog"

    required property var hostWindow
    required property var colors

    // "message": opened from the action on a reply; "import": a manual paste (the paste step comes first); "follow_up": a plan the
    // follow-up composer stored (PC-5D).
    property string mode: "message"
    // follow_up only: the source Run of the request. It is part of the group's origin: shown selected, never removable, not counted
    // among the Runs the user ADDS.
    property string lockedRunId: ""
    // The Backend's preview map (ok, planTitle, shortFingerprint, drafts[], warnings[], existingGroupId, ...) or null.
    property var preview: null
    // [{draftId, name, title, rosterSize, maxParallelism}] and [{runId, title, status, createdAt}] from the Backend.
    property var templates: []
    // Set by the follow-up page only: the paid composer's plan may carry a staffing hint whose parallelism Confirm can copy (an explicit tick).
    property bool offersSuggestedParallelism: false
    property var sourceRuns: []
    // The Backend's creation map after the confirmation, or null. (Not "result": that is a final property of Dialog.)
    property var creation: null
    property string errorText: ""
    // Failures to load the two pickers (strings from the Backend's typed results). Empty when both loaded.
    property var choiceErrors: []
    // True when the failure means "this preview is no longer good" (changed message, expired token): the only way
    // forward is a new preview, which the page can make for the same source.
    property bool offerPreviewAgain: false

    readonly property bool hasPreview: dialog.preview !== null && dialog.preview !== undefined
                                       && dialog.preview.ok === true
    readonly property bool created: dialog.creation !== null && dialog.creation !== undefined
                                    && dialog.creation.ok === true
    readonly property var planDrafts: dialog.hasPreview ? dialog.preview.drafts : []
    readonly property var planWarnings: dialog.hasPreview ? dialog.preview.warnings : []
    readonly property bool hasStaffing: dialog.hasPreview && dialog.preview.hasStaffing === true
    readonly property int staffingMaxWorkers: dialog.hasStaffing ? Number(dialog.preview.staffingMaxWorkers) : 0
    readonly property bool alreadyExists: dialog.hasPreview
                                          && String(dialog.preview.existingGroupId || "").length > 0
    readonly property int maxSourceRuns: 16 - (dialog.lockedRunId.length > 0 ? 1 : 0)
    // Ids of the checked source Runs, newest first (the order of the list).
    property var selectedRunIds: []

    // The template choices, the first being "no template". Presentation only.
    readonly property var templateOptions: {
        const options = [{ "draftId": "", "label": qsTr("Empty roster - choose the models in New Run") }]
        for (let index = 0; index < dialog.templates.length; ++index) {
            const entry = dialog.templates[index]
            options.push({
                "draftId": entry.draftId,
                "label": qsTr("%1 - %2 agents, parallel %3").arg(entry.name).arg(entry.rosterSize).arg(entry.maxParallelism)
            })
        }
        return options
    }

    signal previewRequested(string pastedText)
    signal previewAgainRequested()
    signal reloadChoicesRequested()
    // The one write. The token is the preview's own; the template id is empty for an empty roster.
    signal confirmRequested(string previewToken, string templateDraftId, var sourceRunIds, bool applySuggestedParallelism)
    signal openDraftsRequested()

    function titleForKey(key) {
        for (let index = 0; index < dialog.planDrafts.length; ++index) {
            if (dialog.planDrafts[index].key === key)
                return dialog.planDrafts[index].title
        }
        return String(key)
    }

    function setRunSelected(runId, selected) {
        const next = []
        for (let index = 0; index < dialog.sourceRuns.length; ++index) {
            const id = dialog.sourceRuns[index].runId
            const was = dialog.selectedRunIds.indexOf(id) >= 0
            if (id === runId ? selected : was)
                next.push(id)
        }
        dialog.selectedRunIds = next
    }

    // The worker count of the chosen template roster (the Backend's number), or -1 when no template is chosen.
    function chosenTemplateWorkerCount() {
        const chosen = String(templateCombo.currentValue || "")
        if (chosen.length === 0)
            return -1
        for (let index = 0; index < dialog.templates.length; ++index) {
            if (dialog.templates[index].draftId === chosen)
                return Number(dialog.templates[index].workerCount)
        }
        return -1
    }

    function confirm() {
        if (!dialog.hasPreview || dialog.created)
            return
        dialog.confirmRequested(String(dialog.preview.previewToken),
                                String(templateCombo.currentValue || ""),
                                dialog.selectedRunIds,
                                dialog.offersSuggestedParallelism && dialog.hasStaffing && applyParallelismCheck.checked)
    }

    // Everything the page has put here is dropped; the paste field is cleared with it.
    function reset() {
        dialog.preview = null
        dialog.creation = null
        dialog.errorText = ""
        dialog.offerPreviewAgain = false
        dialog.choiceErrors = []
        dialog.templates = []
        dialog.sourceRuns = []
        dialog.selectedRunIds = []
        applyParallelismCheck.checked = false
        dialog.lockedRunId = ""
        pasteField.text = ""
    }

    // The window's overlay, not the host's parent: the follow-up review opens this dialog from a small panel inside Run Details.
    anchors.centerIn: Overlay.overlay
    width: Math.min(hostWindow.dp(780),
                    Math.max(hostWindow.dp(360), hostWindow.width - hostWindow.dp(32)))
    height: Math.min(hostWindow.dp(720), hostWindow.height - hostWindow.dp(32),
                     Math.max(hostWindow.dp(340), body.implicitHeight + hostWindow.dp(150)))
    modal: true
    closePolicy: Popup.CloseOnEscape
    title: dialog.mode === "import" ? qsTr("Import next-run plan")
           : dialog.mode === "follow_up" ? qsTr("Review the follow-up plan")
           : qsTr("Create Run drafts from a plan")
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(13)

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

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(10)

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            ColumnLayout {
                id: body
                width: scroller.width - dialog.hostWindow.dp(12)
                spacing: dialog.hostWindow.dp(10)

                // The standing promise of this dialog.
                Text {
                    Layout.fillWidth: true
                    objectName: "nextRunPlanNotice"
                    wrapMode: Text.WordWrap
                    text: qsTr("Nothing starts from here. Creating drafts only saves them: each one opens in New Run, where you choose the models, check the task and start it yourself.")
                    color: dialog.colors.textSecondary
                    font.pixelSize: dialog.hostWindow.dp(11)
                }

                // ---- manual paste (Import next-run plan) -------------------------------------------------------------
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: dialog.mode === "import" && !dialog.created
                    spacing: dialog.hostWindow.dp(6)

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Paste a plan: one block fenced as next-run-plan.v1, or the bare JSON document. The Backend reads it; nothing is saved until you confirm the preview.")
                        color: dialog.colors.textSecondary
                        font.pixelSize: dialog.hostWindow.dp(11)
                    }

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: dialog.hostWindow.dp(150)
                        TextArea {
                            id: pasteField
                            objectName: "nextRunPlanPasteField"
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            placeholderText: qsTr("Paste the plan here")
                            color: dialog.colors.textPrimary
                            font.pixelSize: dialog.hostWindow.dp(11)
                            background: Rectangle {
                                radius: dialog.colors.cornerRadiusSmall
                                color: dialog.colors.inputBackground
                                border.width: 1
                                border.color: pasteField.activeFocus ? dialog.colors.inputBorderFocused : dialog.colors.inputBorder
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        ChatActionButton {
                            objectName: "nextRunPlanPreviewButton"
                            hostWindow: dialog.hostWindow
                            colors: dialog.colors
                            text: qsTr("Preview")
                            primary: true
                            enabled: pasteField.text.trim().length > 0
                            onClicked: dialog.previewRequested(pasteField.text)
                        }
                    }
                }

                // ---- failure -------------------------------------------------------------------------------------------
                Rectangle {
                    Layout.fillWidth: true
                    visible: dialog.errorText.length > 0
                    implicitHeight: errorColumn.implicitHeight + dialog.hostWindow.dp(16)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.errorSoft
                    border.width: 1
                    border.color: dialog.colors.error

                    ColumnLayout {
                        id: errorColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: dialog.hostWindow.dp(10)
                        spacing: dialog.hostWindow.dp(6)

                        Text {
                            objectName: "nextRunPlanError"
                            Layout.fillWidth: true
                            text: dialog.errorText
                            color: dialog.colors.error
                            wrapMode: Text.WordWrap
                            font.pixelSize: dialog.hostWindow.dp(11)
                        }

                        ChatActionButton {
                            objectName: "nextRunPlanPreviewAgainButton"
                            visible: dialog.offerPreviewAgain
                            hostWindow: dialog.hostWindow
                            colors: dialog.colors
                            text: qsTr("Preview again")
                            onClicked: dialog.previewAgainRequested()
                        }
                    }
                }

                // ---- the preview -----------------------------------------------------------------------------------------
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: dialog.hasPreview && !dialog.created
                    spacing: dialog.hostWindow.dp(10)

                    Text {
                        objectName: "nextRunPlanSummary"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: dialog.hasPreview
                              ? qsTr("Plan “%1” - %2 draft(s) - %3 - fingerprint %4")
                                    .arg(dialog.preview.planTitle)
                                    .arg(dialog.planDrafts.length)
                                    .arg(dialog.preview.origin === "chat_message" ? qsTr("from a chat reply")
                                         : dialog.preview.origin === "follow_up_request" ? qsTr("composed by the follow-up planner")
                                         : qsTr("pasted"))
                                    .arg(dialog.preview.shortFingerprint)
                              : ""
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(13)
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: dialog.alreadyExists
                        implicitHeight: existingText.implicitHeight + dialog.hostWindow.dp(14)
                        radius: dialog.colors.cornerRadiusSmall
                        color: dialog.colors.warningSoft
                        border.width: 1
                        border.color: dialog.colors.warning

                        Text {
                            id: existingText
                            objectName: "nextRunPlanExistingNotice"
                            anchors.fill: parent
                            anchors.margins: dialog.hostWindow.dp(7)
                            wrapMode: Text.WordWrap
                            text: qsTr("These drafts were already created from this source. Confirming returns them and writes nothing new.")
                            color: dialog.colors.warning
                            font.pixelSize: dialog.hostWindow.dp(11)
                        }
                    }

                    Repeater {
                        model: dialog.planWarnings
                        delegate: Text {
                            required property var modelData
                            objectName: "nextRunPlanWarning"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Note: %1").arg(modelData.message)
                            color: dialog.colors.warning
                            font.pixelSize: dialog.hostWindow.dp(11)
                        }
                    }

                    // One card per draft: its task and merged contracts, word for word.
                    Repeater {
                        model: dialog.planDrafts
                        delegate: Rectangle {
                            id: card
                            required property var modelData
                            required property int index
                            objectName: "nextRunPlanDraftCard"
                            Layout.fillWidth: true
                            implicitHeight: cardColumn.implicitHeight + dialog.hostWindow.dp(20)
                            radius: dialog.colors.cornerRadiusSmall
                            color: dialog.colors.surface
                            border.width: 1
                            border.color: dialog.colors.border

                            ColumnLayout {
                                id: cardColumn
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: dialog.hostWindow.dp(10)
                                spacing: dialog.hostWindow.dp(6)

                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: qsTr("%1. %2").arg(card.index + 1).arg(card.modelData.title)
                                    color: dialog.colors.textPrimary
                                    font.pixelSize: dialog.hostWindow.dp(12.5)
                                    font.weight: Font.DemiBold
                                }

                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: card.modelData.dependsOn.length === 0
                                          ? qsTr("Key %1 - no dependencies").arg(card.modelData.key)
                                          : qsTr("Key %1 - suggested to start after: %2 (advisory, nothing is enforced)")
                                                .arg(card.modelData.key)
                                                .arg(card.modelData.dependsOn.map(key => dialog.titleForKey(key)).join(", "))
                                    color: dialog.colors.textMuted
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                }

                                // The planner's advice on who should work on this draft. An estimate and a suggestion only: it names no
                                // model or cost and nothing is created from it (the real task count is decided during the Run).
                                ColumnLayout {
                                    objectName: "nextRunPlanStaffing"
                                    Layout.fillWidth: true
                                    visible: card.modelData.staffing.present
                                    spacing: dialog.hostWindow.dp(2)
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: qsTr("Suggested staffing (advice and an estimate): %1").arg(card.modelData.staffing.summary)
                                        color: dialog.colors.textSecondary
                                        font.pixelSize: dialog.hostWindow.dp(10.5)
                                        font.weight: Font.Medium
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: qsTr("Why: %1").arg(card.modelData.staffing.reason)
                                        color: dialog.colors.textSecondary
                                        font.pixelSize: dialog.hostWindow.dp(10)
                                    }
                                    Repeater {
                                        model: card.modelData.staffing.workers
                                        delegate: Text {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            textFormat: Text.PlainText
                                            text: qsTr("- %1: %2").arg(modelData.role).arg(modelData.focus)
                                            color: dialog.colors.textMuted
                                            font.pixelSize: dialog.hostWindow.dp(10)
                                        }
                                    }
                                }

                                Text {
                                    text: qsTr("Task")
                                    color: dialog.colors.textSecondary
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                    font.weight: Font.Medium
                                }

                                // Selectable, read-only, never truncated.
                                TextArea {
                                    objectName: "nextRunPlanTaskText"
                                    Layout.fillWidth: true
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    text: card.modelData.task
                                    color: dialog.colors.textPrimary
                                    font.pixelSize: dialog.hostWindow.dp(11)
                                    background: Rectangle {
                                        radius: dialog.colors.cornerRadiusSmall
                                        color: dialog.colors.inputBackground
                                        border.width: 1
                                        border.color: dialog.colors.border
                                    }
                                }

                                Text {
                                    visible: card.modelData.contracts.length > 0
                                    text: qsTr("Contracts and constraints (%1) - delivered word for word to the workers they apply to")
                                          .arg(card.modelData.contracts.length)
                                    color: dialog.colors.textSecondary
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                    font.pixelSize: dialog.hostWindow.dp(10)
                                    font.weight: Font.Medium
                                }

                                Repeater {
                                    model: card.modelData.contracts
                                    delegate: ColumnLayout {
                                        id: contractBlock
                                        required property var modelData
                                        objectName: "nextRunPlanContract"
                                        Layout.fillWidth: true
                                        spacing: dialog.hostWindow.dp(2)

                                        Text {
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            text: qsTr("%1 - %2 - %3").arg(contractBlock.modelData.id)
                                                  .arg(contractBlock.modelData.kind === "constraint" ? qsTr("constraint") : qsTr("interface contract"))
                                                  .arg(contractBlock.modelData.title)
                                            color: dialog.colors.textPrimary
                                            font.pixelSize: dialog.hostWindow.dp(10.5)
                                            font.weight: Font.Medium
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            text: contractBlock.modelData.appliesToPaths.length === 0
                                                  ? qsTr("Applies to every worker of the Run")
                                                  : qsTr("Applies to: %1").arg(contractBlock.modelData.appliesToPaths.join(", "))
                                            color: dialog.colors.textMuted
                                            font.pixelSize: dialog.hostWindow.dp(10)
                                        }

                                        TextArea {
                                            objectName: "nextRunPlanContractText"
                                            Layout.fillWidth: true
                                            readOnly: true
                                            selectByMouse: true
                                            wrapMode: TextEdit.Wrap
                                            text: contractBlock.modelData.text
                                            color: dialog.colors.textPrimary
                                            font.family: "Consolas"
                                            font.pixelSize: dialog.hostWindow.dp(10.5)
                                            background: Rectangle {
                                                radius: dialog.colors.cornerRadiusSmall
                                                color: dialog.colors.inputBackground
                                                border.width: 1
                                                border.color: dialog.colors.border
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // A picker that could not be loaded is a failure, not an empty list: the Backend's own words, and a
                    // way to try again without losing the preview.
                    Rectangle {
                        Layout.fillWidth: true
                        visible: dialog.choiceErrors.length > 0
                        implicitHeight: choiceErrorColumn.implicitHeight + dialog.hostWindow.dp(16)
                        radius: dialog.colors.cornerRadiusSmall
                        color: dialog.colors.errorSoft
                        border.width: 1
                        border.color: dialog.colors.error

                        ColumnLayout {
                            id: choiceErrorColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: dialog.hostWindow.dp(10)
                            spacing: dialog.hostWindow.dp(6)

                            Repeater {
                                model: dialog.choiceErrors
                                delegate: Text {
                                    required property var modelData
                                    objectName: "nextRunPlanChoiceError"
                                    Layout.fillWidth: true
                                    text: modelData
                                    color: dialog.colors.error
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: dialog.hostWindow.dp(11)
                                }
                            }

                            ChatActionButton {
                                objectName: "nextRunPlanReloadChoicesButton"
                                hostWindow: dialog.hostWindow
                                colors: dialog.colors
                                text: qsTr("Reload the choices")
                                onClicked: dialog.reloadChoicesRequested()
                            }
                        }
                    }

                    // ---- roster template --------------------------------------------------------------------------------
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: dialog.hostWindow.dp(4)

                        Text {
                            text: qsTr("Roster")
                            color: dialog.colors.textPrimary
                            font.pixelSize: dialog.hostWindow.dp(12)
                            font.weight: Font.Medium
                        }

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("A plan never chooses models or cost. Leave the roster empty and choose the models in New Run, or copy the roster, parallelism, provider limits and repair setting of one of your saved drafts for this project. The workspace, objective and contracts of that draft are not copied.")
                            color: dialog.colors.textSecondary
                            font.pixelSize: dialog.hostWindow.dp(10.5)
                        }

                        ComboBox {
                            id: templateCombo
                            objectName: "nextRunPlanTemplateCombo"
                            Layout.fillWidth: true
                            model: dialog.templateOptions
                            textRole: "label"
                            valueRole: "draftId"
                            font.pixelSize: dialog.hostWindow.dp(11)
                        }

                        // The plan's staffing advice next to the chosen roster. Numbers only; no roster is created, trimmed or cloned.
                        Text {
                            objectName: "nextRunPlanStaffingComparison"
                            Layout.fillWidth: true
                            visible: dialog.hasStaffing
                            wrapMode: Text.WordWrap
                            text: dialog.chosenTemplateWorkerCount() < 0
                                  ? qsTr("The plan suggests up to %1 worker(s) for a draft. No roster is chosen: add that many agents in New Run if you agree.")
                                        .arg(dialog.staffingMaxWorkers)
                                  : dialog.chosenTemplateWorkerCount() < dialog.staffingMaxWorkers
                                    ? qsTr("The chosen template has %1 worker(s); the plan suggests up to %2. The roster is not changed: add workers in New Run if you agree.")
                                          .arg(dialog.chosenTemplateWorkerCount()).arg(dialog.staffingMaxWorkers)
                                    : qsTr("The chosen template has %1 worker(s), enough for the plan's suggestion of up to %2.")
                                          .arg(dialog.chosenTemplateWorkerCount()).arg(dialog.staffingMaxWorkers)
                            color: dialog.colors.textSecondary
                            font.pixelSize: dialog.hostWindow.dp(10.5)
                        }

                        CheckBox {
                            id: applyParallelismCheck
                            objectName: "nextRunPlanApplyParallelismCheck"
                            visible: dialog.offersSuggestedParallelism && dialog.hasStaffing
                            text: qsTr("Use the suggested parallelism (only for drafts that carry a suggestion; it replaces the template's or the default value)")
                            font.pixelSize: dialog.hostWindow.dp(10.5)
                        }
                    }

                    // ---- source Runs ---------------------------------------------------------------------------------------
                    ColumnLayout {
                        objectName: "nextRunPlanSourceRunsSection"
                        Layout.fillWidth: true
                        visible: dialog.sourceRuns.length > 0
                        spacing: dialog.hostWindow.dp(4)

                        Text {
                            text: dialog.lockedRunId.length > 0
                                  ? qsTr("Further source Runs (optional) - %1 of %2").arg(dialog.selectedRunIds.length).arg(dialog.maxSourceRuns)
                                  : qsTr("Source Runs (optional) - %1 of %2").arg(dialog.selectedRunIds.length).arg(dialog.maxSourceRuns)
                            color: dialog.colors.textPrimary
                            font.pixelSize: dialog.hostWindow.dp(12)
                            font.weight: Font.Medium
                        }

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: dialog.lockedRunId.length > 0
                                  ? qsTr("The Run this plan was made from is always recorded and cannot be removed. You may add further earlier Runs of this project; it is a note on the drafts and changes nothing about those Runs.")
                                  : qsTr("Record which earlier Runs of this project this plan follows from. It is a note on the drafts; it changes nothing about those Runs.")
                            color: dialog.colors.textSecondary
                            font.pixelSize: dialog.hostWindow.dp(10.5)
                        }

                        Repeater {
                            model: dialog.sourceRuns
                            delegate: PlanCheckBox {
                                id: runCheck
                                hostWindow: dialog.hostWindow
                                colors: dialog.colors
                                required property var modelData
                                objectName: "nextRunPlanSourceRunCheck"
                                Layout.fillWidth: true
                                readonly property bool locked: runCheck.modelData.runId === dialog.lockedRunId
                                checked: runCheck.locked || dialog.selectedRunIds.indexOf(runCheck.modelData.runId) >= 0
                                enabled: !runCheck.locked && (checked || dialog.selectedRunIds.length < dialog.maxSourceRuns)
                                text: qsTr("%1 - %2 - %3").arg(runCheck.modelData.title)
                                      .arg(runCheck.modelData.status)
                                      .arg(Qt.formatDateTime(runCheck.modelData.createdAt, "yyyy-MM-dd HH:mm"))
                                onToggled: dialog.setRunSelected(runCheck.modelData.runId, checked)
                            }
                        }
                    }
                }

                // ---- the result ----------------------------------------------------------------------------------------------
                ColumnLayout {
                    objectName: "nextRunPlanResult"
                    Layout.fillWidth: true
                    visible: dialog.created
                    spacing: dialog.hostWindow.dp(6)

                    Text {
                        objectName: "nextRunPlanResultSummary"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: dialog.created
                              ? (dialog.creation.alreadyExisted
                                 ? qsTr("These %1 draft(s) already existed - nothing new was saved.").arg(dialog.creation.drafts.length)
                                 : qsTr("Saved %1 draft(s) from “%2”.").arg(dialog.creation.drafts.length).arg(dialog.creation.planTitle))
                              : ""
                        color: dialog.colors.success
                        font.pixelSize: dialog.hostWindow.dp(13)
                        font.weight: Font.DemiBold
                    }

                    Repeater {
                        model: dialog.created ? dialog.creation.drafts : []
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("%1. %2 - %3 contract(s), roster of %4").arg(modelData.ordinal + 1)
                                  .arg(modelData.title).arg(modelData.contractCount).arg(modelData.rosterSize)
                            color: dialog.colors.textPrimary
                            font.pixelSize: dialog.hostWindow.dp(11.5)
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Open Runs, then Drafts, and use Open on a draft to complete it in New Run.")
                        color: dialog.colors.textSecondary
                        font.pixelSize: dialog.hostWindow.dp(11)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)

            Item { Layout.fillWidth: true }

            ChatActionButton {
                objectName: "nextRunPlanCloseButton"
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: dialog.created ? qsTr("Close") : qsTr("Cancel")
                onClicked: dialog.close()
            }

            ChatActionButton {
                objectName: "nextRunPlanConfirmButton"
                visible: !dialog.created
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                primary: true
                enabled: dialog.hasPreview
                text: dialog.alreadyExists
                      ? qsTr("Show the existing drafts")
                      : qsTr("Create %1 draft(s)").arg(dialog.planDrafts.length)
                onClicked: dialog.confirm()
            }

            ChatActionButton {
                objectName: "nextRunPlanOpenDraftsButton"
                visible: dialog.created
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                primary: true
                text: qsTr("Open Drafts")
                onClicked: {
                    dialog.openDraftsRequested()
                    dialog.close()
                }
            }
        }
    }
}
