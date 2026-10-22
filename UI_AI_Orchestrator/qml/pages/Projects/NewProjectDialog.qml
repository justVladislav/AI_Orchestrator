import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "Projects_Objets"

Dialog {
    id: dialog
    objectName: "newProjectDialog"

    required property var hostWindow
    required property var colors
    required property var backend
    // Set by the page after a failed backend call; kept visible until the
    // next attempt so the dialog does not close on a rejected command.
    property string errorText: ""
    // Expand the Description editor to dp(220) height for long-form context.
    property bool descriptionExpanded: false

    // AG-18.4B: "existing" (use whatever CMake preset already exists),
    // "create" (generate a minimal local CMakeUserPresets.json - the page
    // writes it, before Project creation, only on explicit confirmation),
    // or "later" (unchanged prior behavior - no verification setup at all).
    property string verificationMode: "later"
    property string vsTestResultText: ""
    property bool vsTestResultIsError: false
    // The probeId testExistingConfiguration() returned for the probe this
    // dialog is currently actually waiting on -
    // 0 means none. onTestConfigurationFinished() ignores any completion
    // whose own probeId does not match this, so a stale result (fields
    // changed, mode switched, or the dialog closed and reopened for a
    // different project while the old probe was still running) is never
    // shown as if it answered the current question.
    property double vsActiveProbeId: 0
    onVerificationModeChanged: dialog.vsInvalidateActiveProbe()

    // Read-only preview, re-evaluated live from the current field text -
    // never writes anything. Backend-validated, not merely locally guessed.
    readonly property var vsPreviewResult: {
        if (dialog.verificationMode !== "create" || !dialog.backend) return ({success: false})
        return dialog.backend.projectVerificationSetup.composePreview(
            "Ninja", vsConfigurationCombo.currentText, vsQtPrefixField.text.trim(),
            vsConfigurePresetField.text.trim(), vsBuildPresetField.text.trim(),
            vsBinaryDirField.text.trim())
    }
    readonly property var vsTargetPathResult: {
        if (dialog.verificationMode === "later" || pathField.text.trim().length === 0
                || !dialog.backend) {
            return ({success: false})
        }
        return dialog.backend.projectVerificationSetup.resolveTargetPath(pathField.text.trim())
    }
    readonly property bool vsCreateModeValid: dialog.verificationMode !== "create"
        || (vsPreviewResult.success === true && vsTargetPathResult.success === true)

    function pathFieldTrimmed() {
        return pathField.text.trim()
    }
    // Any edit to a field a running probe's answer depends on - or a mode
    // switch - invalidates that probe: its eventual completion must never
    // be shown as if it answered the NEW, now-different question. A
    // best-effort cancellation is requested too, but invalidation alone
    // (dropping vsActiveProbeId) is what actually guarantees the stale
    // result is ignored, since cancellation is cooperative and not
    // instantaneous.
    function vsInvalidateActiveProbe() {
        if (dialog.vsActiveProbeId === 0) return
        dialog.vsActiveProbeId = 0
        if (dialog.backend) dialog.backend.projectVerificationSetup.cancelTestConfiguration()
    }
    function vsActiveConfigurePresetName() {
        return dialog.verificationMode === "create"
            ? vsConfigurePresetField.text.trim() : vsExistingConfigurePresetField.text.trim()
    }
    function vsActiveBuildPresetName() {
        return dialog.verificationMode === "create"
            ? vsBuildPresetField.text.trim() : vsExistingBuildPresetField.text.trim()
    }

    signal projectAccepted(string name, string path, string description, var verificationSetup)

    function utf8ByteLength(text) {
        // QML's JavaScript engine is ECMAScript, not a browser runtime, so the
        // Web API TextEncoder is unavailable. Count UTF-8 bytes explicitly,
        // including four-byte surrogate pairs, without limiting stored text.
        let bytes = 0
        for (let index = 0; index < text.length; ++index) {
            const codeUnit = text.charCodeAt(index)
            if (codeUnit < 0x80) {
                bytes += 1
            } else if (codeUnit < 0x800) {
                bytes += 2
            } else if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF
                       && index + 1 < text.length) {
                const low = text.charCodeAt(index + 1)
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    bytes += 4
                    ++index
                } else {
                    bytes += 3
                }
            } else {
                bytes += 3
            }
        }
        return bytes
    }

    function submit() {
        // Keep one submission path for the visible button and headless UI
        // tests so both exercise the same trimmed signal contract.
        if (nameField.text.trim().length === 0 || pathField.text.trim().length === 0)
            return
        if (!dialog.vsCreateModeValid) return

        let verificationSetup = {mode: dialog.verificationMode}
        if (dialog.verificationMode === "create") {
            verificationSetup.generator = "Ninja"
            verificationSetup.configuration = vsConfigurationCombo.currentText
            verificationSetup.qtPrefixPath = vsQtPrefixField.text.trim()
            verificationSetup.configurePresetName = vsConfigurePresetField.text.trim()
            verificationSetup.buildPresetName = vsBuildPresetField.text.trim()
            verificationSetup.binaryDirRelative = vsBinaryDirField.text.trim()
        } else if (dialog.verificationMode === "existing") {
            // The chosen pair is remembered for the project (the Backend still proves it before a Run uses it).
            verificationSetup.configurePresetName = vsExistingConfigurePresetField.text.trim()
            verificationSetup.buildPresetName = vsExistingBuildPresetField.text.trim()
        }
        dialog.projectAccepted(nameField.text.trim(),
                               pathField.text.trim(),
                               descriptionField.text.trim(),
                               verificationSetup)
    }

    anchors.centerIn: parent
    width: hostWindow.dp(600)
    modal: true
    title: qsTr("New project")
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(13)

    onOpened: {
        nameField.clear()
        pathField.clear()
        descriptionField.clear()
        dialog.errorText = ""
        dialog.descriptionExpanded = false
        dialog.verificationMode = "later"
        dialog.vsTestResultText = ""
        dialog.vsTestResultIsError = false
        dialog.vsActiveProbeId = 0
        vsQtPrefixField.clear()
        // Reset to the same sensible defaults the fields carry at initial
        // creation - never a blank identifier a fresh open would otherwise
        // immediately reject.
        vsConfigurePresetField.text = "local-qt-debug"
        vsBuildPresetField.text = "local-qt-debug"
        vsBinaryDirField.text = "build/local-qt-debug"
        vsConfigurationCombo.currentIndex = 0
        vsExistingConfigurePresetField.clear()
        vsExistingBuildPresetField.clear()
        nameField.forceActiveFocus()
    }

    onClosed: {
        // Never leave a real cmake probe running unobserved after the
        // dialog itself is gone - cancelTestConfiguration() is a safe no-op
        // when nothing is in flight. Also drop the active probe id so a
        // completion that still arrives after cancellation (cancellation is
        // cooperative, not instantaneous) is ignored rather than updating a
        // closed dialog's own state.
        dialog.vsActiveProbeId = 0
        if (dialog.backend) dialog.backend.projectVerificationSetup.cancelTestConfiguration()
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
        id: dialogScroll
        implicitHeight: Math.min(dialogColumn.implicitHeight,
                                 Math.max(dialog.hostWindow.dp(300),
                                          dialog.hostWindow.height
                                          - dialog.hostWindow.dp(150)))
        contentWidth: availableWidth
        contentHeight: dialogColumn.implicitHeight
        clip: true
        rightPadding: dialog.hostWindow.dp(12)
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: dialogColumn
            width: dialogScroll.availableWidth
            spacing: dialog.hostWindow.dp(13)

        Text {
            Layout.fillWidth: true
            text: qsTr("Create a workspace that will hold project context, tasks and orchestration settings.")
            color: dialog.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(14)
        }

        Text {
            text: qsTr("Name")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        TextField {
            id: nameField
            objectName: "projectNameField"
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.hostWindow.dp(38)
            placeholderText: qsTr("My project")
            color: dialog.colors.textPrimary
            placeholderTextColor: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(13)
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter

            background: Rectangle {
                radius: dialog.colors.cornerRadiusSmall
                color: dialog.colors.inputBackground
                border.width: nameField.activeFocus ? 1 : 0
                border.color: dialog.colors.inputBorderFocused
            }
        }

        Text {
            text: qsTr("Project path")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)

            TextField {
                id: pathField
                objectName: "projectPathField"
                onTextChanged: dialog.vsInvalidateActiveProbe()
                Layout.fillWidth: true
                Layout.preferredHeight: dialog.hostWindow.dp(38)
                placeholderText: qsTr("C:/Projects/MyProject")
                color: dialog.colors.textPrimary
                placeholderTextColor: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(13)
                selectByMouse: true
                verticalAlignment: TextInput.AlignVCenter

                background: Rectangle {
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: pathField.activeFocus ? 1 : 0
                    border.color: dialog.colors.inputBorderFocused
                }
            }

            ProjectActionButton {
                text: qsTr("Browse…")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: folderDialog.open()
            }
        }

        FolderDialog {
            id: folderDialog
            title: qsTr("Select project folder")

            onAccepted: {
                // selectedFolder is a file:// URL; the backend expects a plain
                // local path, matching what users type into the field.
                let path = folderDialog.selectedFolder.toString()
                if (path.startsWith("file://"))
                    path = path.substring(7)
                // A Windows path yields "file:///C:/Users/..." -> after
                // stripping "file://" that leaves "/C:/Users/...": strip
                // that extra leading slash only when a drive letter follows.
                // A real POSIX path (e.g. "/Users/...") must keep its own
                // leading slash, or the result becomes relative and
                // std::filesystem::exists() resolves it against the wrong
                // directory.
                if (/^\/[A-Za-z]:\//.test(path))
                    path = path.substring(1)
                pathField.text = decodeURIComponent(path)
            }
        }

        // ── Verification setup (AG-18.4B) ──────────────────────────────────────
        Text {
            text: qsTr("Verification setup")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(13)
            font.weight: Font.Medium
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)

            RowLayout {
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(16)

                RadioButton {
                    id: vsModeExisting
                    objectName: "verificationModeExisting"
                    text: qsTr("Use existing presets")
                    checked: dialog.verificationMode === "existing"
                    onToggled: if (checked) dialog.verificationMode = "existing"
                    contentItem: Text {
                        text: vsModeExisting.text
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        leftPadding: vsModeExisting.indicator.width + dialog.hostWindow.dp(6)
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                RadioButton {
                    id: vsModeCreate
                    objectName: "verificationModeCreate"
                    text: qsTr("Create local Qt/CMake preset")
                    checked: dialog.verificationMode === "create"
                    onToggled: if (checked) dialog.verificationMode = "create"
                    contentItem: Text {
                        text: vsModeCreate.text
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        leftPadding: vsModeCreate.indicator.width + dialog.hostWindow.dp(6)
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                RadioButton {
                    id: vsModeLater
                    objectName: "verificationModeLater"
                    text: qsTr("Configure later")
                    checked: dialog.verificationMode === "later"
                    onToggled: if (checked) dialog.verificationMode = "later"
                    contentItem: Text {
                        text: vsModeLater.text
                        color: dialog.colors.textPrimary
                        font.pixelSize: dialog.hostWindow.dp(12)
                        leftPadding: vsModeLater.indicator.width + dialog.hostWindow.dp(6)
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            // "Configure later": unchanged prior behavior - no fields, no
            // Backend verification-setup call of any kind.
            Text {
                visible: dialog.verificationMode === "later"
                Layout.fillWidth: true
                text: qsTr("The first Manifest Run on this project will report verification setup "
                          + "as required. You can configure it from Run Details afterward.")
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(11)
                wrapMode: Text.WordWrap
            }

            // "Use existing presets": no file is ever written by this dialog;
            // "Test configuration" only reads, via a real local cmake probe.
            ColumnLayout {
                visible: dialog.verificationMode === "existing"
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(6)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Discovery runs automatically on the first Manifest Run. Optionally "
                              + "test a specific preset pair now.")
                    color: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(11)
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: dialog.hostWindow.dp(8)
                    TextField {
                        id: vsExistingConfigurePresetField
                        objectName: "verificationExistingConfigurePresetField"
                        onTextChanged: dialog.vsInvalidateActiveProbe()
                        Layout.fillWidth: true
                        Layout.preferredHeight: dialog.hostWindow.dp(34)
                        placeholderText: qsTr("Configure preset name")
                        color: dialog.colors.textPrimary
                        placeholderTextColor: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(12)
                        selectByMouse: true
                        background: Rectangle {
                            radius: dialog.colors.cornerRadiusSmall
                            color: dialog.colors.inputBackground
                            border.width: vsExistingConfigurePresetField.activeFocus ? 1 : 0
                            border.color: dialog.colors.inputBorderFocused
                        }
                    }
                    TextField {
                        id: vsExistingBuildPresetField
                        objectName: "verificationExistingBuildPresetField"
                        onTextChanged: dialog.vsInvalidateActiveProbe()
                        Layout.fillWidth: true
                        Layout.preferredHeight: dialog.hostWindow.dp(34)
                        placeholderText: qsTr("Build preset name (optional)")
                        color: dialog.colors.textPrimary
                        placeholderTextColor: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(12)
                        selectByMouse: true
                        background: Rectangle {
                            radius: dialog.colors.cornerRadiusSmall
                            color: dialog.colors.inputBackground
                            border.width: vsExistingBuildPresetField.activeFocus ? 1 : 0
                            border.color: dialog.colors.inputBorderFocused
                        }
                    }
                }
            }

            // "Create local Qt/CMake preset": every field is Backend-
            // validated live via vsPreviewResult; nothing is written until
            // "Create project" is clicked.
            ColumnLayout {
                visible: dialog.verificationMode === "create"
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(6)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: dialog.hostWindow.dp(8)

                    Text {
                        text: qsTr("Generator: Ninja")
                        color: dialog.colors.textSecondary
                        font.pixelSize: dialog.hostWindow.dp(11.5)
                    }
                    Item { Layout.fillWidth: true }
                    ProjectComboBox {
                        id: vsConfigurationCombo
                        objectName: "verificationConfigurationCombo"
                        onCurrentTextChanged: dialog.vsInvalidateActiveProbe()
                        hostWindow: dialog.hostWindow
                        colors: dialog.colors
                        Layout.preferredWidth: dialog.hostWindow.dp(120)
                        Layout.preferredHeight: dialog.hostWindow.dp(30)
                        model: ["Debug", "Release"]
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: dialog.hostWindow.dp(8)

                    TextField {
                        id: vsQtPrefixField
                        objectName: "verificationQtPrefixField"
                        onTextChanged: dialog.vsInvalidateActiveProbe()
                        Layout.fillWidth: true
                        Layout.preferredHeight: dialog.hostWindow.dp(34)
                        placeholderText: qsTr("Qt prefix path, e.g. C:/Qt/6.11.1/msvc2022_64")
                        color: dialog.colors.textPrimary
                        placeholderTextColor: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(12)
                        selectByMouse: true
                        background: Rectangle {
                            radius: dialog.colors.cornerRadiusSmall
                            color: dialog.colors.inputBackground
                            border.width: vsQtPrefixField.activeFocus ? 1 : 0
                            border.color: dialog.colors.inputBorderFocused
                        }
                    }
                    ProjectActionButton {
                        text: qsTr("Browse…")
                        hostWindow: dialog.hostWindow
                        colors: dialog.colors
                        onClicked: vsQtPrefixFolderDialog.open()
                    }
                }
                FolderDialog {
                    id: vsQtPrefixFolderDialog
                    title: qsTr("Select Qt installation prefix")
                    onAccepted: {
                        let path = vsQtPrefixFolderDialog.selectedFolder.toString()
                        if (path.startsWith("file://")) path = path.substring(7)
                        if (/^\/[A-Za-z]:\//.test(path)) path = path.substring(1)
                        vsQtPrefixField.text = decodeURIComponent(path)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: dialog.hostWindow.dp(8)

                    TextField {
                        id: vsConfigurePresetField
                        objectName: "verificationConfigurePresetField"
                        onTextChanged: dialog.vsInvalidateActiveProbe()
                        Layout.fillWidth: true
                        Layout.preferredHeight: dialog.hostWindow.dp(34)
                        placeholderText: qsTr("Configure preset name")
                        text: "local-qt-debug"
                        color: dialog.colors.textPrimary
                        placeholderTextColor: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(12)
                        selectByMouse: true
                        background: Rectangle {
                            radius: dialog.colors.cornerRadiusSmall
                            color: dialog.colors.inputBackground
                            border.width: vsConfigurePresetField.activeFocus ? 1 : 0
                            border.color: dialog.colors.inputBorderFocused
                        }
                    }
                    TextField {
                        id: vsBuildPresetField
                        objectName: "verificationBuildPresetField"
                        onTextChanged: dialog.vsInvalidateActiveProbe()
                        Layout.fillWidth: true
                        Layout.preferredHeight: dialog.hostWindow.dp(34)
                        placeholderText: qsTr("Build preset name")
                        text: "local-qt-debug"
                        color: dialog.colors.textPrimary
                        placeholderTextColor: dialog.colors.textMuted
                        font.pixelSize: dialog.hostWindow.dp(12)
                        selectByMouse: true
                        background: Rectangle {
                            radius: dialog.colors.cornerRadiusSmall
                            color: dialog.colors.inputBackground
                            border.width: vsBuildPresetField.activeFocus ? 1 : 0
                            border.color: dialog.colors.inputBorderFocused
                        }
                    }
                }

                TextField {
                    id: vsBinaryDirField
                    objectName: "verificationBinaryDirField"
                    onTextChanged: dialog.vsInvalidateActiveProbe()
                    Layout.fillWidth: true
                    Layout.preferredHeight: dialog.hostWindow.dp(34)
                    placeholderText: qsTr("Binary directory (relative), e.g. build/local-qt-debug")
                    text: "build/local-qt-debug"
                    color: dialog.colors.textPrimary
                    placeholderTextColor: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(12)
                    selectByMouse: true
                    background: Rectangle {
                        radius: dialog.colors.cornerRadiusSmall
                        color: dialog.colors.inputBackground
                        border.width: vsBinaryDirField.activeFocus ? 1 : 0
                        border.color: dialog.colors.inputBorderFocused
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: dialog.pathFieldTrimmed().length > 0
                    text: dialog.vsTargetPathResult.success
                          ? qsTr("Will create: %1").arg(dialog.vsTargetPathResult.targetPath)
                          : qsTr("Target path unavailable: %1: %2")
                                .arg(dialog.vsTargetPathResult.errorCode || "")
                                .arg(dialog.vsTargetPathResult.errorText || "")
                    color: dialog.vsTargetPathResult.success
                           ? dialog.colors.textMuted : dialog.colors.error
                    font.pixelSize: dialog.hostWindow.dp(10.5)
                    wrapMode: Text.WordWrap
                }

                Text {
                    text: qsTr("Preview")
                    color: dialog.colors.textSecondary
                    font.pixelSize: dialog.hostWindow.dp(11)
                    font.weight: Font.Medium
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: dialog.hostWindow.dp(110)
                    radius: dialog.colors.cornerRadiusSmall
                    color: dialog.colors.inputBackground
                    border.width: 1
                    border.color: dialog.colors.border
                    clip: true

                    ScrollView {
                        anchors.fill: parent
                        anchors.margins: dialog.hostWindow.dp(6)
                        TextArea {
                            objectName: "verificationPresetPreview"
                            readOnly: true
                            text: dialog.vsPreviewResult.success
                                  ? dialog.vsPreviewResult.json
                                  : (dialog.vsPreviewResult.errorText || "")
                            color: dialog.vsPreviewResult.success
                                   ? dialog.colors.textPrimary : dialog.colors.error
                            font.pixelSize: dialog.hostWindow.dp(10.5)
                            font.family: "Consolas, monospace"
                            wrapMode: TextArea.NoWrap
                            background: null
                        }
                    }
                }
            }

            Text {
                visible: dialog.verificationMode === "create"
                Layout.fillWidth: true
                text: qsTr("The generated preset can be tested after the project is created, when CMake can evaluate it against the real project sources.")
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(10.5)
                wrapMode: Text.WordWrap
            }

            // Existing presets can be tested against the real project.
            // Never invoked automatically; a configure may create a build
            // tree, so the user must explicitly ask for it.
            RowLayout {
                visible: dialog.verificationMode === "existing"
                Layout.fillWidth: true
                spacing: dialog.hostWindow.dp(8)

                ProjectActionButton {
                    objectName: "verificationTestConfigurationButton"
                    text: dialog.backend && dialog.backend.projectVerificationSetup.testingConfiguration
                          ? qsTr("Testing…") : qsTr("Test configuration")
                    enabled: dialog.backend
                             && !dialog.backend.projectVerificationSetup.testingConfiguration
                             && dialog.pathFieldTrimmed().length > 0
                             && dialog.vsActiveConfigurePresetName().length > 0
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    onClicked: {
                        dialog.vsTestResultText = ""
                        dialog.vsTestResultIsError = false
                        dialog.vsActiveProbeId =
                            dialog.backend.projectVerificationSetup.testExistingConfiguration(
                                pathField.text.trim(), dialog.vsActiveConfigurePresetName(),
                                dialog.vsActiveBuildPresetName())
                    }
                }
                ProjectActionButton {
                    visible: dialog.backend
                             && dialog.backend.projectVerificationSetup.testingConfiguration
                    text: qsTr("Cancel")
                    hostWindow: dialog.hostWindow
                    colors: dialog.colors
                    onClicked: {
                        dialog.backend.projectVerificationSetup.cancelTestConfiguration()
                        dialog.vsActiveProbeId = 0
                    }
                }
            }
            Text {
                objectName: "verificationTestConfigurationResult"
                visible: dialog.vsTestResultText.length > 0
                Layout.fillWidth: true
                text: dialog.vsTestResultText
                color: dialog.vsTestResultIsError ? dialog.colors.error : dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(10.5)
                wrapMode: Text.WordWrap
            }
        }

        Connections {
            target: dialog.backend ? dialog.backend.projectVerificationSetup : null
            function onTestConfigurationFinished(probeId, success, state, diagnostic,
                    configureConfirmed, buildConfirmed, errorCode, errorText) {
                // Ignore a completion for a probe that is no longer the one
                // this dialog is actually waiting on - the fields may have
                // changed, the mode may have switched, or the dialog may
                // have been closed and reopened for a different project
                // while this exact probe was still running in the
                // background.
                if (probeId !== dialog.vsActiveProbeId) return
                dialog.vsActiveProbeId = 0
                if (!success) {
                    dialog.vsTestResultIsError = true
                    dialog.vsTestResultText = errorCode + ": " + errorText
                    return
                }
                dialog.vsTestResultIsError = !configureConfirmed
                let text = qsTr("State: %1").arg(state)
                text += configureConfirmed
                    ? qsTr(" · configure preset confirmed")
                    : qsTr(" · configure preset not found")
                if (dialog.vsActiveBuildPresetName().length > 0) {
                    text += buildConfirmed
                        ? qsTr(" · build preset confirmed")
                        : qsTr(" · build preset not found")
                }
                if (diagnostic.length > 0) text += "\n" + diagnostic
                dialog.vsTestResultText = text
            }
        }

        // ── Description label + counter + expand toggle ───────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: dialog.hostWindow.dp(8)

            Text {
                text: qsTr("Description")
                color: dialog.colors.textPrimary
                font.pixelSize: dialog.hostWindow.dp(13)
                font.weight: Font.Medium
            }

            Item { Layout.fillWidth: true }

            Text {
                objectName: "descriptionCounter"
                visible: descriptionField.length > 0
                text: descriptionField.length >= 1000
                      ? qsTr("%1 chars  ·  %2 KB")
                        .arg(descriptionField.length)
                        .arg((dialog.utf8ByteLength(descriptionField.text) / 1024).toFixed(1))
                      : qsTr("%1 chars").arg(descriptionField.length)
                color: dialog.colors.textMuted
                font.pixelSize: dialog.hostWindow.dp(9)
            }

            Text {
                text: dialog.descriptionExpanded ? qsTr("Collapse ▲") : qsTr("Expand ▼")
                color: expandDescMouse.containsMouse
                       ? dialog.colors.textPrimary : dialog.colors.accent
                font.pixelSize: dialog.hostWindow.dp(9)

                MouseArea {
                    id: expandDescMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.descriptionExpanded = !dialog.descriptionExpanded
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Saved as durable project context — reused by Chat, Run, and Comparison.")
            color: dialog.colors.textMuted
            font.pixelSize: dialog.hostWindow.dp(9.5)
            wrapMode: Text.WordWrap
        }

        // ── Scrollable Description editor ────────────────────────────────────
        Rectangle {
            id: descContainer
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.descriptionExpanded
                                    ? dialog.hostWindow.dp(220)
                                    : dialog.hostWindow.dp(120)
            radius: dialog.colors.cornerRadiusSmall
            color: dialog.colors.inputBackground
            border.width: descriptionField.activeFocus ? 1 : 0
            border.color: dialog.colors.inputBorderFocused
            clip: true

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: dialog.colors.reduceAnimations ? 0 : 160; easing.type: Easing.OutCubic }
            }

            Flickable {
                id: descFlickable
                objectName: "descriptionFlickable"
                anchors.fill: parent
                anchors.margins: dialog.hostWindow.dp(2)
                clip: true
                contentWidth: width
                contentHeight: Math.max(descriptionField.implicitHeight, height)
                boundsBehavior: Flickable.StopAtBounds

                // Connects this Flickable to the TextArea for cursor tracking.
                TextArea.flickable: descriptionField

                TextArea {
                    id: descriptionField
                    objectName: "descriptionField"
                    width: Math.max(0, descFlickable.width
                                    - (descScrollBar.visible
                                       ? dialog.hostWindow.dp(12)
                                       : dialog.hostWindow.dp(4)))
                    placeholderText: qsTr("What are we building? Describe the project context and goals.\nInternal newlines are preserved and sent to the backend.")
                    color: dialog.colors.textPrimary
                    placeholderTextColor: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(13)
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    // No background — the parent Rectangle provides it.
                    background: null
                }

                ScrollBar.vertical: ScrollBar {
                    id: descScrollBar
                    visible: descFlickable.contentHeight > descFlickable.height
                    policy: ScrollBar.AsNeeded
                    contentItem: Rectangle {
                        implicitWidth: dialog.hostWindow.dp(5)
                        radius: width / 2
                        color: descScrollBar.active  ? dialog.colors.textSecondary
                             : descScrollBar.hovered ? dialog.colors.textMuted
                             : dialog.colors.border
                        opacity: descScrollBar.active || descScrollBar.hovered ? 1.0 : 0.5
                    }
                    background: null
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: dialog.errorText.length > 0
            text: dialog.errorText
            color: dialog.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(12)
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: dialog.hostWindow.dp(4)
            spacing: dialog.hostWindow.dp(8)

            Item { Layout.fillWidth: true }

            ProjectActionButton {
                text: qsTr("Cancel")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.reject()
            }

            ProjectActionButton {
                text: qsTr("Create project")
                primary: true
                enabled: nameField.text.trim().length > 0
                         && pathField.text.trim().length > 0
                         && dialog.vsCreateModeValid
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: {
                    // The page owns the backend call and only closes this dialog
                    // on success, so a validation failure keeps the input visible.
                    dialog.submit()
                }
            }
        }
    }
    }
}
