import QtQuick
import QtQuick.Layouts

// B3-III: the READ-ONLY report of the owner-startup recovery of the compaction intents (CompactionIntentRecoveryController.report).
// Its scope is the WHOLE DATABASE - every project - and ONE moment, the start of the owner; it is not the selected project, not a
// period and not the current number of unfinished operations, so it is labelled so and never cleared by a change of project.
//   not applicable   the pass did not take part (a guest, an in-memory database, a refused lock): nothing is drawn - an absence is not an error
//   complete, nothing to report                nothing is drawn
//   complete with inspected rows / notes       a neutral summary with Details
//   partial / unavailable                      the status and the ORIGINAL first error (code, message, native detail when there is one)
// No button of any kind: nothing here retries, resends, recovers, closes or deletes. All dynamic text is plain text and wraps.
ColumnLayout {
    id: block
    objectName: "projectMemoryStartupRecovery"

    required property var hostWindow
    required property var colors
    // CompactionIntentRecoveryController.report (a map), or null for a stub.
    property var report: null
    property bool detailsOpen: false

    readonly property bool hasReport: block.report !== null && block.report !== undefined && block.report.applicable === true
    readonly property string status: block.hasReport ? String(block.report.status) : "none"
    readonly property bool needsAttention: block.hasReport && block.report.needsAttention === true
    readonly property bool shown: block.hasReport && (block.needsAttention || block.report.hasActivity === true || block.report.hasNotes === true)

    function summaryText() {
        if (!block.hasReport) return ""
        if (block.status === "unavailable")
            return qsTr("The recovery of unfinished compaction protocols could not run. Nothing was changed.")
        if (block.status === "partial")
            return qsTr("The recovery of unfinished compaction protocols finished only partly. Some operations are unconfirmed or in conflict; nothing was overwritten.")
        return qsTr("Unfinished compaction protocols were checked at startup.")
    }

    function countsText() {
        if (!block.hasReport) return ""
        return qsTr("Inspected %1 · marked unresolved %2 · already unresolved %3 · closed %4 · conflicts %5 · unconfirmed %6")
               .arg(block.report.inspected).arg(block.report.markedUnresolved).arg(block.report.alreadyUnresolved)
               .arg(block.report.closedConfirmed).arg(block.report.conflicts).arg(block.report.unconfirmed)
    }

    Layout.fillWidth: true
    visible: block.shown
    spacing: block.hostWindow.dp(4)

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: content.implicitHeight + block.hostWindow.dp(16)
        radius: block.colors.cornerRadiusSmall
        color: block.colors.inputBackground
        border.width: 1
        border.color: block.needsAttention ? block.colors.warning : block.colors.border

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: block.hostWindow.dp(9)
            spacing: block.hostWindow.dp(3)

            Text {
                objectName: "startupRecoveryScope"
                Layout.fillWidth: true
                text: qsTr("Database startup recovery — all projects")
                color: block.colors.textPrimary
                textFormat: Text.PlainText
                font.pixelSize: block.hostWindow.dp(11)
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "startupRecoveryStatus"
                Layout.fillWidth: true
                text: block.status === "complete" ? qsTr("Complete")
                      : block.status === "partial" ? qsTr("Partial") : qsTr("Unavailable")
                color: block.needsAttention ? block.colors.warning : block.colors.success
                textFormat: Text.PlainText
                font.pixelSize: block.hostWindow.dp(10)
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "startupRecoverySummary"
                Layout.fillWidth: true
                text: block.summaryText()
                color: block.colors.textSecondary
                textFormat: Text.PlainText
                font.pixelSize: block.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "startupRecoveryFirstError"
                Layout.fillWidth: true
                visible: block.hasReport && block.report.hasFirstError === true
                text: block.hasReport ? block.report.firstErrorMessage : ""
                color: block.colors.error
                textFormat: Text.PlainText
                font.pixelSize: block.hostWindow.dp(10)
                wrapMode: Text.Wrap
            }

            Text {
                objectName: "startupRecoveryFirstErrorCode"
                Layout.fillWidth: true
                visible: block.hasReport && block.report.hasFirstError === true
                text: block.hasReport ? qsTr("Error code: %1").arg(block.report.firstErrorCode) : ""
                color: block.colors.textMuted
                textFormat: Text.PlainText
                font.pixelSize: block.hostWindow.dp(10)
                wrapMode: Text.WrapAnywhere
            }

            // The native detail only when the storage really gave one (never a 0 made up from an absence).
            Text {
                objectName: "startupRecoveryFirstNative"
                Layout.fillWidth: true
                visible: block.hasReport && block.report.firstNative.present === true
                text: block.hasReport ? block.report.firstNative.text : ""
                color: block.colors.textMuted
                textFormat: Text.PlainText
                font.pixelSize: block.hostWindow.dp(9.5)
                wrapMode: Text.Wrap
            }

            Text {
                objectName: "startupRecoveryDetailsToggle"
                Layout.fillWidth: true
                text: block.detailsOpen ? qsTr("Hide details") : qsTr("Details")
                color: block.colors.accent
                textFormat: Text.PlainText
                font.pixelSize: block.hostWindow.dp(10)
                font.underline: true
                TapHandler { onTapped: block.detailsOpen = !block.detailsOpen }
            }

            ColumnLayout {
                id: details
                objectName: "startupRecoveryDetails"
                Layout.fillWidth: true
                visible: block.detailsOpen
                spacing: block.hostWindow.dp(3)

                Text {
                    objectName: "startupRecoveryCounts"
                    Layout.fillWidth: true
                    text: block.countsText()
                    color: block.colors.textSecondary
                    textFormat: Text.PlainText
                    font.pixelSize: block.hostWindow.dp(9.5)
                    wrapMode: Text.WordWrap
                }

                Text {
                    objectName: "startupRecoveryScopeNote"
                    Layout.fillWidth: true
                    text: qsTr("This is the report of the last start of the owner of the database (all projects). It is not the current number of unfinished operations.")
                    color: block.colors.textMuted
                    textFormat: Text.PlainText
                    font.pixelSize: block.hostWindow.dp(9)
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: block.hasReport ? block.report.diagnostics : []
                    delegate: Text {
                        id: diagnostic
                        objectName: "startupRecoveryDiagnostic"
                        required property var modelData
                        Layout.fillWidth: true
                        text: qsTr("%1 · %2: %3").arg(diagnostic.modelData.stageText).arg(diagnostic.modelData.code).arg(diagnostic.modelData.message)
                        color: block.colors.textMuted
                        textFormat: Text.PlainText
                        font.pixelSize: block.hostWindow.dp(9)
                        wrapMode: Text.Wrap
                    }
                }

                // Each write error together with ITS native detail (also when the transition was later confirmed).
                Repeater {
                    model: block.hasReport ? block.report.writeCauses : []
                    delegate: Text {
                        id: writeCause
                        objectName: "startupRecoveryWriteCause"
                        required property var modelData
                        Layout.fillWidth: true
                        text: writeCause.modelData.native.present === true
                              ? qsTr("Write error %1: %2 — %3").arg(writeCause.modelData.code).arg(writeCause.modelData.message).arg(writeCause.modelData.native.text)
                              : qsTr("Write error %1: %2").arg(writeCause.modelData.code).arg(writeCause.modelData.message)
                        color: block.colors.textMuted
                        textFormat: Text.PlainText
                        font.pixelSize: block.hostWindow.dp(9)
                        wrapMode: Text.Wrap
                    }
                }

                Text {
                    objectName: "startupRecoveryIncomplete"
                    Layout.fillWidth: true
                    visible: block.hasReport && block.report.diagnosticsIncomplete === true
                    text: qsTr("The diagnostics are incomplete: at least one cause was lost or shortened.")
                    color: block.colors.warning
                    textFormat: Text.PlainText
                    font.pixelSize: block.hostWindow.dp(9)
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
