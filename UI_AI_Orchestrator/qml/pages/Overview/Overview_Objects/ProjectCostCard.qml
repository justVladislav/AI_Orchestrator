import QtQuick
import QtQuick.Layouts

// A1-min: the Overview's project cost estimate - the execution ledger and the stored memory compactions together.
// QML only WORDS what the Qt layer (ProjectCostController) reports: the load state, the price state, the pre-formatted amount and
// the counters. It never reads or formats a raw amount, and a state is never turned into another one:
//   idle         no project selected            "—"
//   unavailable  the read failed                "Unavailable" + the original message and code (never an old sum, never "$0")
//   nothing      nothing recorded yet           "—" + "No recorded model executions or memory compactions yet"
//   none         records, but no price data     "No cost data recorded" (not "empty": a project made only of such records is not empty)
//   unknown      priced items, none priced      "Cost unknown"
//   partial      some price is incomplete       "Incomplete estimate: $X; total unknown"
//   known        every price is complete        "$X" (a known zero is "$0.0000")
// The explanation of what the estimate does NOT cover is one tap away (no hover needed).
Rectangle {
    id: card
    objectName: "projectCostCard"

    required property var hostWindow
    required property var colors
    // backend.projectCost (ProjectCostController).
    required property var cost
    property bool detailsOpen: false

    // The wording of a price state. priceState and costText come from the Qt layer (the amount is formatted there): this only
    // chooses the localized phrase. The provider rows of the Overview word the same states with the same phrases.
    function moneyWording(priceState, costText) {
        if (priceState === "known")
            return costText
        if (priceState === "partial")
            return qsTr("Incomplete estimate: %1; total unknown").arg(costText)
        if (priceState === "unknown")
            return qsTr("Cost unknown")
        return "—"
    }

    // B3-III: the recording coverage of the SAME read (a map, or undefined for a stub). It is shown apart from the money below.
    readonly property var coverageMap: card.cost.coverage !== undefined ? card.cost.coverage : null
    readonly property bool coverageHasOperations: card.coverageMap !== null && card.coverageMap.state === "available"
                                                   && card.coverageMap.hasCounts === true
                                                   && (Number(card.coverageMap.openOperations) > 0
                                                       || Number(card.coverageMap.unresolvedOperations) > 0)

    readonly property string loadState: cost.loadState
    readonly property bool available: loadState === "available"
    readonly property bool unavailable: loadState === "unavailable"
    readonly property real recordedItems: Number(cost.ledgerExecutionCount) + Number(cost.compactionRecordCount)
    // Nothing was recorded at all. A project that has only records without price data is NOT this.
    readonly property bool nothingRecorded: available && recordedItems === 0
    readonly property bool onlyNoPriceRecords: available && recordedItems > 0 && cost.priceState === "none"
    // The value is a sentence rather than one figure: drawn smaller, italic, in the warning colour, wrapped (never cut).
    readonly property bool sentence: unavailable
                                     || (available && (cost.priceIncomplete || onlyNoPriceRecords))

    readonly property string valueText: {
        if (card.unavailable)
            return qsTr("Unavailable")
        if (!card.available)
            return "—"
        if (card.nothingRecorded)
            return "—"
        if (card.onlyNoPriceRecords)
            return qsTr("No cost data recorded")
        return card.moneyWording(card.cost.priceState, card.cost.costText)
    }

    // The one sentence that explains the state of the value.
    readonly property string statusText: {
        if (card.unavailable)
            return qsTr("The project cost could not be read: %1").arg(card.cost.errorMessage)
        if (!card.available)
            return qsTr("No project selected")
        if (card.nothingRecorded)
            return card.coverageHasOperations ? qsTr("No stored cost entries")
                                              : qsTr("No recorded model executions or memory compactions yet")
        if (card.onlyNoPriceRecords)
            return qsTr("%1 memory compaction records have no price data").arg(card.cost.compactionNoneCount)
        if (card.cost.priceState === "partial")
            return qsTr("%1 of %2 pricing entries have an incomplete price")
                   .arg(card.cost.incompletePriceCount).arg(card.cost.pricingEvidenceCount)
        if (card.cost.priceState === "unknown")
            return qsTr("%1 items have an unknown price").arg(card.cost.incompletePriceCount)
        return ""
    }

    implicitHeight: Math.max(hostWindow.dp(108), textColumn.implicitHeight + 2 * hostWindow.dp(14))
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: card.hostWindow.dp(14)
        spacing: card.hostWindow.dp(12)

        Rectangle {
            Layout.preferredWidth: card.hostWindow.dp(38)
            Layout.preferredHeight: card.hostWindow.dp(38)
            Layout.alignment: Qt.AlignTop
            radius: card.colors.cornerRadiusSmall
            color: Qt.rgba(card.colors.warning.r, card.colors.warning.g, card.colors.warning.b, 0.16)

            Text {
                anchors.centerIn: parent
                text: "$"
                color: card.colors.warning
                font.pixelSize: card.hostWindow.dp(15)
                font.weight: Font.Bold
            }
        }

        ColumnLayout {
            id: textColumn
            Layout.fillWidth: true
            // A small preferred width: the column takes what the card gives it and a long wrapped line never widens the card.
            Layout.preferredWidth: card.hostWindow.dp(60)
            Layout.alignment: Qt.AlignTop
            spacing: card.hostWindow.dp(2)

            Text {
                objectName: "projectCostTitle"
                Layout.fillWidth: true
                text: qsTr("Project cost estimate")
                color: card.colors.textSecondary
                font.pixelSize: card.hostWindow.dp(11)
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "projectCostValue"
                Layout.fillWidth: true
                text: card.valueText
                color: card.sentence ? card.colors.warning : card.colors.textPrimary
                font.pixelSize: card.sentence ? card.hostWindow.dp(14) : card.hostWindow.dp(21)
                font.weight: Font.DemiBold
                font.italic: card.sentence
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "projectCostStatus"
                Layout.fillWidth: true
                visible: text.length > 0
                text: card.statusText
                color: card.colors.textMuted
                font.pixelSize: card.hostWindow.dp(10)
                wrapMode: Text.Wrap
            }

            // The two sources are shown apart: ledger executions are not compactions, and neither is "the number of calls".
            Text {
                objectName: "projectCostBreakdown"
                Layout.fillWidth: true
                visible: card.available && card.recordedItems > 0
                text: qsTr("%1 model executions · %2 memory compactions")
                      .arg(card.cost.ledgerExecutionCount).arg(card.cost.compactionRecordCount)
                color: card.colors.textMuted
                font.pixelSize: card.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            // Records without any price data are counted apart: not incomplete, and not a zero cost.
            Text {
                objectName: "projectCostNoPriceRecords"
                Layout.fillWidth: true
                visible: card.available && Number(card.cost.compactionNoneCount) > 0 && !card.onlyNoPriceRecords
                text: qsTr("%1 memory compaction records have no price data").arg(card.cost.compactionNoneCount)
                color: card.colors.textMuted
                font.pixelSize: card.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "projectCostErrorCode"
                Layout.fillWidth: true
                visible: card.unavailable && card.cost.errorCode.length > 0
                text: qsTr("Error code: %1").arg(card.cost.errorCode)
                color: card.colors.textMuted
                font.pixelSize: card.hostWindow.dp(10)
                wrapMode: Text.WrapAnywhere
            }

            // B3-III: the recording coverage of the compaction intents, apart from the amount and the price state: it never changes them.
            CompactionCoverageBlock {
                objectName: "compactionCoverageBlock"
                Layout.fillWidth: true
                hostWindow: card.hostWindow
                colors: card.colors
                coverage: card.coverageMap
            }

            // What the estimate is based on (not a claim that every operation was recorded).
            Text {
                objectName: "projectCostCoverage"
                Layout.fillWidth: true
                visible: card.available
                text: qsTr("Based on recorded executions and memory compactions")
                color: card.colors.textMuted
                font.pixelSize: card.hostWindow.dp(10)
                font.italic: true
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "projectCostDetailsToggle"
                Layout.fillWidth: true
                visible: card.available
                text: card.detailsOpen ? qsTr("Hide details") : qsTr("Details")
                color: card.colors.accent
                font.pixelSize: card.hostWindow.dp(10)
                font.underline: true
                TapHandler { onTapped: card.detailsOpen = !card.detailsOpen }
            }

            // The explanation of what the estimate does not cover, one tap away (no hover).
            Text {
                objectName: "projectCostDetailsCoverage"
                Layout.fillWidth: true
                visible: card.available && card.detailsOpen
                text: qsTr("Operations that were interrupted or never recorded may be missing.")
                color: card.colors.textSecondary
                font.pixelSize: card.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "projectCostDetailsTokens"
                Layout.fillWidth: true
                visible: card.available && card.detailsOpen
                text: qsTr("Recorded tokens and costs include only stored entries; an operation without a confirmed final recording may have used tokens that are not counted.")
                color: card.colors.textSecondary
                textFormat: Text.PlainText
                font.pixelSize: card.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }

            Text {
                objectName: "projectCostDetailsPricing"
                Layout.fillWidth: true
                visible: card.available && card.detailsOpen
                text: qsTr("Some older executions without a stored price are priced from the current model catalog; memory compaction costs are shown as recorded.")
                color: card.colors.textSecondary
                font.pixelSize: card.hostWindow.dp(10)
                wrapMode: Text.WordWrap
            }
        }
    }
}
