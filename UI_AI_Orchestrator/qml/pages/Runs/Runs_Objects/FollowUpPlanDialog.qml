pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// PC-5D: the dialog of the opt-in PAID next-Run composer (`Plan next Run...` / `Plan recovery...`). Two stages in one dialog:
//   1. the REQUEST: the goal, how many drafts, the split hint, the model, an optional cap and the files whose content may be sent;
//   2. the QUOTE: what would be sent, to whom, the tokens and the WORST-CASE cost, and the one paid action - `Authorize and send`.
//
// Everything is the Backend's: the candidate files and what happens to each, the model picker with its reasons, the quote and every
// refusal arrive as properties from FollowUpPlanPanel (which owns the Backend calls); this file only renders them and relays the
// user's explicit choices. It parses and validates nothing, classifies nothing, and keeps no money rule: the cost shown is the
// Backend's worst case in micro-dollars, formatted. Nothing is sent by opening, filling in or quoting; only `Authorize and send` asks
// the Backend to authorise, and even then nothing starts and no Drafts exist until the user has reviewed the stored plan and confirmed.
Dialog {
    id: dialog
    objectName: "followUpPlanDialog"

    required property var hostWindow
    required property var colors

    // "next_run" | "recovery": the Backend's vocabulary, set by the panel.
    property string kind: "next_run"
    property string projectId: ""
    property string sourceRunId: ""
    // The Backend's model picker: [{providerId, providerName, providerKind, modelId, displayName, usable, reasonCode, ...}].
    property var models: []
    property string modelError: ""
    // The Backend's candidate list: [{path, safety, disposition, eligible, includedByDefault, sha256Short, originalBytes, shownBytes,
    // estimatedTokens, truncated}].
    property var files: []
    property bool filesLoading: false
    property string filesError: ""
    // The Backend's request map of the QUOTED request, or null while the form is shown.
    property var quote: null
    property string errorText: ""
    // True while a Backend operation of the panel is running (one at a time).
    property bool busy: false
    property string busyOperation: ""
    // The candidate files the user ticked, by path.
    property var checkedPaths: []
    // Non-null only while a saved quote is being returned to the editable form. After the current candidate list arrives, keep the
    // paths that quote actually contained instead of silently selecting a new default set.
    property var pathsToRestore: null

    readonly property bool hasQuote: dialog.quote !== null && dialog.quote !== undefined
                                     && String(dialog.quote.state || "") === "quoted"
    readonly property bool isRecovery: dialog.kind === "recovery"
    readonly property var quoteDetails: dialog.hasQuote ? (dialog.quote.quote || ({})) : ({})
    readonly property int maxSentFiles: 8

    // The model picker's entries, with the Backend's reason in words for one that is not offered. Presentation only.
    readonly property var modelOptions: {
        const options = []
        for (let index = 0; index < dialog.models.length; ++index) {
            const entry = dialog.models[index]
            options.push({
                "providerId": String(entry.providerId),
                "modelId": String(entry.modelId),
                "usable": entry.usable === true,
                "label": entry.usable === true
                         ? qsTr("%1 / %2").arg(entry.providerName || entry.providerId).arg(entry.displayName || entry.modelId)
                         : qsTr("%1 / %2 - not available: %3").arg(entry.providerName || entry.providerId)
                               .arg(entry.displayName || entry.modelId).arg(dialog.modelReasonText(String(entry.reasonCode)))
            })
        }
        return options
    }
    readonly property var chosenModel: modelCombo.currentIndex >= 0 && modelCombo.currentIndex < dialog.modelOptions.length
                                       ? dialog.modelOptions[modelCombo.currentIndex] : null

    signal describeRequested()
    // The intent of the quote, exactly what the user chose; the Backend validates every field.
    signal quoteRequested(var intent)
    signal authorizeRequested(string requestId)
    // The quote itself stays durable until its replacement is stored. The panel reloads the model/file choices and then calls
    // restoreForEditing(), so a quote reopened after a restart is just as editable as one made in this dialog session.
    signal changeRequested(var request)

    function modelReasonText(code) {
        switch (code) {
        case "provider_disabled": return qsTr("its provider is disabled")
        case "model_disabled": return qsTr("the model is disabled")
        case "price_unknown": return qsTr("no price is entered for it, so its cost cannot be quoted")
        case "output_limit_too_small": return qsTr("its output limit is too small for a plan")
        default: return code
        }
    }

    function safetyText(safety) {
        switch (safety) {
        case "unsafe_path": return qsTr("not sent: the path is not allowed")
        case "symlink": return qsTr("not sent: it is a link")
        case "binary": return qsTr("not sent: it is not text")
        case "invalid_utf8": return qsTr("not sent: it is not valid UTF-8 text")
        case "secret_detected": return qsTr("not sent: it looks like it holds a secret")
        case "too_large": return qsTr("not sent: it is too large")
        case "read_failed": return qsTr("not sent: it could not be read")
        default: return safety
        }
    }

    function usd(micros) {
        return "$" + (Number(micros || 0) / 1000000).toFixed(4)
    }

    function setPathChecked(path, checked) {
        const next = []
        for (let index = 0; index < dialog.files.length; ++index) {
            const id = dialog.files[index].path
            const was = dialog.checkedPaths.indexOf(id) >= 0
            if (id === path ? checked : was)
                next.push(id)
        }
        dialog.checkedPaths = next
    }

    // The cap as typed (US dollars) -> micro-dollars. A text that is not a number is passed as it is: the controller refuses it by name.
    function capValue() {
        const text = capField.text.trim()
        if (text.length === 0)
            return undefined
        const number = Number(text)
        return isFinite(number) ? Math.round(number * 1000000) : text
    }

    function buildIntent() {
        const intent = {
            "projectId": dialog.projectId,
            "sourceRunId": dialog.sourceRunId,
            "kind": dialog.kind,
            "goal": goalField.text,
            "maxDrafts": draftsBox.value,
            "splitHint": String(splitCombo.currentValue),
            "providerId": dialog.chosenModel ? dialog.chosenModel.providerId : "",
            "modelId": dialog.chosenModel ? dialog.chosenModel.modelId : "",
            "selectedPaths": dialog.checkedPaths
        }
        const cap = dialog.capValue()
        if (cap !== undefined)
            intent.capMicros = cap
        return intent
    }

    function requestQuote() {
        if (dialog.busy)
            return
        dialog.errorText = ""
        dialog.quoteRequested(dialog.buildIntent())
    }

    function authorizeNow() {
        if (!dialog.hasQuote || dialog.busy)
            return
        dialog.errorText = ""
        dialog.authorizeRequested(String(dialog.quote.id))
    }

    function changeRequest() {
        if (!dialog.hasQuote || dialog.busy)
            return
        dialog.changeRequested(dialog.quote)
    }

    // The panel calls this after it put the Backend's picker here: the first model the Backend offers is the default.
    function selectFirstUsableModel() {
        for (let index = 0; index < dialog.modelOptions.length; ++index) {
            if (dialog.modelOptions[index].usable) {
                modelCombo.currentIndex = index
                return
            }
        }
        modelCombo.currentIndex = dialog.modelOptions.length > 0 ? 0 : -1
    }

    function selectModel(providerId, modelId) {
        for (let index = 0; index < dialog.modelOptions.length; ++index) {
            const option = dialog.modelOptions[index]
            if (option.providerId === providerId && option.modelId === modelId) {
                modelCombo.currentIndex = index
                return
            }
        }
        modelCombo.currentIndex = -1
    }

    // Restores every editable value that was sealed into the stored quote. Candidate metadata is deliberately not trusted from the
    // old dialog state: the panel describes the files again, and pathsToRestore is intersected with that current list when it arrives.
    function restoreForEditing(request) {
        const details = request.quote || ({})
        goalField.text = String(request.goal || "")
        draftsBox.value = Number(request.maxDrafts || 1)
        for (let index = 0; index < splitCombo.model.length; ++index) {
            if (String(splitCombo.model[index].value) === String(request.splitHint || "")) {
                splitCombo.currentIndex = index
                break
            }
        }
        dialog.selectModel(String(details.providerId || ""), String(details.modelId || ""))
        capField.text = details.capMicros === undefined || details.capMicros === null
                        ? "" : String(Number(details.capMicros) / 1000000)
        dialog.pathsToRestore = (request.files || []).map(file => String(file.path))
        dialog.quote = null
    }

    // Everything the panel put here is dropped; the typed fields are cleared with it.
    function reset() {
        dialog.quote = null
        dialog.errorText = ""
        dialog.files = []
        dialog.filesError = ""
        dialog.filesLoading = false
        dialog.models = []
        dialog.modelError = ""
        dialog.checkedPaths = []
        dialog.pathsToRestore = null
        goalField.text = ""
        capField.text = ""
        draftsBox.value = 2
        splitCombo.currentIndex = 0
        modelCombo.currentIndex = 0
    }

    anchors.centerIn: Overlay.overlay
    width: Math.min(hostWindow.dp(780),
                    Math.max(hostWindow.dp(360), hostWindow.width - hostWindow.dp(32)))
    height: Math.min(hostWindow.dp(760), hostWindow.height - hostWindow.dp(32),
                     Math.max(hostWindow.dp(340), body.implicitHeight + hostWindow.dp(150)))
    modal: true
    closePolicy: Popup.CloseOnEscape
    title: dialog.hasQuote
           ? (dialog.isRecovery ? qsTr("Recovery plan - quote") : qsTr("Next Run plan - quote"))
           : (dialog.isRecovery ? qsTr("Plan recovery") : qsTr("Plan next Run"))
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
                    objectName: "followUpPlanNotice"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("This asks a paid model to plan the next Run. Nothing is sent until you press Authorize on the quote. Nothing starts from here: Drafts are created only after you have read the plan and confirmed it.")
                    color: dialog.colors.textSecondary
                    font.pixelSize: dialog.hostWindow.dp(11)
                }

                // ---- failure -------------------------------------------------------------------------------------------
                Rectangle {
                    Layout.fillWidth: true
                    visible: dialog.errorText.length > 0
                    implicitHeight: errorLabel.implicitHeight + dialog.hostWindow.dp(16)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.errorSoft
                    border.width: 1
                    border.color: dialog.colors.error

                    Text {
                        id: errorLabel
                        objectName: "followUpPlanError"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: dialog.hostWindow.dp(10)
                        text: dialog.errorText
                        textFormat: Text.PlainText
                        color: dialog.colors.error
                        wrapMode: Text.WordWrap
                        font.pixelSize: dialog.hostWindow.dp(11)
                    }
                }

                // ==== stage 1: the request ============================================================================================
                ColumnLayout {
                    objectName: "followUpRequestStage"
                    Layout.fillWidth: true
                    visible: !dialog.hasQuote
                    spacing: dialog.hostWindow.dp(10)

                    Text {
                        text: dialog.isRecovery ? qsTr("What should the recovery plan achieve?") : qsTr("What should the next Run achieve?")
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        font.weight: Font.Medium
                    }

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: dialog.hostWindow.dp(96)
                        TextArea {
                            id: goalField
                            objectName: "followUpGoalField"
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            placeholderText: qsTr("Describe the goal in a few sentences")
                            color: dialog.colors.textPrimary
                            font.pixelSize: dialog.hostWindow.dp(11)
                            background: Rectangle {
                                radius: dialog.colors.cornerRadiusSmall
                                color: dialog.colors.inputBackground
                                border.width: 1
                                border.color: goalField.activeFocus ? dialog.colors.inputBorderFocused : dialog.colors.inputBorder
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: dialog.hostWindow.dp(12)

                        ColumnLayout {
                            spacing: dialog.hostWindow.dp(3)
                            Text {
                                text: qsTr("At most this many drafts")
                                color: dialog.colors.textSecondary
                                font.pixelSize: dialog.hostWindow.dp(10.5)
                            }
                            SpinBox {
                                id: draftsBox
                                objectName: "followUpMaxDraftsBox"
                                from: 1
                                to: 4
                                value: 2
                                editable: false
                                font.pixelSize: dialog.hostWindow.dp(11)
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: dialog.hostWindow.dp(3)
                            Text {
                                text: qsTr("How many drafts")
                                color: dialog.colors.textSecondary
                                font.pixelSize: dialog.hostWindow.dp(10.5)
                            }
                            ComboBox {
                                id: splitCombo
                                objectName: "followUpSplitCombo"
                                Layout.fillWidth: true
                                textRole: "label"
                                valueRole: "value"
                                model: [
                                    { "value": "auto", "label": qsTr("Let the model decide (up to the limit)") },
                                    { "value": "one", "label": qsTr("Exactly one draft") },
                                    { "value": "many", "label": qsTr("Several drafts (at least two)") }
                                ]
                                font.pixelSize: dialog.hostWindow.dp(11)
                            }
                        }
                    }

                    Text {
                        text: qsTr("Model")
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        font.weight: Font.Medium
                    }
                    Text {
                        objectName: "followUpModelError"
                        Layout.fillWidth: true
                        visible: dialog.modelError.length > 0
                        text: dialog.modelError
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: dialog.colors.error
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }
                    ComboBox {
                        id: modelCombo
                        objectName: "followUpModelCombo"
                        Layout.fillWidth: true
                        model: dialog.modelOptions
                        textRole: "label"
                        font.pixelSize: dialog.hostWindow.dp(11)
                        delegate: ItemDelegate {
                            id: modelRow
                            required property var modelData
                            required property int index
                            width: modelCombo.width
                            enabled: modelRow.modelData.usable
                            text: modelRow.modelData.label
                            font.pixelSize: dialog.hostWindow.dp(11)
                            highlighted: modelCombo.highlightedIndex === modelRow.index
                        }
                    }
                    Text {
                        objectName: "followUpNoModelNotice"
                        Layout.fillWidth: true
                        visible: dialog.modelOptions.length === 0 && dialog.modelError.length === 0
                        text: qsTr("No model is configured. Add a provider and a priced model in Providers first.")
                        wrapMode: Text.WordWrap
                        color: dialog.colors.warning
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }

                    Text {
                        text: qsTr("Spending cap (optional)")
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        font.weight: Font.Medium
                    }
                    TextField {
                        id: capField
                        objectName: "followUpCapField"
                        Layout.preferredWidth: dialog.hostWindow.dp(180)
                        placeholderText: qsTr("US dollars, at most 0.36")
                        selectByMouse: true
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(11)
                        background: Rectangle {
                            radius: dialog.colors.cornerRadiusSmall
                            color: dialog.colors.inputBackground
                            border.width: 1
                            border.color: capField.activeFocus ? dialog.colors.inputBorderFocused : dialog.colors.inputBorder
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Left empty, the system ceiling of $0.36 applies. A lower cap shortens how long the answer may be; a cap that is too low for any answer is refused.")
                        color: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(10)
                    }

                    // ---- the files ----------------------------------------------------------------------------------------------
                    Text {
                        text: qsTr("Files (%1 ticked)").arg(dialog.checkedPaths.length)
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        font.weight: Font.Medium
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("The CONTENT of the ticked files is sent to the provider you chose. They are the files this Run's proposals touched. At most %1 files are sent (the first %1 in path order); a file that cannot be sent safely is listed with the reason and cannot be ticked.").arg(dialog.maxSentFiles)
                        color: dialog.colors.textSecondary
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }
                    Text {
                        objectName: "followUpFilesLoading"
                        visible: dialog.filesLoading
                        text: qsTr("Reading the candidate files...")
                        color: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }
                    Text {
                        objectName: "followUpFilesError"
                        Layout.fillWidth: true
                        visible: dialog.filesError.length > 0
                        text: dialog.filesError
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: dialog.colors.error
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !dialog.filesLoading && dialog.filesError.length === 0 && dialog.files.length === 0
                        text: qsTr("This Run touched no file. The plan will be made from the project summary and the Outcome alone.")
                        wrapMode: Text.WordWrap
                        color: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }
                    Repeater {
                        model: dialog.files
                        delegate: ColumnLayout {
                            id: fileRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 0
                            PlanCheckBox {
                                objectName: "followUpFileCheck"
                                hostWindow: dialog.hostWindow
                                colors: dialog.colors
                                Layout.fillWidth: true
                                checked: dialog.checkedPaths.indexOf(fileRow.modelData.path) >= 0
                                enabled: fileRow.modelData.eligible === true
                                text: fileRow.modelData.eligible === true
                                      ? qsTr("%1 - about %2 tokens%3").arg(fileRow.modelData.path)
                                            .arg(fileRow.modelData.estimatedTokens)
                                            .arg(fileRow.modelData.truncated === true ? qsTr(" (cut to fit)") : "")
                                      : fileRow.modelData.path
                                onToggled: dialog.setPathChecked(fileRow.modelData.path, checked)
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: dialog.hostWindow.dp(30)
                                visible: fileRow.modelData.eligible !== true
                                text: dialog.safetyText(String(fileRow.modelData.safety))
                                wrapMode: Text.WordWrap
                                color: dialog.colors.textMuted
                                font.pixelSize: dialog.hostWindow.dp(10)
                            }
                        }
                    }
                }

                // ==== stage 2: the quote ===============================================================================================
                ColumnLayout {
                    objectName: "followUpQuoteStage"
                    Layout.fillWidth: true
                    visible: dialog.hasQuote
                    spacing: dialog.hostWindow.dp(8)

                    Text {
                        objectName: "followUpQuoteSummary"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: dialog.hasQuote
                              ? qsTr("Quote - nothing has been sent. At most %1 for one answer of up to %2 tokens, from about %3 tokens of context.")
                                    .arg(dialog.usd(dialog.quoteDetails.worstCaseCostMicros))
                                    .arg(dialog.quoteDetails.maxOutputTokens)
                                    .arg(dialog.quoteDetails.estimatedInputTokens)
                              : ""
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(13)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "followUpQuoteRecipient"
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                        textFormat: Text.PlainText
                        text: dialog.hasQuote
                              ? qsTr("Recipient: %1 (%2) - model %3. Your cap: %4. Valid until %5.")
                                    .arg(String(dialog.quoteDetails.providerKind))
                                    .arg(String(dialog.quoteDetails.baseUrl))
                                    .arg(String(dialog.quoteDetails.apiModelId))
                                    .arg(dialog.usd(dialog.quoteDetails.capMicros))
                                    .arg(Qt.formatDateTime(dialog.quote.quoteExpiresAt, "HH:mm"))
                              : ""
                        color: dialog.colors.textSecondary
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }

                    Text {
                        text: dialog.hasQuote ? qsTr("Files that will be sent (%1)").arg(dialog.quote.files.length) : ""
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        font.weight: Font.Medium
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: dialog.hasQuote && dialog.quote.files.length === 0
                        wrapMode: Text.WordWrap
                        text: qsTr("No file content is sent: only the project summary and the Outcome of the Run.")
                        color: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(10.5)
                    }
                    Repeater {
                        model: dialog.hasQuote ? dialog.quote.files : []
                        delegate: Text {
                            id: pinnedRow
                            required property var modelData
                            objectName: "followUpQuoteFile"
                            Layout.fillWidth: true
                            wrapMode: Text.WrapAnywhere
                            textFormat: Text.PlainText
                            text: qsTr("%1 - %2 bytes, about %3 tokens%4 - sha256 %5")
                                  .arg(pinnedRow.modelData.path).arg(pinnedRow.modelData.originalBytes)
                                  .arg(pinnedRow.modelData.estimatedTokens)
                                  .arg(pinnedRow.modelData.truncated === true ? qsTr(" (cut to fit)") : "")
                                  .arg(pinnedRow.modelData.sha256Short)
                            color: dialog.colors.textSecondary
                            font.pixelSize: dialog.hostWindow.dp(10.5)
                        }
                    }

                    // THE warning of the dialog: what leaves the machine and what it can cost.
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: sendWarning.implicitHeight + dialog.hostWindow.dp(18)
                        radius: dialog.colors.cornerRadiusSmall
                        color: dialog.colors.warningSoft
                        border.width: 1
                        border.color: dialog.colors.warning

                        Text {
                            id: sendWarning
                            objectName: "followUpSendWarning"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: dialog.hostWindow.dp(9)
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            text: dialog.hasQuote
                                  ? qsTr("Authorizing sends the CONTENT of the %1 file(s) above, a summary of the project and the Outcome of this Run to %2 and can cost up to %3. If the call is stopped or fails after it was made, it may still be charged. Nothing starts and no Drafts are created: you will read the plan first, and Drafts exist only after you confirm it.")
                                        .arg(dialog.quote.files.length)
                                        .arg(String(dialog.quoteDetails.providerKind) + " " + String(dialog.quoteDetails.baseUrl))
                                        .arg(dialog.usd(dialog.quoteDetails.worstCaseCostMicros))
                                  : ""
                            color: dialog.colors.warning
                            font.pixelSize: dialog.hostWindow.dp(11)
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)

            Text {
                Layout.fillWidth: true
                visible: dialog.busy
                text: dialog.busyOperation === "authorize" ? qsTr("Authorizing...")
                      : dialog.busyOperation === "requestQuote" ? qsTr("Getting the quote...")
                      : qsTr("Working...")
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(10.5)
            }
            Item { Layout.fillWidth: true; visible: !dialog.busy }

            RunActionButton {
                objectName: "followUpCloseButton"
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Close")
                onClicked: dialog.close()
            }

            RunActionButton {
                objectName: "followUpChangeButton"
                visible: dialog.hasQuote
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Change the request")
                enabled: !dialog.busy
                onClicked: dialog.changeRequest()
            }

            RunActionButton {
                objectName: "followUpGetQuoteButton"
                visible: !dialog.hasQuote
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                primary: true
                text: qsTr("Get quote")
                enabled: !dialog.busy && !dialog.filesLoading && goalField.text.trim().length > 0
                         && dialog.chosenModel !== null && dialog.chosenModel.usable === true
                onClicked: dialog.requestQuote()
            }

            RunActionButton {
                objectName: "followUpAuthorizeButton"
                visible: dialog.hasQuote
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                primary: true
                text: dialog.hasQuote ? qsTr("Authorize and send (up to %1)").arg(dialog.usd(dialog.quoteDetails.worstCaseCostMicros)) : qsTr("Authorize and send")
                enabled: !dialog.busy
                onClicked: dialog.authorizeNow()
            }
        }
    }
}
