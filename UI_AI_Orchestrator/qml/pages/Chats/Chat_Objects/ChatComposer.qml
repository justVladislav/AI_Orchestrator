import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: composer

    required property var hostWindow
    required property var colors
    required property var providersModel
    required property var projectsModel

    property string providerId: ""
    property string modelId: ""
    property bool providerAvailable: true
    property string projectId: ""

    // "sending": the just-typed user message is in flight, blocks re-send.
    // "generating": an assistant reply is being produced, swaps Send->Stop.
    property bool sending: false
    property bool generating: false

    property bool editing: false
    property string editingPreview: ""
    property bool contextAttachmentEnabled: false
    property string contextAttachmentSummary: ""

    signal sendRequested(string message)
    signal stopRequested()
    signal providerSelected(string providerId, string modelId)
    signal projectSelected(string projectId)
    signal attachRequested()
    signal detachRequested()
    signal cancelEditRequested()

    readonly property int maxComposerHeight: hostWindow.dp(160)
    readonly property bool canSend: composerField.text.trim().length > 0
                                    && !sending && !generating && providerAvailable

    function trySend() {
        if (!canSend)
            return

        composer.sendRequested(composerField.text.trim())
        composerField.clear()
    }

    function setDraft(value) {
        composerField.text = value
        composerField.forceActiveFocus()
        composerField.cursorPosition = composerField.text.length
    }

    radius: colors.cornerRadiusMedium
    color: colors.surface
    border.width: 1
    border.color: colors.border
    implicitHeight: contentColumn.implicitHeight + hostWindow.dp(20)

    ColumnLayout {
        id: contentColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: composer.hostWindow.dp(12)
        spacing: composer.hostWindow.dp(8)

        RowLayout {
            Layout.fillWidth: true
            spacing: composer.hostWindow.dp(6)
            visible: composer.editing

            Text {
                Layout.fillWidth: true
                text: qsTr("Editing message: \u201C%1\u201D").arg(composer.editingPreview)
                color: composer.colors.textMuted
                elide: Text.ElideRight
                font.pixelSize: composer.hostWindow.dp(10.5)
            }

            ChatActionButton {
                text: qsTr("Cancel edit")
                hostWindow: composer.hostWindow
                colors: composer.colors
                onClicked: composer.cancelEditRequested()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: composer.hostWindow.dp(8)

            ChatIconButton {
                objectName: "chatAttachProjectContextButton"
                hostWindow: composer.hostWindow
                colors: composer.colors
                iconGlyph: "+"
                outlined: true
                accessibleLabel: qsTr("Add context or attachment")
                ToolTip.text: composer.contextAttachmentEnabled
                              ? qsTr("Project context is attached to the next message")
                              : qsTr("Attach the reviewed Project Context Snapshot")
                onClicked: composer.attachRequested()
            }

            ChatProviderComboBox {
                Layout.fillWidth: true
                Layout.minimumWidth: composer.hostWindow.dp(130)
                Layout.preferredWidth: composer.hostWindow.dp(220)
                Layout.preferredHeight: composer.hostWindow.dp(34)
                hostWindow: composer.hostWindow
                colors: composer.colors
                providersModel: composer.providersModel
                currentProviderId: composer.providerId
                currentModelId: composer.modelId
                onProviderSelected: (providerId, modelId) => composer.providerSelected(providerId, modelId)
            }

            ChatProjectComboBox {
                Layout.fillWidth: true
                Layout.minimumWidth: composer.hostWindow.dp(110)
                Layout.preferredWidth: composer.hostWindow.dp(190)
                Layout.preferredHeight: composer.hostWindow.dp(34)
                hostWindow: composer.hostWindow
                colors: composer.colors
                projectsModel: composer.projectsModel
                currentProjectId: composer.projectId
                onProjectSelected: projectId => composer.projectSelected(projectId)
            }

            Text {
                visible: composer.width >= composer.hostWindow.dp(620)
                text: qsTr("%1 characters").arg(composerField.text.length)
                color: composer.colors.textMuted
                font.pixelSize: composer.hostWindow.dp(10.5)
            }
        }

        Rectangle {
            objectName: "chatPendingProjectContext"
            Layout.fillWidth: true
            Layout.preferredHeight: pendingContextRow.implicitHeight + composer.hostWindow.dp(12)
            visible: composer.contextAttachmentEnabled
            radius: composer.colors.cornerRadiusSmall
            color: composer.colors.accentSoft
            border.width: 1
            border.color: composer.colors.accent

            RowLayout {
                id: pendingContextRow
                anchors.fill: parent
                anchors.margins: composer.hostWindow.dp(6)
                spacing: composer.hostWindow.dp(8)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Attached to next message: %1").arg(composer.contextAttachmentSummary)
                    color: composer.colors.textPrimary
                    wrapMode: Text.WordWrap
                    font.pixelSize: composer.hostWindow.dp(10.5)
                }

                ChatActionButton {
                    text: qsTr("Remove")
                    hostWindow: composer.hostWindow
                    colors: composer.colors
                    onClicked: composer.detachRequested()
                }
            }
        }

        ScrollView {
            id: composerScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(
                Math.max(composerField.contentHeight + composer.hostWindow.dp(22),
                         composer.hostWindow.dp(44)),
                composer.maxComposerHeight)
            clip: true
            rightPadding: composer.hostWindow.dp(12)

            TextArea {
                id: composerField
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("Ask anything…")
                color: composer.colors.textPrimary
                placeholderTextColor: composer.colors.textMuted
                font.pixelSize: composer.hostWindow.dp(13)
                selectByMouse: true

                Accessible.name: qsTr("Message")

                background: Rectangle {
                    radius: composer.colors.cornerRadiusSmall
                    color: composer.colors.inputBackground
                    border.width: composerField.activeFocus ? 1 : 0
                    border.color: composer.colors.inputBorderFocused
                }

                Keys.onPressed: event => {
                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                            && !(event.modifiers & Qt.ShiftModifier)) {
                        event.accepted = true
                        composer.trySend()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: composer.hostWindow.dp(8)

            Text {
                Layout.fillWidth: true
                text: composer.providerAvailable
                      ? qsTr("Enter to send · Shift+Enter for new line")
                      : qsTr("Selected provider is unavailable — choose another to send")
                color: composer.providerAvailable ? composer.colors.textMuted : composer.colors.error
                elide: Text.ElideRight
                font.pixelSize: composer.hostWindow.dp(10.5)
            }

            StackLayout {
                currentIndex: composer.generating ? 1 : 0

                ChatActionButton {
                    text: qsTr("Send")
                    iconGlyph: "\u2192"
                    primary: true
                    enabled: composer.canSend
                    hostWindow: composer.hostWindow
                    colors: composer.colors
                    onClicked: composer.trySend()
                }

                ChatActionButton {
                    text: qsTr("Stop")
                    iconGlyph: "\u25A0"
                    hostWindow: composer.hostWindow
                    colors: composer.colors
                    onClicked: composer.stopRequested()
                }
            }
        }
    }
}
