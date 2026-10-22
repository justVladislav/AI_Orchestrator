pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: delegateRoot

    required property var hostWindow
    required property var colors

    required property int index
    required property string messageId
    required property string role
    required property string content
    required property string timestamp
    required property string status
    required property int tokenCount
    required property string contextFingerprint
    required property int contextTokens
    // PC-3D: decided by the Backend from the stored text, never by QML: "none" | "one" | "multiple" | "too_large".
    // Only a completed Assistant message can be anything but "none".
    required property string planEnvelope

    signal copyRequested(string messageId, string content)
    signal planDraftsRequested(string messageId)
    signal retryRequested(string messageId)
    signal editRequested(string messageId, string content)

    readonly property bool isUser: role === "user"
    readonly property bool isAssistant: role === "assistant"
    readonly property bool isSystem: role === "system"
    readonly property bool isError: role === "error"

    readonly property bool isProcessing: status === "sending" || status === "generating"
    readonly property real maxBubbleWidth: width * 0.74

    width: ListView.view ? ListView.view.width : 0
    height: (isUser || isAssistant)
            ? bubbleColumn.implicitHeight + hostWindow.dp(10)
            : noticeRect.height + hostWindow.dp(10)

    // Hidden TextEdit used purely as a real system-clipboard bridge:
    // TextEdit.copy() copies the current selection to the OS clipboard
    // without requiring any extra import.
    TextEdit {
        id: clipboardBridge
        visible: false
        text: ""
    }

    function requestCopy() {
        clipboardBridge.text = delegateRoot.content
        clipboardBridge.selectAll()
        clipboardBridge.copy()
        delegateRoot.copyRequested(delegateRoot.messageId, delegateRoot.content)
    }

    // System / error rows: centered, full-width notice style (not a bubble).
    Rectangle {
        id: noticeRect
        visible: delegateRoot.isSystem || delegateRoot.isError
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        width: Math.min(delegateRoot.width * 0.86, noticeText.implicitWidth + delegateRoot.hostWindow.dp(28))
        height: noticeText.implicitHeight + delegateRoot.hostWindow.dp(14)
        radius: delegateRoot.colors.cornerRadiusSmall
        color: delegateRoot.isError ? delegateRoot.colors.errorSoft : delegateRoot.colors.surfaceAlternative
        border.width: delegateRoot.isError ? 1 : 0
        border.color: delegateRoot.colors.error

        Text {
            id: noticeText
            anchors.centerIn: parent
            width: parent.width - delegateRoot.hostWindow.dp(20)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: delegateRoot.content
            color: delegateRoot.isError ? delegateRoot.colors.error : delegateRoot.colors.textMuted
            font.pixelSize: delegateRoot.hostWindow.dp(11)
        }
    }

    // User / assistant rows: aligned bubble with meta line and actions.
    ColumnLayout {
        id: bubbleColumn
        visible: delegateRoot.isUser || delegateRoot.isAssistant
        anchors.top: parent.top
        anchors.right: delegateRoot.isUser ? parent.right : undefined
        anchors.left: delegateRoot.isAssistant ? parent.left : undefined
        width: Math.min(implicitWidth, delegateRoot.maxBubbleWidth)
        spacing: delegateRoot.hostWindow.dp(3)

        Text {
            Layout.alignment: delegateRoot.isUser ? Qt.AlignRight : Qt.AlignLeft
            text: (delegateRoot.isUser ? qsTr("You") : qsTr("Assistant")) + " · " + delegateRoot.timestamp
            color: delegateRoot.colors.textMuted
            font.pixelSize: delegateRoot.hostWindow.dp(10)
        }

        Rectangle {
            Layout.alignment: delegateRoot.isUser ? Qt.AlignRight : Qt.AlignLeft
            Layout.preferredWidth: Math.min(delegateRoot.maxBubbleWidth,
                                            messageText.implicitWidth
                                            + delegateRoot.hostWindow.dp(24))
            Layout.maximumWidth: delegateRoot.maxBubbleWidth
            Layout.preferredHeight: messageText.implicitHeight + delegateRoot.hostWindow.dp(18)
            radius: delegateRoot.colors.cornerRadiusMedium
            color: delegateRoot.isUser
                   ? delegateRoot.colors.accentSoft
                   : delegateRoot.colors.surfaceAlternative
            border.width: delegateRoot.isUser ? 0 : 1
            border.color: delegateRoot.colors.border

            Text {
                id: messageText
                anchors.centerIn: parent
                width: parent.width - delegateRoot.hostWindow.dp(24)
                wrapMode: Text.WordWrap
                text: delegateRoot.status === "generating" && delegateRoot.content.length === 0
                      ? qsTr("Generating response…")
                      : delegateRoot.content
                font.italic: delegateRoot.status === "generating" && delegateRoot.content.length === 0
                color: delegateRoot.colors.textPrimary
                font.pixelSize: delegateRoot.hostWindow.dp(12.5)
            }
        }

        RowLayout {
            Layout.alignment: delegateRoot.isUser ? Qt.AlignRight : Qt.AlignLeft
            spacing: delegateRoot.hostWindow.dp(4)

            Text {
                visible: delegateRoot.status === "sending"
                text: qsTr("Sending…")
                color: delegateRoot.colors.textMuted
                font.pixelSize: delegateRoot.hostWindow.dp(10)
            }

            Text {
                visible: delegateRoot.status === "stopped"
                text: qsTr("Generation stopped")
                color: delegateRoot.colors.warning
                font.pixelSize: delegateRoot.hostWindow.dp(10)
            }

            Text {
                visible: delegateRoot.isAssistant && delegateRoot.tokenCount > 0
                text: qsTr("%1 tokens").arg(delegateRoot.tokenCount)
                color: delegateRoot.colors.textMuted
                font.pixelSize: delegateRoot.hostWindow.dp(10)
            }

            Text {
                visible: delegateRoot.isAssistant && delegateRoot.contextFingerprint.length > 0
                text: qsTr("Project context %1 · ~%2 input tokens")
                      .arg(delegateRoot.contextFingerprint.slice(0, 12)).arg(delegateRoot.contextTokens)
                color: delegateRoot.colors.textMuted
                font.pixelSize: delegateRoot.hostWindow.dp(10)
            }

            Item { Layout.preferredWidth: delegateRoot.hostWindow.dp(4) }

            ChatIconButton {
                visible: !delegateRoot.isProcessing
                hostWindow: delegateRoot.hostWindow
                colors: delegateRoot.colors
                iconGlyph: "\u29C9"
                accessibleLabel: qsTr("Copy message")
                implicitWidth: delegateRoot.hostWindow.dp(24)
                implicitHeight: delegateRoot.hostWindow.dp(24)
                onClicked: delegateRoot.requestCopy()
            }

            ChatIconButton {
                visible: delegateRoot.isAssistant && !delegateRoot.isProcessing
                hostWindow: delegateRoot.hostWindow
                colors: delegateRoot.colors
                iconGlyph: "\u21BB"
                accessibleLabel: qsTr("Retry response")
                implicitWidth: delegateRoot.hostWindow.dp(24)
                implicitHeight: delegateRoot.hostWindow.dp(24)
                onClicked: delegateRoot.retryRequested(delegateRoot.messageId)
            }

            ChatIconButton {
                visible: delegateRoot.isUser && !delegateRoot.isProcessing
                hostWindow: delegateRoot.hostWindow
                colors: delegateRoot.colors
                iconGlyph: "\u270E"
                accessibleLabel: qsTr("Edit message")
                implicitWidth: delegateRoot.hostWindow.dp(24)
                implicitHeight: delegateRoot.hostWindow.dp(24)
                onClicked: delegateRoot.editRequested(delegateRoot.messageId, delegateRoot.content)
            }
        }

        // PC-3D: a reply that holds exactly one next-run plan offers the explicit action; the Backend has
        // already validated nothing at this point - the action only opens the preview. A reply with several
        // plan blocks, or one too large to import, says why there is no action instead of guessing.
        ChatActionButton {
            objectName: "chatMessagePlanActionButton"
            Layout.alignment: Qt.AlignLeft
            visible: delegateRoot.isAssistant && delegateRoot.planEnvelope === "one"
            hostWindow: delegateRoot.hostWindow
            colors: delegateRoot.colors
            text: qsTr("Create Run drafts…")
            implicitHeight: delegateRoot.hostWindow.dp(30)
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Review the plan in this reply and save it as Run drafts. Nothing starts.")
            onClicked: delegateRoot.planDraftsRequested(delegateRoot.messageId)
        }

        Text {
            objectName: "chatMessagePlanHint"
            Layout.alignment: Qt.AlignLeft
            Layout.maximumWidth: delegateRoot.maxBubbleWidth
            visible: delegateRoot.isAssistant
                     && (delegateRoot.planEnvelope === "multiple" || delegateRoot.planEnvelope === "too_large")
            wrapMode: Text.WordWrap
            text: delegateRoot.planEnvelope === "multiple"
                  ? qsTr("This reply holds several next-run plan blocks, so it cannot be turned into drafts. Ask the model for exactly one block.")
                  : qsTr("The next-run plan block in this reply is larger than 128 KiB and cannot be imported.")
            color: delegateRoot.colors.warning
            font.pixelSize: delegateRoot.hostWindow.dp(10)
        }
    }
}
