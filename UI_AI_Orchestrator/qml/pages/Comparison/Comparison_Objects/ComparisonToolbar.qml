import QtQuick
import QtQuick.Layouts

Rectangle {
    id: toolbar

    required property var hostWindow
    required property var colors

    property var projectsModel: null
    property var sessionsModel: null
    property string layoutMode: "wide"
    property string selectedProjectId: ""
    property string selectedSessionId: ""
    property string statusText: ""
    property string lastUpdatedText: ""
    property bool busy: false
    property bool canCreateComparison: true
    property bool canRefresh: false
    property bool canCancel: false

    readonly property int projectCount: projectSelector.count
    readonly property int sessionCount: sessionSelector.count

    signal projectSelected(string projectId)
    signal sessionSelected(string sessionId)
    signal createRequested()
    signal refreshRequested()
    signal cancelRequested()

    radius: colors.cornerRadiusMedium
    color: colors.surface
    border.width: 1
    border.color: colors.border
    implicitHeight: content.implicitHeight + hostWindow.dp(24)

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: toolbar.hostWindow.dp(14)
        spacing: toolbar.hostWindow.dp(10)

        ColumnLayout {
            Layout.fillWidth: true
            spacing: toolbar.hostWindow.dp(2)

            Text {
                Layout.fillWidth: true
                text: qsTr("Comparison")
                color: toolbar.colors.textPrimary
                font.pixelSize: toolbar.hostWindow.dp(22)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Compare persisted results from multiple models for the same request.")
                color: toolbar.colors.textSecondary
                font.pixelSize: toolbar.hostWindow.dp(12.5)
                wrapMode: Text.WordWrap
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: toolbar.layoutMode === "wide" ? 3
                     : toolbar.layoutMode === "medium" ? 2 : 1
            columnSpacing: toolbar.hostWindow.dp(10)
            rowSpacing: toolbar.hostWindow.dp(8)

            ComparisonComboBox {
                id: projectSelector
                Layout.fillWidth: true
                Layout.preferredWidth: toolbar.hostWindow.dp(220)
                Layout.preferredHeight: toolbar.hostWindow.dp(36)
                hostWindow: toolbar.hostWindow
                colors: toolbar.colors
                itemsModel: toolbar.projectsModel
                itemIdRole: "projectId"
                itemLabelRole: "name"
                currentItemId: toolbar.selectedProjectId
                placeholderText: qsTr("Select project")
                onItemSelected: itemId => toolbar.projectSelected(itemId)
            }

            ComparisonComboBox {
                id: sessionSelector
                Layout.fillWidth: true
                Layout.preferredWidth: toolbar.hostWindow.dp(280)
                Layout.preferredHeight: toolbar.hostWindow.dp(36)
                enabled: count > 0 && !toolbar.busy
                hostWindow: toolbar.hostWindow
                colors: toolbar.colors
                itemsModel: toolbar.sessionsModel
                itemIdRole: "sessionId"
                itemLabelRole: "title"
                currentItemId: toolbar.selectedSessionId
                placeholderText: count > 0
                                 ? qsTr("Select comparison")
                                 : qsTr("No comparisons for this project")
                onItemSelected: itemId => toolbar.sessionSelected(itemId)
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.columnSpan: toolbar.layoutMode === "narrow" ? 1 : 1
                spacing: toolbar.hostWindow.dp(6)

                ComparisonActionButton {
                    text: toolbar.layoutMode === "wide" ? qsTr("New comparison") : qsTr("New")
                    iconGlyph: "+"
                    enabled: toolbar.canCreateComparison && toolbar.selectedProjectId.length > 0 && !toolbar.busy
                    accessibleLabel: qsTr("Create comparison")
                    hostWindow: toolbar.hostWindow
                    colors: toolbar.colors
                    onClicked: toolbar.createRequested()
                }

                ComparisonActionButton {
                    text: toolbar.layoutMode === "wide" ? qsTr("Refresh") : ""
                    iconGlyph: "\u21BB"
                    enabled: toolbar.canRefresh && toolbar.selectedSessionId.length > 0 && !toolbar.busy
                    accessibleLabel: qsTr("Refresh comparison")
                    hostWindow: toolbar.hostWindow
                    colors: toolbar.colors
                    onClicked: toolbar.refreshRequested()
                }

                ComparisonActionButton {
                    text: toolbar.layoutMode === "wide" ? qsTr("Cancel") : ""
                    iconGlyph: "\u25A0"
                    danger: true
                    enabled: toolbar.canCancel && toolbar.selectedSessionId.length > 0
                    accessibleLabel: qsTr("Cancel comparison")
                    hostWindow: toolbar.hostWindow
                    colors: toolbar.colors
                    onClicked: toolbar.cancelRequested()
                }

                Item { Layout.fillWidth: true }

                ColumnLayout {
                    Layout.maximumWidth: toolbar.hostWindow.dp(190)
                    spacing: toolbar.hostWindow.dp(1)
                    visible: toolbar.selectedSessionId.length > 0

                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        text: toolbar.busy ? qsTr("Updating…") : toolbar.statusText
                        color: toolbar.colors.textSecondary
                        font.pixelSize: toolbar.hostWindow.dp(11)
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        visible: toolbar.lastUpdatedText.length > 0
                        text: qsTr("Updated %1").arg(toolbar.lastUpdatedText)
                        color: toolbar.colors.textMuted
                        font.pixelSize: toolbar.hostWindow.dp(10)
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
