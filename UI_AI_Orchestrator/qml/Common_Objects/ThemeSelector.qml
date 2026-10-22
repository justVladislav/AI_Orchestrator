import QtQuick
import QtQuick.Layouts

Rectangle {
    id: selector

    required property var hostWindow
    required property var colors
    required property string themeMode

    signal themeRequested(string mode)

    implicitWidth: buttons.implicitWidth + hostWindow.dp(8)
    implicitHeight: colors.controlHeight + hostWindow.dp(8)
    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border

    RowLayout {
        id: buttons

        anchors.centerIn: parent
        spacing: selector.hostWindow.dp(4)

        ThemeModeButton {
            hostWindow: selector.hostWindow
            colors: selector.colors
            modeName: "light"
            iconSource: selector.colors.isLightMode
                        ? "qrc:/assets/light_mode_dark.png"
                        : "qrc:/assets/light_mode_light.png"
            accessibleLabel: qsTr("Light theme")
            checked: selector.themeMode === modeName
            onClicked: selector.themeRequested(modeName)
        }

        ThemeModeButton {
            hostWindow: selector.hostWindow
            colors: selector.colors
            modeName: "dark"
            iconSource: selector.colors.isLightMode
                        ? "qrc:/assets/dark_mode_dark.png"
                        : "qrc:/assets/dark_mode_light.png"
            accessibleLabel: qsTr("Dark theme")
            checked: selector.themeMode === modeName
            onClicked: selector.themeRequested(modeName)
        }

        ThemeModeButton {
            hostWindow: selector.hostWindow
            colors: selector.colors
            modeName: "transparent"
            iconSource: "qrc:/assets/transparent.png"
            accessibleLabel: qsTr("Transparent theme")
            checked: selector.themeMode === modeName
            onClicked: selector.themeRequested(modeName)
        }
    }
}
