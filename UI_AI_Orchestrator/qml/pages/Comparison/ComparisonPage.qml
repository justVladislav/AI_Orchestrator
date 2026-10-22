pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "Comparison_Objects"

Item {
    id: page

    required property var hostWindow
    required property var colors

    property var projectsModel: null
    property var sessionsModel: null
    property var resultsModel: null
    property var summaryData: null
    property var modelChoices: []

    property string selectedProjectId: ""
    property string selectedSessionId: ""
    property string currentSessionStatus: ""
    property string lastUpdatedText: ""
    property bool busy: false
    property string errorText: ""

    property bool canCreateComparison: true
    property bool canRefresh: false
    property bool canCancel: false

    signal projectSelectionRequested(string projectId)
    signal sessionSelectionRequested(string sessionId)
    signal createComparisonRequested(var request)
    signal refreshRequested(string sessionId)
    signal cancelRequested(string sessionId)
    signal retryResultRequested(string resultId)
    signal preferredResultRequested(string resultId)
    signal resultSelectionChanged(string resultId, bool selected)
    signal synthesisRequested(string sessionId, var resultIds)
    signal humanRatingRequested(string resultId, int score, string comment)

    readonly property real logicalWidth: hostWindow.scaleFactor > 0
                                         ? width / hostWindow.scaleFactor
                                         : width
    readonly property string layoutMode: logicalWidth >= 1000
                                         ? "wide"
                                         : logicalWidth >= 680
                                           ? "medium"
                                           : "narrow"

    readonly property int projectCount: toolbar.projectCount
    readonly property int sessionCount: toolbar.sessionCount
    readonly property int resultCount: cardsView.count
    readonly property bool hasResults: resultCount > 0
    readonly property bool hasBlockingError: errorText.length > 0 && !hasResults

    readonly property string viewState: {
        if (hasBlockingError)
            return "error"
        if (busy && !hasResults)
            return "loading"
        if (projectCount === 0)
            return "noProjects"
        if (selectedProjectId.length === 0)
            return "projectNotSelected"
        if (sessionCount === 0)
            return "noSessions"
        if (selectedSessionId.length === 0)
            return "sessionNotSelected"
        if (hasResults)
            return "content"
        if (currentSessionStatus === "queued" || currentSessionStatus === "running")
            return "sessionRunning"
        if (currentSessionStatus === "failed")
            return "sessionFailed"
        if (currentSessionStatus === "cancelled")
            return "sessionCancelled"
        return "sessionEmpty"
    }

    readonly property string statusText: {
        switch (currentSessionStatus) {
        case "queued": return qsTr("Queued")
        case "running": return qsTr("In progress")
        case "completed": return qsTr("Completed")
        case "partial": return qsTr("Completed with partial results")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        default: return ""
        }
    }

    function emptyTitle(state) {
        switch (state) {
        case "error": return qsTr("Unable to load comparison")
        case "loading": return qsTr("Loading comparison…")
        case "noProjects": return qsTr("No projects available")
        case "projectNotSelected": return qsTr("Select a project")
        case "noSessions": return qsTr("No comparisons yet")
        case "sessionNotSelected": return qsTr("Select a comparison")
        case "sessionRunning": return qsTr("Comparison is running")
        case "sessionFailed": return qsTr("Comparison failed")
        case "sessionCancelled": return qsTr("Comparison was cancelled")
        default: return qsTr("No results available")
        }
    }

    function emptyDescription(state) {
        switch (state) {
        case "error": return errorText
        case "loading": return qsTr("The latest persisted results will appear here when they are available.")
        case "noProjects": return qsTr("Create or activate a project before starting a comparison.")
        case "projectNotSelected": return qsTr("Choose a project above to see its comparison sessions.")
        case "noSessions": return qsTr("Run the same request against multiple models to create a comparison.")
        case "sessionNotSelected": return qsTr("Choose a comparison session above to inspect its results.")
        case "sessionRunning": return qsTr("Results will appear as providers finish their work.")
        case "sessionFailed": return errorText.length > 0 ? errorText : qsTr("No provider result was completed.")
        case "sessionCancelled": return qsTr("This session stopped before a result was completed.")
        default: return qsTr("This session does not contain any persisted model results.")
        }
    }

    function openNewComparisonDialog() {
        // 1. Загружаем и сразу передаем первичные свойства
        if (!newComparisonDialogLoader.active) {
            newComparisonDialogLoader.setSource(
                "qrc:/qt/qml/UI_AI_Orchestrator/pages/Comparison/Comparison_Objects/NewComparisonDialog.qml",
                {
                    hostWindow: page.hostWindow,
                    colors: page.colors,
                    projectId: page.selectedProjectId,
                    modelChoices: page.modelChoices
                }
            )
            newComparisonDialogLoader.active = true
        }

        // 2. Дожидаемся готовности кадра и просто открываем диалог
        Qt.callLater(function() {
            const dialog = newComparisonDialogLoader.item
            if (!dialog)
                return
            dialog.hostWindow = page.hostWindow
            dialog.colors = page.colors
            dialog.projectId = page.selectedProjectId
            dialog.modelChoices = page.modelChoices
            dialog.open()
        })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: page.colors.contentMargin
        spacing: page.hostWindow.dp(14)

        ComparisonToolbar {
            id: toolbar
            Layout.fillWidth: true
            hostWindow: page.hostWindow
            colors: page.colors
            layoutMode: page.layoutMode
            projectsModel: page.projectsModel
            sessionsModel: page.sessionsModel
            selectedProjectId: page.selectedProjectId
            selectedSessionId: page.selectedSessionId
            statusText: page.statusText
            lastUpdatedText: page.lastUpdatedText
            busy: page.busy
            canCreateComparison: page.canCreateComparison
            canRefresh: page.canRefresh
            canCancel: page.canCancel

            onProjectSelected: projectId => page.projectSelectionRequested(projectId)
            onSessionSelected: sessionId => page.sessionSelectionRequested(sessionId)
            onCreateRequested: page.openNewComparisonDialog()
            onRefreshRequested: page.refreshRequested(page.selectedSessionId)
            onCancelRequested: page.cancelRequested(page.selectedSessionId)
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: inlineErrorText.implicitHeight + page.hostWindow.dp(16)
            visible: page.errorText.length > 0 && page.hasResults
            radius: page.colors.cornerRadiusSmall
            color: page.colors.errorSoft
            border.width: 1
            border.color: page.colors.error

            Text {
                id: inlineErrorText
                anchors.fill: parent
                anchors.margins: page.hostWindow.dp(8)
                text: page.errorText
                color: page.colors.error
                font.pixelSize: page.hostWindow.dp(11)
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ComparisonEmptyState {
                anchors.fill: parent
                visible: page.viewState !== "content"
                hostWindow: page.hostWindow
                colors: page.colors
                title: page.emptyTitle(page.viewState)
                description: page.emptyDescription(page.viewState)
            }

            GridLayout {
                id: comparisonLayout
                anchors.fill: parent
                visible: page.viewState === "content"
                columns: page.layoutMode === "wide" ? 2 : 1
                rows: page.layoutMode === "wide" ? 1 : 2
                columnSpacing: page.hostWindow.dp(14)
                rowSpacing: page.hostWindow.dp(14)

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: page.layoutMode === "wide" ? 0 : page.hostWindow.dp(140)
                    Layout.preferredHeight: page.layoutMode === "wide"
                                            ? -1 : comparisonLayout.height * 0.58

                    ListView {
                        id: cardsView
                        anchors.fill: parent
                        clip: true
                        orientation: ListView.Horizontal
                        spacing: page.hostWindow.dp(14)
                        model: page.resultsModel
                        boundsBehavior: Flickable.StopAtBounds
                        flickDeceleration: 1800

                        ScrollBar.horizontal: ScrollBar {
                            id: cardsScrollBar
                            policy: ScrollBar.AsNeeded
                            contentItem: Rectangle {
                                implicitHeight: page.hostWindow.dp(4)
                                radius: height / 2
                                color: cardsScrollBar.pressed ? page.colors.accent : page.colors.textMuted
                                opacity: cardsScrollBar.active ? 0.72 : 0.28
                            }
                            background: Item { }
                        }
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }

                        delegate: ComparisonResultCard {
                            required property var model

                            width: Math.max(0, cardsView.width - page.hostWindow.dp(4))
                            height: cardsView.height
                            hostWindow: page.hostWindow
                            colors: page.colors

                            resultId: model.resultId || ""
                            providerName: model.providerName || ""
                            modelName: model.modelName || ""
                            status: model.status || "queued"
                            content: model.content || ""
                            errorText: model.errorText || ""
                            strengths: model.strengths || []
                            warnings: model.warnings || []
                            inputTokens: model.inputTokens || 0
                            outputTokens: model.outputTokens || 0
                            totalCostText: model.totalCostText || "—"
                            latencyText: model.latencyText || "—"
                            qualityAvailable: model.qualityAvailable === true
                            qualityScore: model.qualityScore || 0
                            qualitySource: model.qualitySource || ""
                            isPreferred: model.isPreferred === true
                            selectedForSynthesis: model.selectedForSynthesis === true
                            canRetry: model.canRetry === true
                            canSelectForSynthesis: model.canSelectForSynthesis === true

                            onRetryRequested: resultId => page.retryResultRequested(resultId)
                            onPreferredRequested: resultId => page.preferredResultRequested(resultId)
                            onSynthesisSelectionRequested: (resultId, selected) =>
                                page.resultSelectionChanged(resultId, selected)
                            onHumanRatingRequested: (resultId, score, comment) =>
                                page.humanRatingRequested(resultId, score, comment)
                        }
                    }
                }

                ComparisonSummaryPanel {
                    Layout.fillHeight: true
                    Layout.fillWidth: page.layoutMode !== "wide"
                    Layout.minimumWidth: page.layoutMode === "wide" ? page.hostWindow.dp(260) : 0
                    Layout.preferredWidth: page.layoutMode === "wide"
                                           ? Math.min(page.hostWindow.dp(340), comparisonLayout.width * 0.36)
                                           : -1
                    Layout.minimumHeight: page.layoutMode === "wide" ? 0 : page.hostWindow.dp(140)
                    Layout.preferredHeight: page.layoutMode === "wide"
                                            ? -1 : comparisonLayout.height * 0.40
                    hostWindow: page.hostWindow
                    colors: page.colors
                    summaryData: page.summaryData

                    onSendToSynthesisRequested: {
                        const selectedIds = page.summaryData && page.summaryData.selectedResultIds
                                            ? page.summaryData.selectedResultIds : []
                        page.synthesisRequested(page.selectedSessionId, selectedIds)
                    }
                }
            }
        }

        Loader {
            id: newComparisonDialogLoader
            objectName: "newComparisonDialogLoader"
            parent: page
            active: false
        }

        Connections {
            target: newComparisonDialogLoader.item
            ignoreUnknownSignals: true

            function onSubmitted(request) {
                page.createComparisonRequested(request)
            }
        }
    }
}
