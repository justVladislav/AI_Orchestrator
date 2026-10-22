pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row inside ChatSidebar's conversation list. Built on ItemDelegate
// (rather than a plain MouseArea) so it is keyboard-focusable and
// activatable with Enter/Space out of the box.
ItemDelegate {
    id: row

    required property var hostWindow
    required property var colors

    required property int index
    required property string conversationId
    required property string title
    required property string preview
    required property string updatedAt
    required property int unreadCount
    required property bool pinned
    required property bool archived

    property bool selected: false

    signal conversationClicked(string conversationId)

    hoverEnabled: true
    height: hostWindow.dp(66)
    padding: 0

    Accessible.name: title + (unreadCount > 0
                              ? qsTr(", %1 unread").arg(unreadCount)
                              : "")

    onClicked: row.conversationClicked(row.conversationId)

    background: Rectangle {
        anchors.fill: parent
        anchors.margins: row.hostWindow.dp(4)
        radius: row.colors.cornerRadiusSmall
        color: row.selected
               ? row.colors.accentSoft
               : row.hovered || row.visualFocus
                 ? row.colors.buttonHover
                 : "transparent"
        border.width: row.visualFocus ? 1 : 0
        border.color: row.colors.focusRing
    }

    contentItem: RowLayout {
        spacing: row.hostWindow.dp(9)

        Rectangle {
            Layout.preferredWidth: row.hostWindow.dp(6)
            Layout.preferredHeight: row.hostWindow.dp(6)
            Layout.alignment: Qt.AlignVCenter
            radius: width / 2
            visible: row.archived || row.pinned
            color: row.archived ? row.colors.textMuted : row.colors.accent
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: row.hostWindow.dp(10)
            spacing: row.hostWindow.dp(2)

            RowLayout {
                Layout.fillWidth: true
                spacing: row.hostWindow.dp(6)

                Text {
                    text: row.pinned ? "\u25C6" : ""
                    visible: row.pinned
                    color: row.colors.accent
                    font.pixelSize: row.hostWindow.dp(10)
                }

                Text {
                    Layout.fillWidth: true
                    text: row.title
                    color: row.archived ? row.colors.textMuted : row.colors.textPrimary
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    wrapMode: Text.NoWrap
                    font.pixelSize: row.hostWindow.dp(13)
                    font.weight: row.selected ? Font.DemiBold : Font.Medium
                }

                Text {
                    text: row.updatedAt
                    color: row.colors.textMuted
                    font.pixelSize: row.hostWindow.dp(10)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: row.hostWindow.dp(6)

                Text {
                    Layout.fillWidth: true
                    text: row.preview.length > 0 ? row.preview : qsTr("No messages yet")
                    color: row.colors.textMuted
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    wrapMode: Text.NoWrap
                    clip: true
                    font.pixelSize: row.hostWindow.dp(11)
                }

                Rectangle {
                    visible: row.unreadCount > 0
                    Layout.preferredWidth: Math.max(row.hostWindow.dp(16),
                                                    unreadLabel.implicitWidth + row.hostWindow.dp(8))
                    Layout.preferredHeight: row.hostWindow.dp(16)
                    radius: height / 2
                    color: row.colors.accent

                    Text {
                        id: unreadLabel
                        anchors.centerIn: parent
                        text: row.unreadCount > 99 ? "99+" : String(row.unreadCount)
                        color: row.colors.textOnAccent
                        font.pixelSize: row.hostWindow.dp(9)
                        font.weight: Font.DemiBold
                    }
                }
            }
        }
    }
}
