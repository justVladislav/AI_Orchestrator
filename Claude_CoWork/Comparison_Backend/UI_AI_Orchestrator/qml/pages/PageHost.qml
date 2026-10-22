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
            objectName: "projectsPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
            onProjectOpenRequested: projectId => {
                pageHost.sectionRequested("runs")
            }
        }

        ChatsPage {
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }

        RunsPage {
            objectName: "runsPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }

        ComparisonPage {
            objectName: "comparisonPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }

        ProvidersPage {
            objectName: "providersPage"
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
            backend: pageHost.backend
        }

        SettingsPage {
            hostWindow: pageHost.hostWindow
            colors: pageHost.colors
        }
    }
}
