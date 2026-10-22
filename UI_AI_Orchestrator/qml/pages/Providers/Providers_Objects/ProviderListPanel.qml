pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var providersModel

    property int selectedIndex: -1
    signal selectionRequested(int index)

    readonly property string statusFilter: statusCombo.currentIndex === 1
                                           ? "active"
                                           : statusCombo.currentIndex === 2
                                             ? "inactive"
                                             : "all"
    readonly property int matchingCount: {
        const countDependency = providersModel.length
        const query = searchField.text.trim().toLowerCase()
        let matches = 0
        for (let i = 0; i < providersModel.length; ++i) {
            const provider = providersModel[i]
            const statusMatches = statusFilter === "all"
                                  || (statusFilter === "active" && provider.isEnabled)
                                  || (statusFilter === "inactive" && !provider.isEnabled)
            const textMatches = query.length === 0
                                || provider.name.toLowerCase().includes(query)
                                || provider.type.toLowerCase().includes(query)
            if (statusMatches && textMatches)
                ++matches
        }
        return matches + countDependency * 0
    }

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border
    clip: true

    function matchesProvider(providerName, providerType, isEnabled) {
        const query = searchField.text.trim().toLowerCase()
        const statusMatches = statusFilter === "all"
                              || (statusFilter === "active" && isEnabled)
                              || (statusFilter === "inactive" && !isEnabled)
        const textMatches = query.length === 0
                            || providerName.toLowerCase().includes(query)
                            || providerType.toLowerCase().includes(query)
        return statusMatches && textMatches
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: panel.hostWindow.dp(14)
            spacing: panel.hostWindow.dp(9)

            RowLayout {
                Layout.fillWidth: true

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Configured providers")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(14)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                Text {
                    text: qsTr("%1 total").arg(panel.providersModel.length)
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(10)
                }
            }

            Basic.TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.preferredHeight: panel.hostWindow.dp(36)
                placeholderText: qsTr("Search providers")
                color: panel.colors.textPrimary
                placeholderTextColor: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(11)
                selectByMouse: true
                leftPadding: panel.hostWindow.dp(11)
                rightPadding: panel.hostWindow.dp(11)

                background: Rectangle {
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: searchField.activeFocus ? 1 : 0
                    border.color: panel.colors.inputBorderFocused
                }
            }

            ProviderComboBox {
                id: statusCombo
                Layout.fillWidth: true
                Layout.preferredHeight: panel.hostWindow.dp(36)
                hostWindow: panel.hostWindow
                colors: panel.colors
                model: [qsTr("All providers"), qsTr("Active"), qsTr("Inactive")]
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: panel.colors.separator
        }

        ListView {
            id: providerList
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: panel.providersModel
            clip: true
            currentIndex: panel.selectedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { id: providerListScrollBar }

            delegate: Item {
                id: providerItem

                required property int index
                required property string name
                required property string type
                required property string status
                required property bool isEnabled
                required property int modelsCount
                required property string lastCheck

                readonly property bool matches: panel.matchesProvider(name, type, isEnabled)
                readonly property bool selected: index === panel.selectedIndex

                width: providerList.width
                height: matches ? panel.hostWindow.dp(72) : 0
                visible: matches

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: panel.hostWindow.dp(5)
                    radius: panel.colors.cornerRadiusSmall
                    color: providerItem.selected
                           ? panel.colors.accentSoft
                           : providerMouse.containsMouse
                             ? panel.colors.buttonHover
                             : "transparent"
                    border.width: providerItem.selected ? 1 : 0
                    border.color: panel.colors.accent
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: panel.hostWindow.dp(15)
                    anchors.rightMargin: panel.hostWindow.dp(15) + providerListScrollBar.width
                    spacing: panel.hostWindow.dp(10)

                    Rectangle {
                        Layout.preferredWidth: panel.hostWindow.dp(9)
                        Layout.preferredHeight: panel.hostWindow.dp(9)
                        radius: width / 2
                        color: !providerItem.isEnabled
                               ? panel.colors.textDisabled
                               : providerItem.status === "error"
                                 ? panel.colors.error
                                 : panel.colors.success
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(2)

                        RowLayout {
                            Layout.fillWidth: true

                            Text {
                                Layout.fillWidth: true
                                text: providerItem.name
                                color: panel.colors.textPrimary
                                font.pixelSize: panel.hostWindow.dp(12)
                                font.weight: providerItem.selected ? Font.DemiBold : Font.Medium
                                elide: Text.ElideRight
                            }

                            Text {
                                text: qsTr("%1 models").arg(providerItem.modelsCount)
                                color: panel.colors.textMuted
                                font.pixelSize: panel.hostWindow.dp(9)
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: providerItem.type + " · " + providerItem.lastCheck
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(9)
                            elide: Text.ElideRight
                        }
                    }
                }

                MouseArea {
                    id: providerMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.selectionRequested(providerItem.index)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: panel.matchingCount === 0
                text: panel.providersModel.length === 0
                      ? qsTr("No providers configured")
                      : qsTr("No matching providers")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(11)
            }
        }
    }
}
