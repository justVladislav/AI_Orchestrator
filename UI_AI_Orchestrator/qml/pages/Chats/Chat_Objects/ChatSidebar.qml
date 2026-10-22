pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Rectangle {
    id: sidebar

    required property var hostWindow
    required property var colors
    required property var conversationsModel

    property string selectedConversationId: ""

    signal newConversationRequested()
    signal conversationSelected(string conversationId)

    component FilterChip: AbstractButton {
        id: chip

        required property var hostWindow
        required property var colors
        required property string filterLabel
        property bool isActive: false

        Layout.fillWidth: true
        implicitHeight: hostWindow.dp(27)
        hoverEnabled: true
        checkable: true
        checked: isActive

        Accessible.name: filterLabel

        contentItem: Text {
            text: chip.filterLabel
            color: chip.isActive ? chip.colors.accent : chip.colors.textSecondary
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: chip.hostWindow.dp(11)
            font.weight: chip.isActive ? Font.DemiBold : Font.Medium
        }

        background: Rectangle {
            radius: chip.colors.cornerRadiusSmall
            color: chip.isActive
                   ? chip.colors.accentSoft
                   : chip.hovered
                     ? chip.colors.buttonHover
                     : "transparent"
            border.width: chip.visualFocus ? 1 : 0
            border.color: chip.colors.focusRing
        }
    }

    property string activeFilter: "all"
    property string searchText: ""

    function matchesConversation(title, preview, archived) {
        const query = searchText.trim().toLowerCase()
        const matchesText = query.length === 0
                            || title.toLowerCase().includes(query)
                            || preview.toLowerCase().includes(query)
        const matchesFilter = activeFilter === "all"
                              || (activeFilter === "active" && !archived)
                              || (activeFilter === "archived" && archived)

        return matchesText && matchesFilter
    }

    // Computed independently from ListView.count, which still includes
    // zero-height hidden delegates. This keeps the "no search results"
    // state accurate regardless of how filtering is rendered.
    function visibleConversationCount() {
        let count = 0

        for (let i = 0; i < conversationsModel.count; ++i) {
            const item = conversationsModel.get(i)

            if (matchesConversation(item.title, item.preview, item.archived))
                count += 1
        }

        return count
    }

    readonly property int visibleCount: visibleConversationCount()

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border
    clip: true

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: sidebar.hostWindow.dp(12)
        spacing: sidebar.hostWindow.dp(10)

        RowLayout {
            Layout.fillWidth: true
            spacing: sidebar.hostWindow.dp(8)

            Text {
                Layout.fillWidth: true
                text: qsTr("Chats")
                color: sidebar.colors.textPrimary
                font.pixelSize: sidebar.hostWindow.dp(17)
                font.weight: Font.DemiBold
            }

            ChatActionButton {
                text: qsTr("New chat")
                iconGlyph: "+"
                primary: true
                hostWindow: sidebar.hostWindow
                colors: sidebar.colors
                onClicked: sidebar.newConversationRequested()
            }
        }

        Basic.TextField {
            id: searchField
            Layout.fillWidth: true
            Layout.preferredHeight: sidebar.hostWindow.dp(34)
            placeholderText: qsTr("Search conversations")
            color: sidebar.colors.textPrimary
            placeholderTextColor: sidebar.colors.textMuted
            font.pixelSize: sidebar.hostWindow.dp(12.5)
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter
            leftPadding: sidebar.hostWindow.dp(10)
            rightPadding: sidebar.hostWindow.dp(10)

            onTextChanged: sidebar.searchText = text

            background: Rectangle {
                radius: sidebar.colors.cornerRadiusSmall
                color: sidebar.colors.inputBackground
                border.width: searchField.activeFocus ? 1 : 0
                border.color: sidebar.colors.inputBorderFocused
            }

            Accessible.name: qsTr("Search conversations")
        }

        RowLayout {
            id: filterRow
            Layout.fillWidth: true
            spacing: sidebar.hostWindow.dp(6)

            FilterChip {
                hostWindow: sidebar.hostWindow
                colors: sidebar.colors
                filterLabel: qsTr("All")
                isActive: sidebar.activeFilter === "all"
                onClicked: sidebar.activeFilter = "all"
            }

            FilterChip {
                hostWindow: sidebar.hostWindow
                colors: sidebar.colors
                filterLabel: qsTr("Active")
                isActive: sidebar.activeFilter === "active"
                onClicked: sidebar.activeFilter = "active"
            }

            FilterChip {
                hostWindow: sidebar.hostWindow
                colors: sidebar.colors
                filterLabel: qsTr("Archived")
                isActive: sidebar.activeFilter === "archived"
                onClicked: sidebar.activeFilter = "archived"
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: sidebar.colors.separator
        }

        ListView {
            id: conversationList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: sidebar.conversationsModel
            boundsBehavior: Flickable.StopAtBounds
            spacing: sidebar.hostWindow.dp(2)

            ScrollBar.vertical: ScrollBar { id: conversationScrollBar }

            delegate: ChatConversationDelegate {
                id: delegateRoot

                width: Math.max(0, conversationList.width - conversationScrollBar.width
                                - sidebar.hostWindow.dp(4))
                hostWindow: sidebar.hostWindow
                colors: sidebar.colors
                selected: conversationId === sidebar.selectedConversationId
                visible: sidebar.matchesConversation(title, preview, archived)
                height: delegateRoot.visible ? sidebar.hostWindow.dp(66) : 0

                onConversationClicked: id => sidebar.conversationSelected(id)
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - sidebar.hostWindow.dp(32)
                spacing: sidebar.hostWindow.dp(4)
                visible: sidebar.conversationsModel.count === 0
                         || sidebar.visibleCount === 0

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: sidebar.colors.textMuted
                    font.pixelSize: sidebar.hostWindow.dp(12.5)
                    text: sidebar.conversationsModel.count === 0
                          ? qsTr("No chats yet")
                          : qsTr("No conversations match your search")
                }

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    visible: sidebar.conversationsModel.count === 0
                    color: sidebar.colors.textMuted
                    font.pixelSize: sidebar.hostWindow.dp(11)
                    text: qsTr("Start a new conversation to begin.")
                }
            }
        }
    }
}
