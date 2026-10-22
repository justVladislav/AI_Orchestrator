import QtQuick
import QtQuick.Layouts
import "../Common_Objects"

Rectangle {
    id: navigationMenu

    required property var hostWindow
    required property var colors
    required property string themeMode

    property string currentSection: "overview"

    signal sectionRequested(string sectionId)
    signal themeModeRequested(string mode)

    function selectSection(sectionId) {
        currentSection = sectionId
        sectionRequested(sectionId)
    }

    width: hostWindow.dp(248)
    color: colors.surface
    border.width: 1
    border.color: colors.border

    ColumnLayout {
        id: navigationContent

        anchors.fill: parent
        anchors.margins: navigationMenu.hostWindow.dp(16)
        spacing: navigationMenu.hostWindow.dp(5)

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: navigationMenu.hostWindow.dp(58)

            Image {
                id: appLogo
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: navigationMenu.hostWindow.dp(34)
                height: width
                // The PNG is registered in the navigation_icons resource,
                // so only the qrc path works — a relative file path does not
                // exist inside the compiled resource system.
                source: "qrc:/assets/navigation/main_icon.png"
                sourceSize.width: navigationMenu.hostWindow.dp(68)
                sourceSize.height: navigationMenu.hostWindow.dp(68)
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            // Fallback badge if the logo resource fails to load.
            Rectangle {
                visible: appLogo.status !== Image.Ready
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: navigationMenu.hostWindow.dp(34)
                height: width
                radius: navigationMenu.colors.cornerRadiusSmall
                color: navigationMenu.colors.accent

                Text {
                    anchors.centerIn: parent
                    text: "AI"
                    color: navigationMenu.colors.textOnAccent
                    font.pixelSize: navigationMenu.hostWindow.dp(13)
                    font.weight: Font.Bold
                }
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: navigationMenu.hostWindow.dp(46)
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("AI Orchestrator")
                color: navigationMenu.colors.textPrimary
                font.pixelSize: navigationMenu.hostWindow.dp(16)
                font.weight: Font.DemiBold
            }
        }

        NavigationItem {
            Layout.fillWidth: true
            hostWindow: navigationMenu.hostWindow
            colors: navigationMenu.colors
            sectionId: "overview"
            labelText: qsTr("Overview")
            iconText: "⌂"
            selected: navigationMenu.currentSection === sectionId
            onClicked: navigationMenu.selectSection(sectionId)
        }

        NavigationItem {
            Layout.fillWidth: true
            hostWindow: navigationMenu.hostWindow
            colors: navigationMenu.colors
            sectionId: "projects"
            labelText: qsTr("Projects")
            iconText: "▣"
            selected: navigationMenu.currentSection === sectionId
            onClicked: navigationMenu.selectSection(sectionId)
        }

        NavigationItem {
            Layout.fillWidth: true
            hostWindow: navigationMenu.hostWindow
            colors: navigationMenu.colors
            sectionId: "chats"
            labelText: qsTr("Chats")
            iconText: "✦"
            selected: navigationMenu.currentSection === sectionId
            onClicked: navigationMenu.selectSection(sectionId)
        }

        NavigationItem {
            Layout.fillWidth: true
            hostWindow: navigationMenu.hostWindow
            colors: navigationMenu.colors
            sectionId: "runs"
            labelText: qsTr("Runs")
            iconText: "▶"
            selected: navigationMenu.currentSection === sectionId
            onClicked: navigationMenu.selectSection(sectionId)
        }

        NavigationItem {
            Layout.fillWidth: true
            hostWindow: navigationMenu.hostWindow
            colors: navigationMenu.colors
            sectionId: "comparison"
            labelText: qsTr("Comparison")
            iconText: "⇄"
            selected: navigationMenu.currentSection === sectionId
            onClicked: navigationMenu.selectSection(sectionId)
        }

        NavigationItem {
            Layout.fillWidth: true
            hostWindow: navigationMenu.hostWindow
            colors: navigationMenu.colors
            sectionId: "providers"
            labelText: qsTr("Providers and models")
            iconText: "◇"
            selected: navigationMenu.currentSection === sectionId
            onClicked: navigationMenu.selectSection(sectionId)
        }

        Item {
            Layout.fillHeight: true
        }

        NavigationItem {
            Layout.fillWidth: true
            hostWindow: navigationMenu.hostWindow
            colors: navigationMenu.colors
            sectionId: "settings"
            labelText: qsTr("Settings")
            iconText: "⚙"
            selected: navigationMenu.currentSection === sectionId
            onClicked: navigationMenu.selectSection(sectionId)
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: navigationMenu.colors.separator
        }

        // ThemeSelector {
        //     Layout.alignment: Qt.AlignLeft
        //     hostWindow: navigationMenu.hostWindow
        //     colors: navigationMenu.colors
        //     themeMode: navigationMenu.themeMode
        //     onThemeRequested: mode => navigationMenu.themeModeRequested(mode)
        // }
    }
}
