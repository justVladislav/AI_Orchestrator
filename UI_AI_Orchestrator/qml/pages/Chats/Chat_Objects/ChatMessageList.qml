pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: list

    required property var hostWindow
    required property var colors
    required property var messagesModel

    property bool conversationSelected: false
    property bool isLoading: false
    property bool providerUnavailable: false

    signal copyRequested(string messageId, string content)
    signal retryRequested(string messageId)
    signal editRequested(string messageId, string content)
    signal planDraftsRequested(string messageId)

    Rectangle {
        anchors.fill: parent
        radius: list.colors.cornerRadiusMedium
        color: list.colors.surface
        border.width: 1
        border.color: list.colors.border
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: list.hostWindow.dp(34)
                visible: list.conversationSelected && list.providerUnavailable
                color: list.colors.errorSoft

                Text {
                    anchors.centerIn: parent
                    text: qsTr("The selected provider is unavailable. Choose another provider to continue.")
                    color: list.colors.error
                    font.pixelSize: list.hostWindow.dp(11)
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Text {
                    anchors.centerIn: parent
                    width: parent.width * 0.7
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    visible: !list.conversationSelected
                    text: qsTr("Select a conversation, or start a new one to begin.")
                    color: list.colors.textMuted
                    font.pixelSize: list.hostWindow.dp(13)
                }

                Text {
                    anchors.centerIn: parent
                    visible: list.conversationSelected && list.isLoading
                    text: qsTr("Loading messages…")
                    color: list.colors.textMuted
                    font.pixelSize: list.hostWindow.dp(13)
                }

                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width * 0.7
                    spacing: list.hostWindow.dp(4)
                    visible: list.conversationSelected && !list.isLoading && list.messagesModel.count === 0

                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("This conversation is empty")
                        color: list.colors.textPrimary
                        font.pixelSize: list.hostWindow.dp(13)
                        font.weight: Font.DemiBold
                    }

                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("Send a message below to start the conversation.")
                        color: list.colors.textMuted
                        font.pixelSize: list.hostWindow.dp(11.5)
                    }
                }

                ListView {
                    id: messageView
                    anchors.fill: parent
                    anchors.margins: list.hostWindow.dp(14)
                    visible: list.conversationSelected && !list.isLoading && list.messagesModel.count > 0
                    clip: true
                    spacing: list.hostWindow.dp(6)
                    model: list.messagesModel
                    boundsBehavior: Flickable.StopAtBounds

                    ScrollBar.vertical: ScrollBar { id: messageScrollBar }

                    // The page refreshes by rebuilding the whole model, which
                    // resets contentY, and count changes fire before delegates
                    // have measured their heights — so the view must re-anchor
                    // to the end once contentHeight settles. But contentHeight
                    // also grows lazily while scrolling back through history,
                    // so anchoring is applied only while the user is already
                    // following the bottom (stickToBottom); scrolling up
                    // releases it, returning to the end re-engages it.
                    property bool stickToBottom: true

                    function scrollToEndIfSticky() {
                        if (messageView.stickToBottom)
                            messageView.positionViewAtEnd()
                    }

                    onContentYChanged: {
                        if (messageView.moving || messageView.dragging || messageScrollBar.pressed)
                            messageView.stickToBottom = messageView.atYEnd
                    }

                    onCountChanged: {
                        if (messageView.count === 0)
                            messageView.stickToBottom = true
                        Qt.callLater(messageView.scrollToEndIfSticky)
                    }
                    onContentHeightChanged: Qt.callLater(messageView.scrollToEndIfSticky)

                    delegate: ChatMessageDelegate {
                        width: Math.max(0, messageView.width - messageScrollBar.width
                                        - list.hostWindow.dp(4))
                        hostWindow: list.hostWindow
                        colors: list.colors

                        onCopyRequested: (messageId, content) => list.copyRequested(messageId, content)
                        onRetryRequested: messageId => list.retryRequested(messageId)
                        onEditRequested: (messageId, content) => list.editRequested(messageId, content)
                        onPlanDraftsRequested: messageId => list.planDraftsRequested(messageId)
                    }
                }
            }
        }
    }
}
