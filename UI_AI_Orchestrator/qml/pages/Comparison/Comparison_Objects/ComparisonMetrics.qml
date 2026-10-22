import QtQuick
import QtQuick.Layouts

Item {
    id: metrics

    required property var hostWindow
    required property var colors

    property int inputTokens: 0
    property int outputTokens: 0
    property string totalCostText: "—"
    property string latencyText: "—"
    property bool qualityAvailable: false
    property real qualityScore: 0
    property string qualitySource: ""

    readonly property real normalizedQuality: Math.max(0, Math.min(100, qualityScore))

    implicitHeight: grid.implicitHeight

    GridLayout {
        id: grid
        width: parent.width
        columns: 2
        columnSpacing: metrics.hostWindow.dp(14)
        rowSpacing: metrics.hostWindow.dp(6)

        Text {
            Layout.maximumWidth: metrics.width * 0.55
            text: qsTr("Tokens (in / out)")
            color: metrics.colors.textMuted
            font.pixelSize: metrics.hostWindow.dp(10)
            elide: Text.ElideRight
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            text: qsTr("%1 / %2").arg(metrics.inputTokens).arg(metrics.outputTokens)
            color: metrics.colors.textPrimary
            font.pixelSize: metrics.hostWindow.dp(11)
            elide: Text.ElideLeft
        }

        Text {
            text: qsTr("Cost")
            color: metrics.colors.textMuted
            font.pixelSize: metrics.hostWindow.dp(10)
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            text: metrics.totalCostText
            color: metrics.colors.textPrimary
            font.pixelSize: metrics.hostWindow.dp(11)
            elide: Text.ElideLeft
        }

        Text {
            text: qsTr("Latency")
            color: metrics.colors.textMuted
            font.pixelSize: metrics.hostWindow.dp(10)
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            text: metrics.latencyText
            color: metrics.colors.textPrimary
            font.pixelSize: metrics.hostWindow.dp(11)
            elide: Text.ElideLeft
        }

        Text {
            Layout.maximumWidth: metrics.width * 0.55
            text: metrics.qualitySource.length > 0
                  ? qsTr("Quality · %1").arg(metrics.qualitySource)
                  : qsTr("Quality score")
            color: metrics.colors.textMuted
            font.pixelSize: metrics.hostWindow.dp(10)
            elide: Text.ElideRight
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: metrics.hostWindow.dp(6)

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: metrics.hostWindow.dp(6)
                radius: height / 2
                color: metrics.colors.border
                opacity: metrics.qualityAvailable ? 1.0 : 0.55

                Rectangle {
                    width: metrics.qualityAvailable
                           ? parent.width * metrics.normalizedQuality / 100 : 0
                    height: parent.height
                    radius: height / 2
                    color: metrics.normalizedQuality >= 70
                           ? metrics.colors.success
                           : metrics.normalizedQuality >= 40
                             ? metrics.colors.warning
                             : metrics.colors.error
                }
            }

            Text {
                text: metrics.qualityAvailable
                      ? Number(metrics.normalizedQuality).toFixed(0) : "—"
                color: metrics.qualityAvailable
                       ? metrics.colors.textPrimary : metrics.colors.textMuted
                font.pixelSize: metrics.hostWindow.dp(11)
                font.weight: Font.DemiBold
            }
        }
    }
}
