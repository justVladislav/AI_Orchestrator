import QtQuick
import QtQuick.Layouts

// B3-III: the project's UNFINISHED compaction protocols (Open / Unresolved), a list apart from the history of Completed / Failed /
// Cancelled records. The section comes from ONE Application read (ProjectMemoryController.compactionHistorySnapshot().intents): the
// counts and the rows are those of that read, QML never counts rows itself. An intent has no amount, no usage and no price state, so
// nothing here shows money. Open is a protocol without a final recording yet (NOT a running operation, nothing about a request); an
// unresolved one is an earlier protocol that never completed (the sentence of §15d). Its own read error is shown here and nowhere else.
// All dynamic text is plain text and wraps; the rows grow the section, the layout places the next block below.
ColumnLayout {
    id: section
    objectName: "projectMemoryUnfinishedProtocols"

    required property var hostWindow
    required property var colors
    // The "intents" map of the history snapshot, or null.
    property var snapshot: null

    readonly property bool hasSnapshot: section.snapshot !== null && section.snapshot !== undefined
    readonly property string state: section.hasSnapshot ? String(section.snapshot.state) : "none"
    readonly property bool isAvailable: section.state === "available"
    readonly property bool isUnavailable: section.state === "unavailable"
    readonly property var rows: section.isAvailable ? section.snapshot.rows : []
    readonly property string scopeText: !section.hasSnapshot ? ""
        : section.snapshot.coverage.scope === "period" ? qsTr("In the selected period") : qsTr("All time")

    Layout.fillWidth: true
    visible: section.isAvailable || section.isUnavailable
    spacing: section.hostWindow.dp(4)

    Text {
        objectName: "unfinishedProtocolsTitle"
        Layout.fillWidth: true
        Layout.topMargin: section.hostWindow.dp(4)
        text: qsTr("Unfinished compaction protocols")
        color: section.colors.textPrimary
        textFormat: Text.PlainText
        font.pixelSize: section.hostWindow.dp(13)
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "unfinishedProtocolsScope"
        Layout.fillWidth: true
        text: section.scopeText
        color: section.colors.textMuted
        textFormat: Text.PlainText
        font.pixelSize: section.hostWindow.dp(9.5)
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "unfinishedProtocolsUnavailable"
        Layout.fillWidth: true
        visible: section.isUnavailable
        text: qsTr("Compaction recording coverage unavailable: the unfinished protocols could not be read.")
        color: section.colors.error
        textFormat: Text.PlainText
        font.pixelSize: section.hostWindow.dp(10)
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "unfinishedProtocolsError"
        Layout.fillWidth: true
        visible: section.isUnavailable && section.snapshot.errorMessage.length > 0
        text: section.hasSnapshot ? section.snapshot.errorMessage : ""
        color: section.colors.error
        textFormat: Text.PlainText
        font.pixelSize: section.hostWindow.dp(10)
        wrapMode: Text.Wrap
    }

    Text {
        objectName: "unfinishedProtocolsErrorCode"
        Layout.fillWidth: true
        visible: section.isUnavailable && section.snapshot.errorCode.length > 0
        text: section.hasSnapshot ? qsTr("Error code: %1").arg(section.snapshot.errorCode) : ""
        color: section.colors.textMuted
        textFormat: Text.PlainText
        font.pixelSize: section.hostWindow.dp(10)
        wrapMode: Text.WrapAnywhere
    }

    Text {
        objectName: "unfinishedProtocolsNone"
        Layout.fillWidth: true
        visible: section.isAvailable && section.rows.length === 0
        text: qsTr("No open or unresolved compaction operations in this scope")
        color: section.colors.textMuted
        textFormat: Text.PlainText
        font.pixelSize: section.hostWindow.dp(10)
        wrapMode: Text.WordWrap
    }

    Repeater {
        model: section.rows
        delegate: Rectangle {
            id: row
            objectName: "unfinishedProtocolRow"
            required property var modelData
            readonly property bool unresolved: row.modelData.state === "unresolved"
            Layout.fillWidth: true
            Layout.preferredHeight: rowContent.implicitHeight + section.hostWindow.dp(16)
            radius: section.colors.cornerRadiusSmall
            color: section.colors.inputBackground
            border.width: 1
            border.color: row.unresolved ? section.colors.warning : section.colors.border

            ColumnLayout {
                id: rowContent
                anchors.fill: parent
                anchors.margins: section.hostWindow.dp(9)
                spacing: section.hostWindow.dp(3)

                RowLayout {
                    Layout.fillWidth: true
                    Rectangle {
                        Layout.preferredWidth: stateLabel.implicitWidth + section.hostWindow.dp(12)
                        Layout.preferredHeight: section.hostWindow.dp(19)
                        radius: height / 2
                        color: Qt.rgba(0.96, 0.73, 0.30, 0.16)
                        Text {
                            id: stateLabel
                            objectName: "unfinishedProtocolState"
                            anchors.centerIn: parent
                            text: row.unresolved ? qsTr("Unresolved") : qsTr("Open")
                            color: section.colors.warning
                            textFormat: Text.PlainText
                            font.pixelSize: section.hostWindow.dp(9)
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.modelId
                        color: section.colors.textSecondary
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        font.pixelSize: section.hostWindow.dp(9.5)
                    }
                    Text {
                        text: row.modelData.createdAt
                        color: section.colors.textMuted
                        textFormat: Text.PlainText
                        font.pixelSize: section.hostWindow.dp(9)
                    }
                }

                Text {
                    objectName: "unfinishedProtocolText"
                    Layout.fillWidth: true
                    text: row.unresolved
                          ? qsTr("An earlier compaction did not complete its recording protocol. Delivery and cost are unconfirmed; the request may not have started.")
                          : qsTr("This compaction has no confirmed final recording yet. Delivery, token usage and cost are unconfirmed.")
                    color: section.colors.textMuted
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    font.pixelSize: section.hostWindow.dp(9)
                }

                Text {
                    objectName: "unfinishedProtocolMeta"
                    Layout.fillWidth: true
                    text: row.unresolved && row.modelData.markedAt.length > 0
                          ? qsTr("From %1 segment(s) · marked unresolved %2").arg(row.modelData.sourceCount).arg(row.modelData.markedAt)
                          : qsTr("From %1 segment(s)").arg(row.modelData.sourceCount)
                    color: section.colors.textMuted
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    font.pixelSize: section.hostWindow.dp(8.5)
                }
            }
        }
    }
}
