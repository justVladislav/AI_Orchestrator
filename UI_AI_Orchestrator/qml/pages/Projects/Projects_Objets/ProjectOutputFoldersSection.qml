pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// P3e: the selected project's OUTPUT FOLDERS - folders the user explicitly declares to hold generated results, so that their content
// is left out of what the application reads for Runs and project context and proposals cannot write into them. Nothing is deleted and
// no .gitignore is touched. Everything shown is what the Backend (backend.projectOutputApprovals) established; this section never
// reads the file system or the database, never normalises or interprets a path, and never approves anything by itself:
//   * every operation is asynchronous - a request id comes back at once and the result arrives in a signal; one operation at a time;
//   * Approve is only possible for a successful Preview of exactly the text in the field, of this project, that nothing has
//     invalidated; editing the text, changing the project, hiding the page or a root change drops the preview and both confirmations;
//   * a result is applied only to the request that is still the current one - a late or foreign answer changes nothing on screen;
//   * Cancel only ASKS the running inspection to stop (it stays busy until it ends); once the Backend has begun writing, the record
//     is reported as written even if a cancel arrived meanwhile.
ColumnLayout {
    id: section
    objectName: "projectOutputFoldersSection"

    required property var hostWindow
    required property var colors
    required property var backend
    required property string projectId
    // The project's folder as the page knows it; a change of it (same project) drops the local form like a project change does.
    property string projectPath: ""

    readonly property var controller: section.backend ? section.backend.projectOutputApprovals : null
    readonly property bool storageAvailable: section.controller !== null && section.controller.available === true
    readonly property bool controllerBusy: section.controller !== null && section.controller.busy === true
    readonly property bool inspecting: section.controller !== null && section.controller.inspecting === true

    // Two counters decide whether an answer still belongs on screen: formEpoch moves with every edit and every new preview, viewEpoch
    // with a project change, a root change and hiding.
    property int formEpoch: 0
    property int viewEpoch: 0
    // request id (as text) -> {operation, projectId, subject, formEpoch, viewEpoch}; an entry lives until its terminal signal.
    property var inflight: ({})
    property var preview: null
    property double previewRequestId: 0
    property string previewInput: ""
    property bool ackNested: false
    property bool ackIncomplete: false
    property var approvals: []
    property bool listLoaded: false
    property string listError: ""
    property bool pendingRefresh: false
    property var rechecks: ({})
    property string notice: ""
    property string errorLine: ""
    property bool cancelAsked: false

    readonly property bool previewUsable: section.preview !== null && section.preview.success === true
        && section.preview.stale !== true && section.preview.cancelRequested !== true
        && section.previewRequestId > 0 && section.previewInput === pathField.text && section.visible
    readonly property bool needsNested: section.previewUsable && section.preview.requiresNestedConfirmation === true
    readonly property bool needsIncomplete: section.previewUsable && section.preview.requiresIncompleteConfirmation === true
    readonly property bool canApprove: section.storageAvailable && !section.controllerBusy && section.previewUsable
        && section.preview.alreadyApproved !== true
        && (!section.needsNested || section.ackNested) && (!section.needsIncomplete || section.ackIncomplete)
    readonly property bool hasInspectionInflight: {
        for (const key in section.inflight) {
            const operation = section.inflight[key].operation
            if (operation === "preview" || operation === "approve" || operation === "recheck") return true
        }
        return false
    }

    Layout.fillWidth: true
    spacing: hostWindow.dp(8)

    // ---- helpers (display only) -------------------------------------------------------------------------------------------------

    function bytesText(value) {
        const n = Number(value)
        if (n < 1024) return qsTr("%1 B").arg(n)
        if (n < 1024 * 1024) return qsTr("%1 KB").arg((n / 1024).toFixed(1))
        if (n < 1024 * 1024 * 1024) return qsTr("%1 MB").arg((n / (1024 * 1024)).toFixed(1))
        return qsTr("%1 GB").arg((n / (1024 * 1024 * 1024)).toFixed(1))
    }

    function failureText(result) {
        return String(result.errorCode) + ": " + String(result.errorText)
    }

    // Counts of an inspection: exact only when it ran to the end; otherwise they are lower bounds and are said so.
    function countsText(facts) {
        if (facts.exists !== true) return qsTr("The folder does not exist yet, so nothing was inspected.")
        if (facts.inspectionStatus === "complete" && facts.countsAreLowerBounds !== true)
            return qsTr("Inspected completely: %1 entries, %2 files, %3.")
                .arg(facts.entries).arg(facts.files).arg(section.bytesText(facts.bytes))
        if (facts.inspectionStatus === "incomplete" || facts.countsAreLowerBounds === true)
            return qsTr("Inspected only partly - these are lower bounds: at least %1 entries, at least %2 files, at least %3.")
                .arg(facts.entries).arg(facts.files).arg(section.bytesText(facts.bytes))
        return qsTr("Inspection state: %1.").arg(facts.inspectionStatus)
    }

    function reasonsText(facts) {
        const parts = []
        if (facts.hitEntryLimit === true) parts.push(qsTr("It stopped at the entry limit."))
        if (facts.hitDepthLimit === true) parts.push(qsTr("It stopped at the depth limit."))
        if (Number(facts.skippedLinks) > 0)
            parts.push(qsTr("%1 link(s) were not followed - links are never entered.").arg(facts.skippedLinks))
        return parts.join(" ")
    }

    function nestedText(facts) {
        const count = Number(facts.nestedSourceCount)
        if (!(count > 0)) return ""
        const lower = facts.countsAreLowerBounds === true
        return (lower ? qsTr("At least %1 CMakeLists.txt file(s) were found below this folder - they would be hidden too.")
                       : qsTr("%1 CMakeLists.txt file(s) were found below this folder - they would be hidden too."))
            .arg(count)
    }

    function recheckText(result) {
        if (result.success !== true) {
            if (result.errorCode === "output_approval.root_changed")
                return qsTr("Not checked: this approval belongs to an earlier folder of the project, so the same-named folder of the current one was not inspected.")
                    + "\n" + section.failureText(result)
            return qsTr("Not checked: %1").arg(section.failureText(result))
        }
        const stored = result.storedInspectionComplete === true
            ? qsTr("When it was approved, the inspection was complete.")
            : qsTr("When it was approved, the inspection was only partial.")
        return qsTr("Checked now - %1").arg(section.countsText(result)) + " " + section.reasonsText(result) + "\n" + stored
    }

    function approvedAtText(row) {
        return Qt.formatDateTime(new Date(Number(row.approvedAtMs)), "yyyy-MM-dd HH:mm")
    }

    function operationText(operation) {
        if (operation === "preview") return qsTr("Inspecting the folder...")
        if (operation === "approve") return qsTr("Saving the approval...")
        if (operation === "recheck") return qsTr("Checking the approval...")
        if (operation === "list") return qsTr("Loading the approved folders...")
        if (operation === "revoke") return qsTr("Revoking the approval...")
        return qsTr("Working...")
    }

    // ---- state ---------------------------------------------------------------------------------------------------------------------

    function dropForm() {
        section.formEpoch += 1
        section.preview = null
        section.previewRequestId = 0
        section.previewInput = ""
        section.ackNested = false
        section.ackIncomplete = false
    }

    function resetView() {
        section.viewEpoch += 1
        section.dropForm()
        section.rechecks = ({})
        section.notice = ""
        section.errorLine = ""
        section.cancelAsked = false
    }

    function register(id, operation, subject) {
        const entries = Object.assign({}, section.inflight)
        entries[String(id)] = {
            operation: operation, projectId: section.projectId, subject: subject,
            formEpoch: section.formEpoch, viewEpoch: section.viewEpoch
        }
        section.inflight = entries
    }

    function take(requestId) {
        const key = String(requestId)
        const entry = section.inflight[key]
        if (entry === undefined) return null
        const entries = Object.assign({}, section.inflight)
        delete entries[key]
        section.inflight = entries
        return entry
    }

    // The answer still belongs to what is on screen: the same project, the same shown/hidden episode and (for the form) the same form.
    function viewCurrent(entry) {
        return entry.projectId === section.projectId && entry.viewEpoch === section.viewEpoch && section.visible
    }

    function formCurrent(entry) {
        return section.viewCurrent(entry) && entry.formEpoch === section.formEpoch
    }

    function refused() {
        section.errorLine = section.controller.lastErrorCode + ": " + section.controller.lastErrorText
    }

    function tryRefresh() {
        if (!section.pendingRefresh || !section.visible || !section.storageAvailable || section.projectId.length === 0) return
        if (section.controllerBusy) return
        const id = section.controller.listApprovals(section.projectId)
        if (id === 0) return
        section.pendingRefresh = false
        section.register(id, "list", "")
    }

    function requestRefresh() {
        section.pendingRefresh = true
        section.tryRefresh()
    }

    function onPathEdited() {
        if (section.controller !== null) section.controller.invalidatePreview()
        section.dropForm()
        section.notice = ""
        section.errorLine = ""
    }

    function startPreview() {
        if (!section.storageAvailable || section.controllerBusy || pathField.text.length === 0 || section.projectId.length === 0) return
        section.dropForm()
        section.notice = ""
        section.errorLine = ""
        const id = section.controller.previewApproval(section.projectId, pathField.text)
        if (id === 0) {
            section.refused()
            return
        }
        section.register(id, "preview", pathField.text)
    }

    function startApprove() {
        if (!section.canApprove) return
        const id = section.controller.approve(section.projectId, section.previewInput, section.previewRequestId,
            section.needsNested && section.ackNested, section.needsIncomplete && section.ackIncomplete)
        if (id === 0) {
            section.refused()
            return
        }
        section.notice = ""
        section.errorLine = ""
        section.register(id, "approve", section.previewInput)
    }

    function startRecheck(approvalId) {
        if (!section.storageAvailable || section.controllerBusy) return
        const id = section.controller.recheckApproval(section.projectId, approvalId)
        if (id === 0) {
            section.refused()
            return
        }
        section.errorLine = ""
        section.register(id, "recheck", approvalId)
    }

    function startRevoke(approvalId) {
        if (!section.storageAvailable || section.controllerBusy) return
        const id = section.controller.revokeApproval(section.projectId, approvalId)
        if (id === 0) {
            section.refused()
            return
        }
        section.notice = ""
        section.errorLine = ""
        section.register(id, "revoke", approvalId)
    }

    // An accepted cancel makes the Backend drop its own preview, so the local one (and both confirmations) go with it: a form that
    // kept Approve would offer a preview id the Backend no longer takes. The request itself stays in `inflight`, so a write that
    // had already begun is still reported when it ends.
    function cancelRunning() {
        if (section.controller === null) return
        if (section.controller.cancelInspection()) {
            section.cancelAsked = true
            section.dropForm()
        }
    }

    // Leaving (hidden, project changed, destroyed): ask OUR running inspection to stop and drop every actionable preview.
    function release() {
        if (section.controller === null) return
        if (section.hasInspectionInflight) section.controller.cancelInspection()
        section.controller.invalidatePreview()
    }

    function enter() {
        if (section.controller === null) return
        section.controller.setProjectContext(section.projectId)
        section.requestRefresh()
    }

    onProjectIdChanged: {
        section.release()
        section.resetView()
        section.approvals = []
        section.listLoaded = false
        section.listError = ""
        section.enter()
    }
    onProjectPathChanged: {
        // The same project, another folder: nothing that was inspected before may be approved now (the Backend checks again too).
        section.release()
        section.resetView()
        section.requestRefresh()
    }
    onVisibleChanged: {
        if (!section.visible) {
            section.release()
            section.resetView()
        } else {
            section.enter()
        }
    }
    onStorageAvailableChanged: section.requestRefresh()
    Component.onCompleted: Qt.callLater(function () {
        if (section.visible) section.enter()
    })
    Component.onDestruction: section.release()

    Connections {
        target: section.controller
        ignoreUnknownSignals: true

        function onBusyChanged() {
            if (section.controller.busy !== true) section.cancelAsked = false
            // Not here directly: the terminal signal of the operation that just ended follows this one.
            Qt.callLater(section.tryRefresh)
        }

        function onPreviewFinished(requestId, projectId, requestedPath, result) {
            const entry = section.take(requestId)
            if (entry === null || !section.viewCurrent(entry)) return
            if (!section.formCurrent(entry)) {
                // The form moved on (an edit, or a Cancel that dropped it): nothing of this answer is shown, but a cancel is said.
                if (result.cancelled === true) section.notice = qsTr("The inspection was cancelled; nothing was changed.")
                return
            }
            if (result.success !== true) {
                section.errorLine = result.cancelled === true
                    ? qsTr("The inspection was cancelled; nothing was changed.") : section.failureText(result)
                return
            }
            if (result.stale === true || result.cancelRequested === true) {
                section.errorLine = qsTr("That inspection was superseded; inspect the folder again.")
                return
            }
            section.preview = result
            section.previewRequestId = requestId
            section.previewInput = entry.subject
            section.ackNested = false
            section.ackIncomplete = false
        }

        function onApproveFinished(requestId, projectId, requestedPath, result) {
            const entry = section.take(requestId)
            if (entry === null || !section.viewCurrent(entry)) return
            const sameForm = entry.formEpoch === section.formEpoch
            // The Backend dropped its preview when the operation was cancelled or superseded: never keep a local one it no longer knows.
            if (result.stale === true || result.cancelRequested === true || result.cancelled === true) section.dropForm()
            if (result.success === true) {
                // A folder that was written is reported as written, whatever happened to the form or to a cancel request meanwhile.
                let text = result.alreadyApproved === true
                    ? qsTr("\"%1\" was already approved; nothing was changed.").arg(entry.subject)
                    : qsTr("Approved \"%1\". New Runs and project context now leave it out.").arg(entry.subject)
                if (result.cancelRequested === true && result.alreadyApproved !== true)
                    text += " " + qsTr("The cancel request came too late: the approval had already been recorded.")
                section.notice = text
                if (sameForm) section.dropForm()
                return
            }
            if (result.cancelled === true) {
                section.notice = qsTr("The approval was cancelled; nothing was written.")
                return
            }
            section.errorLine = entry.subject + " - " + section.failureText(result)
        }

        function onRecheckFinished(requestId, projectId, approvalId, result) {
            const entry = section.take(requestId)
            if (entry === null || !section.viewCurrent(entry)) return
            if (result.stale === true || result.cancelRequested === true || result.cancelled === true) section.dropForm()
            if (result.cancelled === true) {
                section.notice = qsTr("The check was cancelled; nothing was changed.")
                return
            }
            const next = Object.assign({}, section.rechecks)
            next[entry.subject] = result
            section.rechecks = next
        }

        function onListFinished(requestId, projectId, result) {
            const entry = section.take(requestId)
            if (entry === null || !section.viewCurrent(entry)) return
            section.listLoaded = true
            if (result.success === true) {
                section.approvals = result.approvals
                section.listError = ""
            } else {
                section.listError = section.failureText(result)
            }
        }

        function onRevokeFinished(requestId, projectId, approvalId, result) {
            const entry = section.take(requestId)
            if (entry === null || !section.viewCurrent(entry)) return
            if (result.success !== true) {
                section.errorLine = section.failureText(result)
                return
            }
            const next = Object.assign({}, section.rechecks)
            delete next[entry.subject]
            section.rechecks = next
            section.notice = result.removed === true
                ? qsTr("The approval was revoked. Folders already recorded in earlier Runs keep their recorded area.")
                : qsTr("That approval no longer existed.")
            if (result.removed !== true) section.requestRefresh()
        }

        // A record was really inserted or deleted for THAT project. If it is the one on screen the list is read again - as soon as
        // the controller is free; if the section is hidden, the reading waits until it is shown.
        function onApprovalsChanged(changedProjectId) {
            if (changedProjectId !== section.projectId) return
            section.requestRefresh()
        }
    }

    // ---- view ----------------------------------------------------------------------------------------------------------------------

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: content.implicitHeight + section.hostWindow.dp(22)
        radius: section.colors.cornerRadiusSmall
        color: section.colors.inputBackground
        border.width: 1
        border.color: section.colors.border

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: section.hostWindow.dp(11)
            spacing: section.hostWindow.dp(7)

            Text {
                text: qsTr("Output folders")
                color: section.colors.textPrimary
                font.pixelSize: section.hostWindow.dp(13)
                font.weight: Font.DemiBold
            }

            Text {
                objectName: "outputFolderIntro"
                Layout.fillWidth: true
                text: qsTr("Name folders inside this project that only hold generated results, such as a build folder. Their content is then left out of what the application reads for Runs and project context, and proposals cannot write into them. Nothing is deleted and .gitignore is not changed. A folder that also holds sources would hide those sources too.")
                textFormat: Text.PlainText
                color: section.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: section.hostWindow.dp(10)
            }

            Text {
                objectName: "outputFolderUnavailable"
                Layout.fillWidth: true
                visible: !section.storageAvailable
                text: qsTr("Output folder approvals are not available: the application has no durable storage.")
                textFormat: Text.PlainText
                color: section.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: section.hostWindow.dp(10)
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: section.storageAvailable
                spacing: section.hostWindow.dp(7)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: section.hostWindow.dp(6)

                    TextField {
                        id: pathField
                        objectName: "outputFolderPathField"
                        Layout.fillWidth: true
                        Layout.minimumWidth: section.hostWindow.dp(80)
                        Layout.preferredHeight: section.hostWindow.dp(38)
                        placeholderText: qsTr("Folder inside the project, e.g. build/x64-debug")
                        color: section.colors.textPrimary
                        placeholderTextColor: section.colors.textMuted
                        font.pixelSize: section.hostWindow.dp(12)
                        selectByMouse: true
                        verticalAlignment: TextInput.AlignVCenter
                        onTextChanged: section.onPathEdited()
                        onAccepted: section.startPreview()

                        background: Rectangle {
                            radius: section.colors.cornerRadiusSmall
                            color: section.colors.surface
                            border.width: pathField.activeFocus ? 1 : 0
                            border.color: section.colors.inputBorderFocused
                        }
                    }

                    ProjectActionButton {
                        objectName: "outputFolderPreviewButton"
                        text: qsTr("Preview")
                        enabled: !section.controllerBusy && pathField.text.length > 0 && section.projectId.length > 0
                        hostWindow: section.hostWindow
                        colors: section.colors
                        onClicked: section.startPreview()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: section.controllerBusy
                    spacing: section.hostWindow.dp(8)

                    Text {
                        objectName: "outputFolderBusy"
                        Layout.fillWidth: true
                        text: section.cancelAsked ? qsTr("Cancelling - waiting for the operation to stop...")
                            : section.operationText(section.controller ? section.controller.activeOperation : "")
                        textFormat: Text.PlainText
                        color: section.colors.textSecondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: section.hostWindow.dp(10)
                    }

                    ProjectActionButton {
                        objectName: "outputFolderCancelButton"
                        visible: section.inspecting
                        enabled: !section.cancelAsked
                        text: qsTr("Cancel")
                        hostWindow: section.hostWindow
                        colors: section.colors
                        onClicked: section.cancelRunning()
                    }
                }

                Text {
                    objectName: "outputFolderError"
                    Layout.fillWidth: true
                    visible: section.errorLine.length > 0
                    text: section.errorLine
                    textFormat: Text.PlainText
                    color: section.colors.error
                    wrapMode: Text.Wrap
                    font.pixelSize: section.hostWindow.dp(10)
                }

                Text {
                    objectName: "outputFolderNotice"
                    Layout.fillWidth: true
                    visible: section.notice.length > 0
                    text: section.notice
                    textFormat: Text.PlainText
                    color: section.colors.success
                    wrapMode: Text.Wrap
                    font.pixelSize: section.hostWindow.dp(10)
                }

                // What the Backend established for the text in the field. Shown only while it is still the current preview.
                Rectangle {
                    objectName: "outputFolderPreviewCard"
                    Layout.fillWidth: true
                    visible: section.previewUsable
                    Layout.preferredHeight: previewContent.implicitHeight + section.hostWindow.dp(16)
                    radius: section.colors.cornerRadiusSmall
                    color: section.colors.surface
                    border.width: 1
                    border.color: section.colors.border

                    ColumnLayout {
                        id: previewContent
                        anchors.fill: parent
                        anchors.margins: section.hostWindow.dp(8)
                        spacing: section.hostWindow.dp(5)

                        Text {
                            objectName: "outputFolderPreviewPath"
                            Layout.fillWidth: true
                            text: section.preview ? qsTr("Folder: %1").arg(section.preview.relativePath) : ""
                            textFormat: Text.PlainText
                            color: section.colors.textPrimary
                            wrapMode: Text.Wrap
                            font.pixelSize: section.hostWindow.dp(11)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            objectName: "outputFolderPreviewRoot"
                            Layout.fillWidth: true
                            text: section.preview ? qsTr("In the project folder: %1").arg(section.preview.workspaceRoot) : ""
                            textFormat: Text.PlainText
                            color: section.colors.textMuted
                            wrapMode: Text.Wrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderPreviewCounts"
                            Layout.fillWidth: true
                            text: section.preview ? section.countsText(section.preview) : ""
                            textFormat: Text.PlainText
                            color: section.colors.textSecondary
                            wrapMode: Text.WordWrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderPreviewExisting"
                            Layout.fillWidth: true
                            visible: section.preview !== null && section.preview.exists !== true
                                && String(section.preview.existingPrefix).length > 0
                            text: section.preview ? qsTr("The existing part of the path is: %1").arg(section.preview.existingPrefix) : ""
                            textFormat: Text.PlainText
                            color: section.colors.textMuted
                            wrapMode: Text.Wrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderPreviewReasons"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: section.preview ? section.reasonsText(section.preview) : ""
                            textFormat: Text.PlainText
                            color: section.colors.warning
                            wrapMode: Text.WordWrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderPreviewNested"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: section.preview ? section.nestedText(section.preview) : ""
                            textFormat: Text.PlainText
                            color: section.colors.warning
                            wrapMode: Text.WordWrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderPreviewSamples"
                            Layout.fillWidth: true
                            visible: section.preview !== null && section.preview.nestedSourceSamples !== undefined
                                && section.preview.nestedSourceSamples.length > 0
                            text: section.preview && section.preview.nestedSourceSamples ? section.preview.nestedSourceSamples.join("\n") : ""
                            textFormat: Text.PlainText
                            color: section.colors.textMuted
                            wrapMode: Text.Wrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderPreviewAlready"
                            Layout.fillWidth: true
                            visible: section.preview !== null && section.preview.alreadyApproved === true
                            text: qsTr("This folder is already approved for this project.")
                            textFormat: Text.PlainText
                            color: section.colors.success
                            wrapMode: Text.WordWrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }

                        CheckBox {
                            id: nestedAck
                            objectName: "outputFolderNestedAck"
                            Layout.fillWidth: true
                            visible: section.needsNested
                            text: qsTr("I understand that these CMakeLists.txt files, and any sources below this folder, will be hidden too.")
                            font.pixelSize: section.hostWindow.dp(10)
                            palette.windowText: section.colors.textPrimary
                            contentItem: Text {
                                text: nestedAck.text
                                font: nestedAck.font
                                color: section.colors.textPrimary
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: nestedAck.indicator ? nestedAck.indicator.width + nestedAck.spacing : 0
                            }
                            onToggled: section.ackNested = nestedAck.checked
                        }
                        // A user's click replaces a plain binding of "checked"; this one keeps the box equal to the state.
                        Binding { target: nestedAck; property: "checked"; value: section.ackNested }
                        CheckBox {
                            id: incompleteAck
                            objectName: "outputFolderIncompleteAck"
                            Layout.fillWidth: true
                            visible: section.needsIncomplete
                            text: qsTr("I understand that the folder was inspected only partly, so its content is not fully known.")
                            font.pixelSize: section.hostWindow.dp(10)
                            palette.windowText: section.colors.textPrimary
                            contentItem: Text {
                                text: incompleteAck.text
                                font: incompleteAck.font
                                color: section.colors.textPrimary
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: incompleteAck.indicator ? incompleteAck.indicator.width + incompleteAck.spacing : 0
                            }
                            onToggled: section.ackIncomplete = incompleteAck.checked
                        }
                        Binding { target: incompleteAck; property: "checked"; value: section.ackIncomplete }

                        ProjectActionButton {
                            objectName: "outputFolderApproveButton"
                            text: qsTr("Approve as output folder")
                            primary: true
                            enabled: section.canApprove
                            hostWindow: section.hostWindow
                            colors: section.colors
                            onClicked: section.startApprove()
                        }
                    }
                }

                Text {
                    text: qsTr("Approved folders")
                    color: section.colors.textPrimary
                    font.pixelSize: section.hostWindow.dp(12)
                    font.weight: Font.DemiBold
                }

                Text {
                    objectName: "outputFolderListError"
                    Layout.fillWidth: true
                    visible: section.listError.length > 0
                    text: section.listError
                    textFormat: Text.PlainText
                    color: section.colors.error
                    wrapMode: Text.Wrap
                    font.pixelSize: section.hostWindow.dp(10)
                }

                Text {
                    objectName: "outputFolderEmpty"
                    Layout.fillWidth: true
                    visible: section.listLoaded && section.listError.length === 0 && section.approvals.length === 0
                    text: qsTr("No folders are approved for this project yet.")
                    textFormat: Text.PlainText
                    color: section.colors.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: section.hostWindow.dp(10)
                }

                Repeater {
                    model: section.approvals

                    ColumnLayout {
                        id: row
                        required property var modelData
                        objectName: "outputFolderRow"
                        Layout.fillWidth: true
                        spacing: section.hostWindow.dp(3)

                        readonly property var recheck: section.rechecks[row.modelData.approvalId]

                        Text {
                            objectName: "outputFolderRowPath"
                            Layout.fillWidth: true
                            text: row.modelData.relativePath
                            textFormat: Text.PlainText
                            color: section.colors.textPrimary
                            wrapMode: Text.Wrap
                            font.pixelSize: section.hostWindow.dp(11)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            objectName: "outputFolderRowApplies"
                            Layout.fillWidth: true
                            text: row.modelData.appliesToCurrentRoot === true
                                ? qsTr("Applies to this project's folder.")
                                : qsTr("Not active: it was approved for an earlier folder of this project. It can still be revoked.")
                            textFormat: Text.PlainText
                            color: row.modelData.appliesToCurrentRoot === true ? section.colors.success : section.colors.warning
                            wrapMode: Text.WordWrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderRowRoot"
                            Layout.fillWidth: true
                            text: qsTr("Project folder at approval: %1").arg(row.modelData.workspaceRoot)
                            textFormat: Text.PlainText
                            color: section.colors.textMuted
                            wrapMode: Text.Wrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Text {
                            objectName: "outputFolderRowMeta"
                            Layout.fillWidth: true
                            text: qsTr("Approved by %1 on %2.").arg(row.modelData.approvedBy).arg(section.approvedAtText(row.modelData))
                                + (row.modelData.inspectionComplete === true ? "" : " " + qsTr("The inspection was partial."))
                                + (row.modelData.nestedSourcesAcknowledged === true ? " " + qsTr("Nested sources were confirmed.") : "")
                            textFormat: Text.PlainText
                            color: section.colors.textMuted
                            wrapMode: Text.WordWrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: section.hostWindow.dp(6)

                            ProjectActionButton {
                                objectName: "outputFolderRecheckButton"
                                text: qsTr("Check again")
                                enabled: !section.controllerBusy
                                hostWindow: section.hostWindow
                                colors: section.colors
                                onClicked: section.startRecheck(row.modelData.approvalId)
                            }
                            ProjectActionButton {
                                objectName: "outputFolderRevokeButton"
                                text: qsTr("Revoke")
                                danger: true
                                enabled: !section.controllerBusy
                                hostWindow: section.hostWindow
                                colors: section.colors
                                onClicked: section.startRevoke(row.modelData.approvalId)
                            }
                        }
                        Text {
                            objectName: "outputFolderRecheckResult"
                            Layout.fillWidth: true
                            visible: row.recheck !== undefined
                            text: row.recheck !== undefined ? section.recheckText(row.recheck) : ""
                            textFormat: Text.PlainText
                            color: row.recheck !== undefined && row.recheck.success === true
                                ? section.colors.textSecondary : section.colors.warning
                            wrapMode: Text.Wrap
                            font.pixelSize: section.hostWindow.dp(10)
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            Layout.topMargin: section.hostWindow.dp(3)
                            color: section.colors.border
                        }
                    }
                }
            }
        }
    }
}
