pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Shows real run activity for the selected project instead of a fake stage list.
// All properties are optional so OverviewPage can bind them from the live model
// without crashing when no project is selected.
Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property string projectName

    // Derived from real runs for this project (all default to zero/empty).
    property real    progress:        0.0
    property string  activeRunTitle:  ""
    property string  activeRunStatus: ""
    property int     runCount:        0
    property int     completedCount:  0
    property int     failedCount:     0
    property int     cancelledCount:  0
    property int     queuedCount:     0
    property var     activityModel:   []

    readonly property bool hasActiveRun: activeRunStatus === "running"
                                      || activeRunStatus === "queued"
                                      || activeRunStatus === "cancelling"
    readonly property bool hasAnyRun: runCount > 0
    readonly property bool hasAnyActivity: hasAnyRun || activityModel.length > 0

    implicitHeight: hostWindow.dp(312)
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(16)
        spacing: panel.hostWindow.dp(10)

        Text {
            Layout.fillWidth: true
            text: qsTr("Project activity")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(15)
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        Text {
            Layout.fillWidth: true
            text: panel.projectName
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(11)
            elide: Text.ElideRight
        }

        // ── No runs yet ─────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !panel.hasAnyActivity
            spacing: panel.hostWindow.dp(8)

            Item { Layout.fillHeight: true }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "○"
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(28)
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("No runs yet")
                color: panel.colors.textSecondary
                font.pixelSize: panel.hostWindow.dp(12)
                font.weight: Font.Medium
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Start an orchestration run from Projects.")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(9)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Item { Layout.fillHeight: true }
        }

        // ── Active / recent run ──────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.hasAnyActivity
            spacing: panel.hostWindow.dp(8)

            // Active run name + percentage
            RowLayout {
                Layout.fillWidth: true
                visible: panel.hasActiveRun
                spacing: panel.hostWindow.dp(8)

                Text {
                    Layout.fillWidth: true
                    text: panel.activeRunTitle.length > 0
                          ? panel.activeRunTitle : qsTr("Active run")
                    color: panel.colors.accent
                    font.pixelSize: panel.hostWindow.dp(12)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    text: Math.round(panel.progress * 100) + "%"
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(11)
                    font.weight: Font.DemiBold
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: panel.colors.separator }

            Text {
                Layout.fillWidth: true
                text: qsTr("Recent activity")
                color: panel.colors.textSecondary
                font.pixelSize: panel.hostWindow.dp(10)
                font.weight: Font.DemiBold
            }

            Repeater {
                model: panel.activityModel.slice(0, 3)
                delegate: RowLayout {
                    id: activityRow
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(34)
                    spacing: panel.hostWindow.dp(8)
                    Rectangle {
                        Layout.preferredWidth: panel.hostWindow.dp(7)
                        Layout.preferredHeight: width
                        radius: width / 2
                        color: activityRow.modelData.color
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: activityRow.modelData.title
                            color: panel.colors.textPrimary
                            font.pixelSize: panel.hostWindow.dp(9.5)
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: activityRow.modelData.detail
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(8.5)
                            elide: Text.ElideRight
                        }
                    }
                    Text {
                        text: activityRow.modelData.time
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8)
                    }
                }
            }

            // Progress bar (only when a run is active)
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: panel.hostWindow.dp(7)
                visible: panel.hasActiveRun
                radius: height / 2
                color: panel.colors.buttonBackground

                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, panel.progress))
                    height: parent.height
                    radius: height / 2
                    color: panel.colors.accent
                }
            }

            // No active run (but project has history)
            Text {
                Layout.fillWidth: true
                visible: !panel.hasActiveRun && panel.hasAnyRun
                text: qsTr("No active run")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(11)
                elide: Text.ElideRight
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: panel.colors.separator
            }

            // ── Run count breakdown ──────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(6)

                Repeater {
                    model: [
                        { label: qsTr("Total"),     value: panel.runCount,       color: panel.colors.textSecondary },
                        { label: qsTr("Done"),      value: panel.completedCount,  color: panel.colors.success       },
                        { label: qsTr("Failed"),    value: panel.failedCount,     color: panel.colors.error         },
                        { label: qsTr("Cancelled"), value: panel.cancelledCount,  color: panel.colors.textMuted     }
                    ]
                    delegate: ColumnLayout {
                        id: statItem
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(2)
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: String(statItem.modelData.value)
                            color: statItem.modelData.color
                            font.pixelSize: panel.hostWindow.dp(16)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: statItem.modelData.label
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(8)
                        }
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
