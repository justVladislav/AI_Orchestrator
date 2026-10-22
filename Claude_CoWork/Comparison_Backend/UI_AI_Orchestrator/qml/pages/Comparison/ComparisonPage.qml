pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "Comparison_Objects"

Item {
    id: page

    required property var hostWindow
    required property var colors
    required property var backend

    // ---- Backend-facing UI contract -------------------------------------
    signal projectSelectionRequested(string projectId)
    signal comparisonSelectionRequested(string comparisonId)
    signal resultSelected(string resultId)
    signal resultCopyRequested(string resultId, string content)
    signal resultRetryRequested(string resultId)
    signal synthesisRequested(string comparisonId, var resultIds)
    signal refreshRequested(string comparisonId)

    // ---- Responsive breakpoint --------------------------------------------
    readonly property real logicalWidth: hostWindow.scaleFactor > 0
                                         ? width / hostWindow.scaleFactor
                                         : width
    readonly property string layoutMode: logicalWidth >= 1000
                                         ? "wide"
                                         : logicalWidth >= 680
                                           ? "medium"
                                           : "narrow"

    // ---- Controller bindings -----------------------------------------------
    readonly property string selectedProjectId: backend.comparison.selectedProjectId
    readonly property string selectedComparisonId: backend.comparison.selectedComparisonId
    readonly property string lastUpdatedText: backend.comparison.selectedComparisonUpdatedAt
    readonly property bool isLoading: backend.comparison.isLoading

    // JS snapshot of the current result list, refreshed on resultsUpdated signal.
    // Used by summaryData and statusText derived properties.
    property var resultsList: []

    // Bumped whenever resultsList changes so summaryData / statusText re-evaluate.
    property int dataRevision: 0

    // ---- Controller connection --------------------------------------------
    Connections {
        target: backend.comparison

        function onResultsUpdated(comparisonId) {
            page.resultsList = backend.comparison.resultsSnapshot()
            page.dataRevision++
        }
    }

    // ---- Selection actions -----------------------------------------------

    function selectProject(projectId) {
        page.resultsList = []
        page.dataRevision++
        backend.comparison.selectProject(projectId)
        page.projectSelectionRequested(projectId)
    }

    function selectComparison(comparisonId) {
        backend.comparison.selectComparison(comparisonId)
        if (comparisonId.length > 0)
            page.comparisonSelectionRequested(comparisonId)
    }

    function refresh() {
        backend.comparison.refresh()
        if (page.selectedComparisonId.length > 0)
            page.refreshRequested(page.selectedComparisonId)
    }

    // ---- Result actions --------------------------------------------------

    function copyResult(resultId, content) {
        page.resultCopyRequested(resultId, content)
    }

    function markAsBest(resultId) {
        backend.comparison.markAsBest(page.selectedComparisonId, resultId)
        page.resultSelected(resultId)
    }

    function toggleSynthesisSelection(resultId) {
        backend.comparison.toggleSynthesisSelection(resultId)
    }

    function retryResult(resultId) {
        backend.comparison.retryResult(resultId)
        page.resultRetryRequested(resultId)
    }

    function sendToSynthesis() {
        if (!page.summaryData || !page.summaryData.synthesisEnabled)
            return
        const ids = page.resultsList
            .filter(r => r.selectedForSynthesis && r.status === "completed")
            .map(r => r.resultId)
        page.synthesisRequested(page.selectedComparisonId, ids)
    }

    // ---- Derived summary -----------------------------------------------

    readonly property var summaryData: {
        dataRevision
        if (selectedComparisonId.length === 0) return null

        const results = page.resultsList
        const completed = results.filter(r => r.status === "completed")

        let recommended = results.find(r => r.isBest) || null
        const isManual = recommended !== null

        if (!recommended && completed.length > 0) {
            recommended = completed.reduce(
                (best, r) => (r.qualityScore > best.qualityScore ? r : best),
                completed[0])
        }

        const warnings = []
        for (let i = 0; i < results.length; ++i) {
            const entry = results[i]
            const ws = entry.warnings || []
            for (let j = 0; j < ws.length; ++j)
                warnings.push(entry.providerName + ": " + ws[j])
        }

        let totalCostValue = 0
        let totalTokens = 0
        for (let i = 0; i < results.length; ++i) {
            totalCostValue += parseFloat(String(results[i].totalCost).replace("$", "")) || 0
            totalTokens += (results[i].inputTokens || 0) + (results[i].outputTokens || 0)
        }

        const selected = results.filter(r => r.selectedForSynthesis && r.status === "completed")

        return {
            recommendedLabel: recommended ? (recommended.providerName + " · " + recommended.modelName) : "",
            recommendedIsManual: isManual,
            consensusPoints: [],
            differencePoints: [],
            warnings: warnings,
            totalCost: "$" + Number(totalCostValue).toFixed(3),
            totalTokens: totalTokens,
            selectedLabels: selected.map(r => r.providerName),
            synthesisEnabled: selected.length >= 2
        }
    }

    readonly property string statusText: {
        dataRevision
        if (selectedComparisonId.length === 0) return ""
        const results = page.resultsList
        if (results.some(r => r.status === "running"))  return qsTr("In progress")
        if (results.some(r => r.status === "error"))    return qsTr("Completed with errors")
        if (results.some(r => r.status === "partial"))  return qsTr("Completed with a partial result")
        if (results.length > 0)                         return qsTr("All results completed")
        return ""
    }

    // ---- Layout -----------------------------------------------------------

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: page.colors.contentMargin
        spacing: page.hostWindow.dp(14)

        ComparisonToolbar {
            Layout.fillWidth: true
            hostWindow: page.hostWindow
            colors: page.colors
            projectsModel: backend.comparison.projectsModel
            comparisonGroupsModel: backend.comparison.comparisonListModel
            selectedProjectId: page.selectedProjectId
            selectedComparisonId: page.selectedComparisonId
            statusText: page.statusText
            lastUpdatedText: page.lastUpdatedText
            refreshEnabled: !page.isLoading

            onProjectSelected: projectId => page.selectProject(projectId)
            onComparisonSelected: comparisonId => page.selectComparison(comparisonId)
            onRefreshRequested: page.refresh()
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ComparisonEmptyState {
                anchors.fill: parent
                visible: page.selectedProjectId.length === 0
                hostWindow: page.hostWindow
                colors: page.colors
                title: qsTr("Select a project")
                description: qsTr("Choose a project above to see its comparisons.")
            }

            ComparisonEmptyState {
                anchors.fill: parent
                visible: page.selectedProjectId.length > 0
                         && page.selectedComparisonId.length === 0
                         && !page.isLoading
                hostWindow: page.hostWindow
                colors: page.colors
                title: qsTr("No comparisons found")
                description: qsTr("This project has no comparisons yet. Comparisons appear here once a task has been run against more than one provider.")
            }

            ComparisonEmptyState {
                anchors.fill: parent
                visible: page.selectedComparisonId.length > 0 && page.isLoading
                hostWindow: page.hostWindow
                colors: page.colors
                title: qsTr("Loading comparison…")
            }

            GridLayout {
                id: comparisonLayout
                anchors.fill: parent
                visible: page.selectedComparisonId.length > 0
                         && !page.isLoading
                         && page.resultsList.length > 0
                columns: page.layoutMode === "wide" ? 2 : 1
                rows: page.layoutMode === "wide" ? 1 : 2
                columnSpacing: page.hostWindow.dp(14)
                rowSpacing: page.hostWindow.dp(14)

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: page.layoutMode === "wide" ? -1 : page.hostWindow.dp(220)
                    Layout.preferredHeight: page.layoutMode === "wide"
                                            ? -1 : comparisonLayout.height * 0.58

                    ListView {
                        id: cardsView
                        anchors.fill: parent
                        clip: true
                        orientation: page.layoutMode === "narrow" ? ListView.Vertical : ListView.Horizontal
                        spacing: page.hostWindow.dp(14)
                        model: backend.comparison.resultListModel
                        boundsBehavior: Flickable.StopAtBounds

                        ScrollBar.horizontal: ScrollBar {
                            policy: cardsView.orientation === ListView.Horizontal
                                    ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                        }
                        ScrollBar.vertical: ScrollBar {
                            policy: cardsView.orientation === ListView.Vertical
                                    ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                        }

                        delegate: ComparisonResultCard {
                            width: cardsView.orientation === ListView.Horizontal
                                   ? Math.min(page.hostWindow.dp(320),
                                              Math.max(page.hostWindow.dp(240),
                                                       cardsView.width - page.hostWindow.dp(4)))
                                   : cardsView.width
                            height: cardsView.orientation === ListView.Horizontal
                                    ? cardsView.height : page.hostWindow.dp(420)
                            hostWindow: page.hostWindow
                            colors: page.colors

                            onCopyRequested: (resultId, content) => page.copyResult(resultId, content)
                            onRetryRequested: resultId => page.retryResult(resultId)
                            onBestToggleRequested: resultId => page.markAsBest(resultId)
                            onSynthesisToggleRequested: resultId => page.toggleSynthesisSelection(resultId)
                        }
                    }
                }

                ComparisonSummaryPanel {
                    Layout.fillHeight: true
                    Layout.fillWidth: page.layoutMode !== "wide"
                    Layout.minimumWidth: page.layoutMode === "wide" ? page.hostWindow.dp(250) : -1
                    Layout.preferredWidth: page.layoutMode === "wide"
                                           ? Math.min(page.hostWindow.dp(320), comparisonLayout.width * 0.34)
                                           : -1
                    Layout.minimumHeight: page.layoutMode === "wide" ? -1 : page.hostWindow.dp(220)
                    Layout.preferredHeight: page.layoutMode === "wide"
                                            ? -1 : comparisonLayout.height * 0.40
                    hostWindow: page.hostWindow
                    colors: page.colors
                    summaryData: page.summaryData

                    onSendToSynthesisRequested: page.sendToSynthesis()
                }
            }
        }
    }
}
