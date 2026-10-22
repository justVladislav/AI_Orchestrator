pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: chart

    required property var hostWindow
    required property var colors
    // Pass an empty array when provider cost data is not yet available;
    // the component shows an honest empty state instead of fabricated numbers.
    required property var providerModel
    signal providerRequested(var providerData)

    implicitHeight: hostWindow.dp(326)
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    function seriesColor(index) {
        const series = [colors.accent, colors.information, colors.success,
                        colors.warning, colors.error]
        return series[index % series.length]
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: chart.hostWindow.dp(16)
        spacing: chart.hostWindow.dp(12)

        RowLayout {
            Layout.fillWidth: true
            spacing: chart.hostWindow.dp(10)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: chart.hostWindow.dp(2)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Cost by provider")
                    color: chart.colors.textPrimary
                    font.pixelSize: chart.hostWindow.dp(15)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Recorded model executions · cost and token usage")
                    color: chart.colors.textMuted
                    font.pixelSize: chart.hostWindow.dp(10)
                    elide: Text.ElideRight
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: chart.colors.separator
        }

        // ── Empty state (no provider data available from backend yet) ────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            visible: chart.providerModel.length === 0
            spacing: chart.hostWindow.dp(8)

            Item { Layout.fillHeight: true }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "$"
                color: chart.colors.textMuted
                font.pixelSize: chart.hostWindow.dp(30)
                font.weight: Font.Thin
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Cost breakdown not available yet")
                color: chart.colors.textSecondary
                font.pixelSize: chart.hostWindow.dp(12)
                font.weight: Font.Medium
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Provider cost and token aggregation will appear\nafter the first model execution.")
                color: chart.colors.textMuted
                font.pixelSize: chart.hostWindow.dp(9)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Item { Layout.fillHeight: true }
        }

        // ── Provider rows (populated when provider data is available) ────────
        Repeater {
            model: chart.providerModel

            delegate: Item {
                id: providerRow

                required property int index
                required property var modelData
                readonly property color seriesColor: chart.seriesColor(index)

                Layout.fillWidth: true
                // Bound explicitly to the real-content column below rather than
                // letting a ColumnLayout aggregate implicit height from all
                // children — the decorative bleed Rectangle below is anchored
                // (not Layout-managed), so it cannot feed back into this row's
                // own size and cause jitter on hover repaint.
                implicitHeight: content.implicitHeight

                Rectangle {
                    anchors.fill: content
                    anchors.margins: -chart.hostWindow.dp(6)
                    radius: chart.colors.cornerRadiusSmall
                    color: providerTap.hovered ? chart.colors.buttonHover : "transparent"
                    z: -1
                }

                HoverHandler { id: providerTap }
                TapHandler { onTapped: chart.providerRequested(providerRow.modelData) }

                ColumnLayout {
                    id: content
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    spacing: chart.hostWindow.dp(5)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: chart.hostWindow.dp(8)

                        Text {
                            Layout.fillWidth: true
                            text: providerRow.modelData.name
                            color: chart.colors.textSecondary
                            font.pixelSize: chart.hostWindow.dp(11)
                            elide: Text.ElideRight
                        }
                        Text {
                            text: qsTr("%1 calls").arg(providerRow.modelData.executionCount)
                            color: chart.colors.textMuted
                            font.pixelSize: chart.hostWindow.dp(9)
                        }
                        Text {
                            text: qsTr("%1 tokens").arg(providerRow.modelData.tokensLabel)
                            color: chart.colors.textMuted
                            font.pixelSize: chart.hostWindow.dp(9)
                        }
                        Text {
                            objectName: "providerCostLabel"
                            Layout.preferredWidth: chart.hostWindow.dp(112)
                            text: providerRow.modelData.costLabel
                            // An incomplete amount (a lower bound or unavailable) is drawn apart from
                            // an exact one: warning colour and italic.
                            color: providerRow.modelData.priceIncomplete
                                   ? chart.colors.warning : chart.colors.textPrimary
                            horizontalAlignment: Text.AlignRight
                            font.pixelSize: chart.hostWindow.dp(11)
                            font.weight: Font.Medium
                            font.italic: providerRow.modelData.priceIncomplete
                        }
                    }

                    // The share bar compares providers by EXACT amounts only. With any partial or
                    // unknown provider the Backend reports shareBasis "incomplete": no bar is drawn
                    // (lower bounds are never normalised against each other) and a neutral note
                    // takes its place.
                    Rectangle {
                        visible: providerRow.modelData.shareBasis !== "incomplete"
                        Layout.fillWidth: true
                        Layout.preferredHeight: chart.hostWindow.dp(7)
                        radius: height / 2
                        color: chart.colors.buttonBackground

                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, providerRow.modelData.share))
                            height: parent.height
                            radius: height / 2
                            color: providerRow.seriesColor
                        }
                    }
                    Text {
                        objectName: "incompletePricingNote"
                        visible: providerRow.modelData.shareBasis === "incomplete"
                        Layout.fillWidth: true
                        text: qsTr("Incomplete pricing")
                        color: chart.colors.textMuted
                        font.pixelSize: chart.hostWindow.dp(9)
                        font.italic: true
                    }
                }
            }
        }

        // Push provider rows toward the top; no-op when the empty state fills height.
        Item {
            Layout.fillHeight: true
            visible: chart.providerModel.length > 0
        }
    }
}
