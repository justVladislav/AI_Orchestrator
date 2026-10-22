import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: header

    required property var hostWindow
    required property var colors

    property bool hasConversation: false
    property string conversationTitle: ""
    property string providerName: ""
    property string modelName: ""
    property bool providerAvailable: true

    property bool showBackButton: false
    property bool showSessionButton: false
    // PC-3D: a manual paste of a next-run plan needs a project (the drafts belong to the conversation's project).
    property bool canImportPlan: false

    signal importPlanRequested()
    signal backRequested()
    signal sessionPanelRequested()
    signal deleteConversationRequested()

    color: colors.surface
    radius: colors.cornerRadiusMedium
    border.width: 1
    border.color: colors.border

    implicitHeight: hostWindow.dp(56)

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: header.hostWindow.dp(12)
        anchors.rightMargin: header.hostWindow.dp(12)
        spacing: header.hostWindow.dp(10)

        ChatIconButton {
            visible: header.showBackButton
            hostWindow: header.hostWindow
            colors: header.colors
            iconGlyph: "\u2190"
            accessibleLabel: qsTr("Back to conversation list")
            onClicked: header.backRequested()
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: header.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: header.hasConversation
                      ? header.conversationTitle
                      : qsTr("No conversation selected")
                color: header.colors.textPrimary
                elide: Text.ElideRight
                font.pixelSize: header.hostWindow.dp(15)
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: header.hostWindow.dp(6)
                visible: header.hasConversation

                Rectangle {
                    Layout.preferredWidth: header.hostWindow.dp(7)
                    Layout.preferredHeight: header.hostWindow.dp(7)
                    radius: width / 2
                    color: header.providerAvailable ? header.colors.success : header.colors.error
                }

                Text {
                    Layout.fillWidth: true
                    text: header.providerName.length > 0
                          ? header.providerName + " · " + header.modelName
                            + (header.providerAvailable ? "" : " — " + qsTr("Unavailable"))
                          : qsTr("No provider selected")
                    color: header.providerAvailable ? header.colors.textSecondary : header.colors.error
                    elide: Text.ElideRight
                    font.pixelSize: header.hostWindow.dp(11)
                }
            }
        }

        ChatActionButton {
            objectName: "chatImportPlanButton"
            visible: header.hasConversation
            enabled: header.canImportPlan
            hostWindow: header.hostWindow
            colors: header.colors
            text: qsTr("Import next-run plan")
            implicitHeight: header.hostWindow.dp(32)
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: header.canImportPlan
                          ? qsTr("Paste a next-run plan, review it and save it as Run drafts. Nothing starts.")
                          : qsTr("Select a project for this conversation first: the drafts belong to its project.")
            onClicked: header.importPlanRequested()
        }

        ChatIconButton {
            visible: header.showSessionButton && header.hasConversation
            hostWindow: header.hostWindow
            colors: header.colors
            iconGlyph: "\u25A4"
            accessibleLabel: qsTr("Session details")
            onClicked: header.sessionPanelRequested()
        }

        ChatIconButton {
            visible: header.hasConversation
            hostWindow: header.hostWindow
            colors: header.colors
            danger: true
            iconGlyph: "\u2715"
            accessibleLabel: qsTr("Delete conversation")
            onClicked: header.deleteConversationRequested()
        }
    }
}
