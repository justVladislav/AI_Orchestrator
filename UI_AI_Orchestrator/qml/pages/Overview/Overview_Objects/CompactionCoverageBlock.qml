import QtQuick
import QtQuick.Layouts

// B3-III: the recording COVERAGE of the project's memory compactions, apart from the money. QML only WORDS what the Qt layer reports
// (ProjectCostController.coverage, see ai::ui::compaction_coverage::coverageMap): it never counts rows, never turns an absent count into
// a zero and never reads a state as another one.
//   none         nothing was measured (no project, or the money read failed)      nothing is drawn
//   unavailable  the intent read failed: "Compaction recording coverage unavailable" + the ORIGINAL code and message
//   available    0 / 0: "No open or unresolved compaction operations in this scope" (not "everything is recorded")
//                counts > 0: the counts + one warning that the final recording, delivery, usage and cost are unconfirmed
// The scope is the question that was asked ("All time" or "In the selected period"), also when the read failed.
// "Open" is a protocol without a final recording yet - NOT a running operation; "unresolved" is an earlier one that never completed -
// NOT a proven paid failure. All dynamic text is plain text and wraps; nothing is elided or scaled.
ColumnLayout {
    id: block
    objectName: "compactionCoverageBlock"

    required property var hostWindow
    required property var colors
    // ProjectCostController.coverage - a map; may be undefined for a stub, which draws nothing.
    property var coverage: null

    readonly property bool hasMap: block.coverage !== null && block.coverage !== undefined
    readonly property string state: block.hasMap ? String(block.coverage.state) : "none"
    readonly property bool isAvailable: block.state === "available"
    readonly property bool isUnavailable: block.state === "unavailable"
    readonly property bool hasCounts: block.isAvailable && block.coverage.hasCounts === true
    readonly property real openCount: block.hasCounts ? Number(block.coverage.openOperations) : 0
    readonly property real unresolvedCount: block.hasCounts ? Number(block.coverage.unresolvedOperations) : 0
    readonly property bool hasOperations: block.hasCounts && (block.openCount > 0 || block.unresolvedCount > 0)
    readonly property string scopeText: !block.hasMap ? "" : block.coverage.scope === "period" ? qsTr("In the selected period") : qsTr("All time")

    Layout.fillWidth: true
    visible: block.isAvailable || block.isUnavailable
    spacing: block.hostWindow.dp(2)

    Text {
        objectName: "compactionCoverageTitle"
        Layout.fillWidth: true
        text: qsTr("Compaction recording coverage")
        color: block.colors.textSecondary
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(10)
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "compactionCoverageScope"
        Layout.fillWidth: true
        text: block.scopeText
        color: block.colors.textMuted
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(10)
        wrapMode: Text.WordWrap
    }

    // The counts, only when the Application confirmed them (never a zero made up for an unavailable read).
    Text {
        objectName: "compactionCoverageCounts"
        Layout.fillWidth: true
        visible: block.hasOperations
        text: qsTr("Open protocols: %1 · Unresolved compaction operations: %2")
              .arg(block.openCount).arg(block.unresolvedCount)
        color: block.colors.textPrimary
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(11)
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "compactionCoverageNone"
        Layout.fillWidth: true
        visible: block.hasCounts && !block.hasOperations
        text: qsTr("No open or unresolved compaction operations in this scope")
        color: block.colors.textMuted
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(10)
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "compactionCoverageWarning"
        Layout.fillWidth: true
        visible: block.hasOperations
        text: qsTr("These operations have no confirmed final recording; delivery, token usage and costs are unconfirmed. A request may not have started.")
        color: block.colors.warning
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(10)
        font.italic: true
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "compactionCoverageUnavailable"
        Layout.fillWidth: true
        visible: block.isUnavailable
        text: qsTr("Compaction recording coverage unavailable")
        color: block.colors.warning
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(10)
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Text {
        objectName: "compactionCoverageErrorMessage"
        Layout.fillWidth: true
        visible: block.isUnavailable && block.coverage.errorMessage.length > 0
        text: block.hasMap ? block.coverage.errorMessage : ""
        color: block.colors.textMuted
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(10)
        wrapMode: Text.Wrap
    }

    Text {
        objectName: "compactionCoverageErrorCode"
        Layout.fillWidth: true
        visible: block.isUnavailable && block.coverage.errorCode.length > 0
        text: block.hasMap ? qsTr("Error code: %1").arg(block.coverage.errorCode) : ""
        color: block.colors.textMuted
        textFormat: Text.PlainText
        font.pixelSize: block.hostWindow.dp(10)
        wrapMode: Text.WrapAnywhere
    }
}
