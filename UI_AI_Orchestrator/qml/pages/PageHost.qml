import QtQuick
import QtQuick.Layouts
import "Overview"
import "Projects"
import "Chats"
import "Runs"
import "Comparison"
import "Providers"
import "Settings"

Rectangle {
    id: pageHost

    required property var hostWindow
    required property var colors
    required property var backend
    required property string currentSection

    // Child pages request navigation without reaching into Main.qml or the
    // navigation menu. Main remains the sole owner of currentSection.
    signal sectionRequested(string sectionId)

    readonly property var sectionIndexes: ({
        "overview": 0,
        "projects": 1,
        "chats": 2,
        "runs": 3,
        "comparison": 4,
        "providers": 5,
        "settings": 6
    })
    readonly property int selectedIndex: sectionIndexes[currentSection] === undefined
                                         ? 0
                                         : sectionIndexes[currentSection]

    radius: colors.cornerRadiusLarge
    color: colors.surfaceRaised
    border.width: 1
    border.color: colors.border
    clip: true


    StackLayout {
        anchors.fill: parent
        currentIndex: pageHost.selectedIndex

        OverviewPage {
            objectName: "overviewPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }

        ProjectsPage {
            // Stable objectName so headless tests can find this page instance
            // through QObject::findChild without depending on QML type registration.
            objectName: "projectsPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
            onProjectOpenRequested: projectId => {
                // WorkspaceContextController has already persisted projectId
                // before ProjectsPage emits this success-only signal.
                pageHost.sectionRequested("runs")
            }
            onOrchestrationRequested: projectId => {
                // ProjectsPage has already persisted projectId as the active
                // workspace context. Open the canonical New Run dialog on the
                // Runs page instead of starting an invisible Mock workflow.
                pageHost.sectionRequested("runs")
                Qt.callLater(runsPage.openNewRunDialog)
            }
        }

        ChatsPage {
            objectName: "chatsPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
            onRunDraftsRequested: {
                // The drafts a plan produced live in the Drafts list of the Runs page.
                pageHost.sectionRequested("runs")
                Qt.callLater(runsPage.openDrafts)
            }
        }

        RunsPage {
            id: runsPage
            objectName: "runsPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }

        ComparisonPage {
            objectName: "comparisonPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            projectsModel: pageHost.backend.projects.model
            sessionsModel: pageHost.backend.comparisons.sessionsModel
            resultsModel: pageHost.backend.comparisons.resultsModel
            summaryData: pageHost.backend.comparisons.summaryData
            modelChoices: pageHost.backend.comparisons.modelChoices
            selectedProjectId: pageHost.backend.comparisons.selectedProjectId
            selectedSessionId: pageHost.backend.comparisons.selectedSessionId
            currentSessionStatus: pageHost.backend.comparisons.currentSessionStatus
            lastUpdatedText: pageHost.backend.comparisons.lastUpdatedText
            busy: pageHost.backend.comparisons.busy
            errorText: pageHost.backend.comparisons.errorText
            canCreateComparison: pageHost.backend.comparisons.canCreateComparison
            canRefresh: pageHost.backend.comparisons.canRefresh
            canCancel: pageHost.backend.comparisons.canCancel

            onProjectSelectionRequested: projectId =>
                pageHost.backend.comparisons.selectProject(projectId)
            onSessionSelectionRequested: sessionId =>
                pageHost.backend.comparisons.selectSession(sessionId)
            onCreateComparisonRequested: request =>
                pageHost.backend.comparisons.createComparison(request)
            onRefreshRequested: sessionId =>
                pageHost.backend.comparisons.refresh()
            onCancelRequested: sessionId =>
                pageHost.backend.comparisons.cancelComparison()
            onPreferredResultRequested: resultId =>
                pageHost.backend.comparisons.setPreferredResult(resultId)
            onResultSelectionChanged: (resultId, selected) =>
                pageHost.backend.comparisons.setResultSelected(resultId, selected)
        }

        ProvidersPage {
            objectName: "providersPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }

        SettingsPage {
            objectName: "settingsPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }
    }
}
