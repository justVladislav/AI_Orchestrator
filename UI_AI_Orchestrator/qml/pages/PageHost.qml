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

    // Opens the Runs page (creating it on the first request) and then performs
    // `action` on it exactly once - but only if Runs is still the selected page
    // when the request is served, so a late action never lands on another page.
    function openRunsAnd(action) {
        pageHost.sectionRequested("runs")
        Qt.callLater(function() {
            if (pageHost.currentSection === "runs" && runsSlot.item)
                action(runsSlot.item)
        })
    }

    radius: colors.cornerRadiusLarge
    color: colors.surfaceRaised
    border.width: 1
    border.color: colors.border
    clip: true


    // Every page is created on its first selection and kept afterwards (see CachedPage).
    StackLayout {
        anchors.fill: parent
        currentIndex: pageHost.selectedIndex

        CachedPage {
            id: overviewSlot
            selected: pageHost.selectedIndex === 0
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            sourceComponent: Component {
                OverviewPage {
                    objectName: "overviewPage"
                    width: overviewSlot.pageWindow.pageWidth
                    height: overviewSlot.pageWindow.pageHeight
                    hostWindow: overviewSlot.pageWindow
                    colors: overviewSlot.pageColors
                    backend: pageHost.backend
                }
            }
        }

        CachedPage {
            id: projectsSlot
            selected: pageHost.selectedIndex === 1
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            sourceComponent: Component {
                ProjectsPage {
                    // Stable objectName so headless tests can find this page instance
                    // through QObject::findChild without depending on QML type registration.
                    objectName: "projectsPage"
                    width: projectsSlot.pageWindow.pageWidth
                    height: projectsSlot.pageWindow.pageHeight
                    hostWindow: projectsSlot.pageWindow
                    colors: projectsSlot.pageColors
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
                        pageHost.openRunsAnd(runs => runs.openNewRunDialog())
                    }
                }
            }
        }

        CachedPage {
            id: chatsSlot
            selected: pageHost.selectedIndex === 2
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            sourceComponent: Component {
                ChatsPage {
                    objectName: "chatsPage"
                    width: chatsSlot.pageWindow.pageWidth
                    height: chatsSlot.pageWindow.pageHeight
                    hostWindow: chatsSlot.pageWindow
                    colors: chatsSlot.pageColors
                    backend: pageHost.backend
                    onRunDraftsRequested: {
                        // The drafts a plan produced live in the Drafts list of the Runs page.
                        pageHost.openRunsAnd(runs => runs.openDrafts())
                    }
                }
            }
        }

        CachedPage {
            id: runsSlot
            selected: pageHost.selectedIndex === 3
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            sourceComponent: Component {
                RunsPage {
                    objectName: "runsPage"
                    width: runsSlot.pageWindow.pageWidth
                    height: runsSlot.pageWindow.pageHeight
                    hostWindow: runsSlot.pageWindow
                    colors: runsSlot.pageColors
                    backend: pageHost.backend
                }
            }
        }

        CachedPage {
            id: comparisonSlot
            selected: pageHost.selectedIndex === 4
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            sourceComponent: Component {
                ComparisonPage {
                    objectName: "comparisonPage"
                    width: comparisonSlot.pageWindow.pageWidth
                    height: comparisonSlot.pageWindow.pageHeight
                    hostWindow: comparisonSlot.pageWindow
                    colors: comparisonSlot.pageColors
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
            }
        }

        CachedPage {
            id: providersSlot
            selected: pageHost.selectedIndex === 5
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            sourceComponent: Component {
                ProvidersPage {
                    objectName: "providersPage"
                    width: providersSlot.pageWindow.pageWidth
                    height: providersSlot.pageWindow.pageHeight
                    hostWindow: providersSlot.pageWindow
                    colors: providersSlot.pageColors
                    backend: pageHost.backend
                }
            }
        }

        CachedPage {
            id: settingsSlot
            selected: pageHost.selectedIndex === 6
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            sourceComponent: Component {
                SettingsPage {
                    objectName: "settingsPage"
                    width: settingsSlot.pageWindow.pageWidth
                    height: settingsSlot.pageWindow.pageHeight
                    hostWindow: settingsSlot.pageWindow
                    colors: settingsSlot.pageColors
                    backend: pageHost.backend
                }
            }
        }
    }
}
