pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "Settings_Objects"

Item {
    id: page

    required property var hostWindow
    required property var colors
    required property var backend

    readonly property var settings: backend.settings
    readonly property var scaleOptions: [
        { "value": "0", "label": qsTr("Automatic") },
        { "value": "75", "label": qsTr("75%") },
        { "value": "100", "label": qsTr("100%") },
        { "value": "125", "label": qsTr("125%") },
        { "value": "150", "label": qsTr("150%") },
        { "value": "175", "label": qsTr("175%") },
        { "value": "200", "label": qsTr("200%") }
    ]
    readonly property var contextPolicyOptions: [
        { "value": "active_project", "label": qsTr("Use active project") },
        { "value": "ask_every_time", "label": qsTr("Ask every time") },
        { "value": "no_project", "label": qsTr("No project context") }
    ]

    // ── Responsive breakpoints (matches project convention) ───────────────────
    readonly property real logicalWidth: hostWindow.scaleFactor > 0
                                         ? width / hostWindow.scaleFactor
                                         : width
    readonly property string layoutMode: logicalWidth >= 1000 ? "wide"
                                         : logicalWidth >= 680  ? "medium"
                                         : "narrow"

    // Confirmation dialog state — no backend; dialog is purely presentational.
    property bool clearHistoryDialogVisible: false

    // Public signal; no fake handler is wired.
    signal clearHistoryRequested()

    // The Backend's own refusal of the last typed verification folder ("" when it was accepted or nothing was typed).
    property string verificationFolderError: ""

    function applyVerificationFolder() {
        const accepted = page.settings.setVerificationScratchRoot(scratchField.text)
        page.verificationFolderError = accepted ? "" : page.settings.lastError
        if (accepted)
            scratchField.text = page.settings.verificationScratchRoot
    }

    Connections {
        target: page.settings
        // Follows the stored value unless the user is in the middle of typing another one.
        function onVerificationScratchChanged() {
            if (!scratchField.activeFocus)
                scratchField.text = page.settings.verificationScratchRoot
        }
    }

    // ── Scroll view ───────────────────────────────────────────────────────────

    Basic.ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth
        contentHeight: outerCol.implicitHeight
        clip: true

        Basic.ScrollBar.vertical: SettingsScrollBar {
            hostWindow: page.hostWindow
            colors: page.colors
        }
        Basic.ScrollBar.horizontal: Basic.ScrollBar {
            policy: Basic.ScrollBar.AlwaysOff
        }

        // Outer column: full viewport width, supplies top/bottom gutters.
        ColumnLayout {
            id: outerCol
            width: scroll.availableWidth
            spacing: 0

            Item { implicitHeight: page.colors.contentMargin; implicitWidth: 1 }

            // Inner column: carries horizontal content margin.
            ColumnLayout {
                id: innerCol
                Layout.fillWidth: true
                Layout.leftMargin: page.colors.contentMargin
                Layout.rightMargin: page.colors.contentMargin
                spacing: page.hostWindow.dp(20)

                // ── Page header ───────────────────────────────────────────────

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: page.hostWindow.dp(4)

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Settings")
                        color: page.colors.textPrimary
                        font.pixelSize: page.hostWindow.dp(28)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Manage appearance, orchestration, and application preferences.")
                        color: page.colors.textSecondary
                        font.pixelSize: page.hostWindow.dp(14)
                        wrapMode: Text.WordWrap
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: page.settings.lastError.length > 0
                    text: page.settings.lastError
                    color: page.colors.error
                    font.pixelSize: page.hostWindow.dp(12)
                    wrapMode: Text.WordWrap
                }

                // ── Responsive grid: Appearance · Orchestration · Chat ────────

                GridLayout {
                    Layout.fillWidth: true
                    columns: page.layoutMode === "wide" ? 2 : 1
                    uniformCellWidths: page.layoutMode === "wide"
                    columnSpacing: page.hostWindow.dp(20)
                    rowSpacing: page.hostWindow.dp(20)

                    // ── 1. Appearance ─────────────────────────────────────────

                    SettingsSection {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        hostWindow: page.hostWindow
                        colors: page.colors
                        sectionTitle: qsTr("Appearance")

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Theme")
                            description: qsTr("Light, dark, or transparent window style.")

                            RowLayout {
                                spacing: page.hostWindow.dp(4)

                                SettingsSegmentButton {
                                    hostWindow: page.hostWindow
                                    colors: page.colors
                                    text: qsTr("Light")
                                    selected: page.settings.themeMode === "light"
                                    onClicked: page.settings.setThemeMode("light")
                                }
                                SettingsSegmentButton {
                                    hostWindow: page.hostWindow
                                    colors: page.colors
                                    text: qsTr("Dark")
                                    selected: page.settings.themeMode === "dark"
                                    onClicked: page.settings.setThemeMode("dark")
                                }
                                SettingsSegmentButton {
                                    hostWindow: page.hostWindow
                                    colors: page.colors
                                    text: qsTr("Transparent")
                                    selected: page.settings.themeMode === "transparent"
                                    onClicked: page.settings.setThemeMode("transparent")
                                }
                            }
                        }

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Interface scale")
                            description: qsTr("Auto or custom text and element sizing.")
                            SettingsComboBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Interface scale")
                                options: page.scaleOptions
                                selectedValue: String(page.settings.interfaceScalePercent)
                                onValueSelected: value =>
                                    page.settings.setInterfaceScalePercent(Number(value))
                            }
                        }

                        SettingsToggle {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Compact mode")
                            description: qsTr("Reduce padding and spacing throughout the interface.")
                            connected: true
                            checked: page.settings.compactMode
                            onToggled: value => page.settings.setCompactMode(value)
                        }

                        SettingsToggle {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Reduce animations")
                            description: qsTr("Disable transitions and motion effects.")
                            connected: true
                            checked: page.settings.reduceAnimations
                            onToggled: value => page.settings.setReduceAnimations(value)
                            isLast: true
                        }
                    }

                    // ── 2. Orchestration defaults ─────────────────────────────

                    SettingsSection {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        hostWindow: page.hostWindow
                        colors: page.colors
                        sectionTitle: qsTr("Orchestration defaults")

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Default project context policy")
                            description: qsTr("Which project context is injected when launching a run.")
                            SettingsComboBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Default project context policy")
                                options: page.contextPolicyOptions
                                selectedValue: page.settings.runContextPolicy
                                onValueSelected: value =>
                                    page.settings.setRunContextPolicy(value)
                            }
                        }

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Default execution profile")
                            description: qsTr("Preset used when no profile is explicitly selected.")
                            SettingsComboBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Default execution profile")
                                options: page.settings.executionProfileChoices
                                selectedValue: page.settings.defaultExecutionProfileId
                                onValueSelected: value =>
                                    page.settings.setDefaultExecutionProfileId(value)
                            }
                        }

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Default timeout")
                            description: qsTr("Maximum run duration before automatic cancellation.")
                            SettingsSpinBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Default timeout in seconds")
                                from: 1
                                to: 86400
                                stepSize: 30
                                suffix: qsTr(" s")
                                value: Math.round(Number(page.settings.defaultTimeoutMs) / 1000)
                                onValueSelected: selectedValue =>
                                    page.settings.setDefaultTimeoutMs(selectedValue * 1000)
                            }
                        }

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Default max parallel agents")
                            description: qsTr("Simultaneous model requests per orchestration run.")
                            SettingsSpinBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Default max parallel agents")
                                from: 1
                                to: 32
                                value: page.settings.defaultMaxParallelAgents
                                onValueSelected: selectedValue =>
                                    page.settings.setDefaultMaxParallelAgents(selectedValue)
                            }
                        }

                        SettingsToggle {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Confirm before launching many models")
                            description: qsTr("Show a prompt when a run targets more than three providers.")
                            connected: true
                            checked: page.settings.confirmManyModels
                            onToggled: value => page.settings.setConfirmManyModels(value)
                            isLast: true
                        }
                    }

                    // ── 3. Chat defaults — full width on wide ─────────────────

                    SettingsSection {
                        Layout.fillWidth: true
                        Layout.columnSpan: page.layoutMode === "wide" ? 2 : 1
                        hostWindow: page.hostWindow
                        colors: page.colors
                        sectionTitle: qsTr("Chat defaults")

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Project context")
                            description: qsTr("Active project, ask every time, or no project — for new chats.")
                            SettingsComboBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Chat project context")
                                options: page.contextPolicyOptions
                                selectedValue: page.settings.chatContextPolicy
                                onValueSelected: value =>
                                    page.settings.setChatContextPolicy(value)
                            }
                        }

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Default provider and model")
                            description: qsTr("Provider and model pre-selected when opening a new chat.")
                            SettingsComboBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Default chat provider and model")
                                options: page.settings.chatModelChoices
                                selectedValue: page.settings.defaultChatProviderId.length > 0
                                               ? page.settings.defaultChatProviderId + "|"
                                                 + page.settings.defaultChatModelId
                                               : ""
                                onValueSelected: value => {
                                    for (let index = 0;
                                         index < page.settings.chatModelChoices.length;
                                         ++index) {
                                        const choice = page.settings.chatModelChoices[index]
                                        if (String(choice.value) === value) {
                                            page.settings.setDefaultChatModel(
                                                        choice.providerId, choice.modelId)
                                            return
                                        }
                                    }
                                }
                            }
                        }

                        SettingsRow {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("History limit")
                            description: qsTr("Maximum number of messages retained per conversation.")
                            SettingsSpinBox {
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Chat history limit")
                                from: 1
                                to: 10000
                                stepSize: 25
                                value: page.settings.chatHistoryLimit
                                onValueSelected: selectedValue =>
                                    page.settings.setChatHistoryLimit(selectedValue)
                            }
                        }

                        SettingsToggle {
                            Layout.fillWidth: true
                            hostWindow: page.hostWindow
                            colors: page.colors
                            label: qsTr("Streaming responses")
                            description: qsTr("Display model output as it is generated token by token.")
                            connected: false
                            notConnectedLabel: qsTr("Coming later")
                            isLast: true
                        }
                    }
                }

                // ── 4. Data and privacy — always full width ───────────────────

                SettingsSection {
                    Layout.fillWidth: true
                    hostWindow: page.hostWindow
                    colors: page.colors
                    sectionTitle: qsTr("Data and privacy")

                    // Workspace path — full-width read-only field below label
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: page.hostWindow.dp(12)
                        Layout.bottomMargin: page.hostWindow.dp(12)
                        spacing: page.hostWindow.dp(6)

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Workspace path")
                            color: page.colors.textPrimary
                            font.pixelSize: page.hostWindow.dp(13)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Root directory for project files and outputs.")
                            color: page.colors.textSecondary
                            font.pixelSize: page.hostWindow.dp(12)
                            wrapMode: Text.WordWrap
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: page.hostWindow.dp(34)
                            color: page.colors.inputBackground
                            border.color: page.colors.inputBorder
                            border.width: 1
                            radius: page.colors.cornerRadiusSmall

                            Text {
                                anchors {
                                    left: parent.left; right: parent.right
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: page.hostWindow.dp(10)
                                    rightMargin: page.hostWindow.dp(10)
                                }
                                text: qsTr("Not configured — read-only placeholder")
                                color: page.colors.textDisabled
                                font.pixelSize: page.hostWindow.dp(12)
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: page.colors.separator
                    }

                    // Database path — masked read-only field
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: page.hostWindow.dp(12)
                        Layout.bottomMargin: page.hostWindow.dp(12)
                        spacing: page.hostWindow.dp(6)

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Database path")
                            color: page.colors.textPrimary
                            font.pixelSize: page.hostWindow.dp(13)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Location of the local SQLite database. Masked for privacy.")
                            color: page.colors.textSecondary
                            font.pixelSize: page.hostWindow.dp(12)
                            wrapMode: Text.WordWrap
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: page.hostWindow.dp(34)
                            color: page.colors.inputBackground
                            border.color: page.colors.inputBorder
                            border.width: 1
                            radius: page.colors.cornerRadiusSmall

                            Text {
                                anchors {
                                    left: parent.left; right: parent.right
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: page.hostWindow.dp(10)
                                    rightMargin: page.hostWindow.dp(10)
                                }
                                text: "••••••••••••••••••••••••"
                                color: page.colors.textMuted
                                font.pixelSize: page.hostWindow.dp(14)
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: page.colors.separator
                    }

                    // Folder of the TEMPORARY verification copies - validated by the Backend (length, link, overlap with the project,
                    // privacy, write probe); a refusal is shown as the Backend worded it and the previous setting stays in force.
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: page.hostWindow.dp(12)
                        Layout.bottomMargin: page.hostWindow.dp(12)
                        spacing: page.hostWindow.dp(6)

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Verification folder")
                            color: page.colors.textPrimary
                            font.pixelSize: page.hostWindow.dp(13)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Where temporary copies of a project are built to be checked (build, lint). Choose a short folder "
                                       + "(at most %1 characters) that only you can write to; a folder that does not exist yet is "
                                       + "created for you. Leave it empty to use the system temporary folder.")
                                  .arg(page.settings.verificationScratchMaxLength)
                            color: page.colors.textSecondary
                            font.pixelSize: page.hostWindow.dp(12)
                            wrapMode: Text.WordWrap
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: page.hostWindow.dp(8)

                            SettingsTextField {
                                id: scratchField
                                objectName: "verificationScratchField"
                                Layout.fillWidth: true
                                hostWindow: page.hostWindow
                                colors: page.colors
                                accessibleLabel: qsTr("Verification folder")
                                text: page.settings.verificationScratchRoot
                                placeholderText: page.settings.verificationScratchEffective
                                onCommitted: page.applyVerificationFolder()
                            }
                            SettingsActionButton {
                                objectName: "verificationScratchApplyButton"
                                hostWindow: page.hostWindow
                                colors: page.colors
                                text: qsTr("Apply")
                                actionEnabled: true
                                onClicked: page.applyVerificationFolder()
                            }
                            SettingsActionButton {
                                objectName: "verificationScratchDefaultButton"
                                hostWindow: page.hostWindow
                                colors: page.colors
                                text: qsTr("Use default")
                                actionEnabled: page.settings.verificationScratchSource === "setting"
                                onClicked: {
                                    scratchField.text = ""
                                    page.applyVerificationFolder()
                                }
                            }
                        }
                        Text {
                            objectName: "verificationScratchStatus"
                            Layout.fillWidth: true
                            readonly property string refusal: page.verificationFolderError
                            text: refusal.length > 0
                                  ? refusal
                                  : page.settings.verificationScratchUsable
                                    ? qsTr("In use: %1").arg(page.settings.verificationScratchEffective)
                                    : page.settings.verificationScratchProblem
                            color: refusal.length > 0 || !page.settings.verificationScratchUsable
                                   ? page.colors.error : page.colors.textMuted
                            font.pixelSize: page.hostWindow.dp(12)
                            wrapMode: Text.WordWrap
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: page.colors.separator
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Data retention period")
                        description: qsTr("How long chat and run history is kept before automatic removal.")
                        SettingsDisabledComboBox {
                            hostWindow: page.hostWindow
                            colors: page.colors
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Export data")
                        description: qsTr("Download all workspace data as a JSON archive.")
                        SettingsActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Export")
                            actionEnabled: false
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Import data")
                        description: qsTr("Restore workspace data from a previously exported archive.")
                        SettingsActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Import")
                            actionEnabled: false
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Backup")
                        description: qsTr("Create a timestamped backup of the current database.")
                        SettingsActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Backup")
                            actionEnabled: false
                        }
                    }

                    // Clear history — danger row with confirmation dialog.
                    // The trigger button is enabled (it only opens the
                    // presentational dialog below); the dialog's own Confirm
                    // action stays disabled since no delete contract exists.
                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Clear all history")
                        description: qsTr("Permanently remove all chat conversations and run records.")
                        isLast: true

                        SettingsActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Clear history")
                            actionEnabled: true
                            danger: true
                            onClicked: page.clearHistoryDialogVisible = true
                        }
                    }
                }

                // ── 5. Diagnostics — always full width ────────────────────────

                SettingsSection {
                    Layout.fillWidth: true
                    hostWindow: page.hostWindow
                    colors: page.colors
                    sectionTitle: qsTr("Diagnostics")

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Application version")
                        Text {
                            objectName: "applicationVersionText"
                            // The version the executable was built as (BackendBridge); empty only when nothing set it.
                            readonly property string versionText: page.backend.applicationVersion
                            text: versionText.length > 0 ? versionText : qsTr("Version information unavailable")
                            color: versionText.length > 0 ? page.colors.textPrimary : page.colors.textDisabled
                            font.pixelSize: page.hostWindow.dp(12)
                            font.italic: versionText.length === 0
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Database status")
                        SettingsStatusValue {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: page.backend.status
                            dotColor: page.backend.status.indexOf("unavailable") >= 0
                                      ? page.colors.error : page.colors.success
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Credential store status")
                        SettingsStatusValue {
                            hostWindow: page.hostWindow
                            colors: page.colors
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Provider runtime status")
                        SettingsStatusValue {
                            hostWindow: page.hostWindow
                            colors: page.colors
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Open logs")
                        description: qsTr("Open the application log directory in the file manager.")
                        SettingsActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Open logs")
                            actionEnabled: false
                        }
                    }

                    SettingsRow {
                        Layout.fillWidth: true
                        hostWindow: page.hostWindow
                        colors: page.colors
                        label: qsTr("Copy diagnostics")
                        description: qsTr("Copy a diagnostic summary to the clipboard for support requests.")
                        isLast: true
                        SettingsActionButton {
                            hostWindow: page.hostWindow
                            colors: page.colors
                            text: qsTr("Copy diagnostics")
                            actionEnabled: false
                        }
                    }
                }
            }

            Item { implicitHeight: page.hostWindow.dp(24); implicitWidth: 1 }
        }
    }

    // ── Clear history confirmation dialog ─────────────────────────────────────
    // Presentational only: Confirm stays disabled and no data is deleted.
    // clearHistoryRequested is intentionally never emitted here — emitting it
    // would imply a delete actually happened.

    Rectangle {
        anchors.fill: parent
        visible: page.clearHistoryDialogVisible
        color: page.colors.overlay
        z: 100

        MouseArea {
            anchors.fill: parent
            onClicked: page.clearHistoryDialogVisible = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(page.hostWindow.dp(380), parent.width - page.hostWindow.dp(40))
            implicitHeight: dialogCol.implicitHeight + page.hostWindow.dp(48)
            color: page.colors.surfaceRaised
            border.color: page.colors.border
            border.width: 1
            radius: page.colors.cornerRadiusMedium

            MouseArea { anchors.fill: parent } // absorb backdrop click

            ColumnLayout {
                id: dialogCol
                anchors {
                    left: parent.left; right: parent.right; top: parent.top
                    margins: page.hostWindow.dp(24)
                }
                spacing: page.hostWindow.dp(12)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Clear all history?")
                    color: page.colors.textPrimary
                    font.pixelSize: page.hostWindow.dp(16)
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("This action is not connected to a backend operation yet and will not delete any data. Once connected, it would permanently remove all chat and run history.")
                    color: page.colors.textSecondary
                    font.pixelSize: page.hostWindow.dp(13)
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: page.hostWindow.dp(4)
                    spacing: page.hostWindow.dp(8)

                    Item { Layout.fillWidth: true }

                    SettingsActionButton {
                        hostWindow: page.hostWindow
                        colors: page.colors
                        text: qsTr("Cancel")
                        actionEnabled: true
                        onClicked: page.clearHistoryDialogVisible = false
                    }

                    // Confirm stays disabled — no backend delete contract yet.
                    SettingsActionButton {
                        hostWindow: page.hostWindow
                        colors: page.colors
                        text: qsTr("Clear history")
                        actionEnabled: false
                        danger: true
                    }
                }
            }
        }
    }
}
