import QtQuick
import QtQuick.Window
import QtQuick.Controls
import "Common_Objects"
import "Navigation"
import "pages"

ApplicationWindow {
    id: root

    required property var backend

    readonly property real designWidth: 1280
    readonly property real designHeight: 800
    readonly property real minimumScaleFactor: 0.60
    readonly property real maximumScaleFactor: 2.25
    readonly property real interfaceScaleMultiplier:
        backend.settings.interfaceScalePercent > 0
        ? backend.settings.interfaceScalePercent / 100.0 : 1.0
    readonly property real rawScaleFactor: Math.min(width / designWidth,
                                                    height / designHeight)
                                              * interfaceScaleMultiplier
    readonly property real scaleFactor: Math.max(minimumScaleFactor,
                                                 Math.min(maximumScaleFactor,
                                                          rawScaleFactor))

    readonly property string themeMode: backend.settings.themeMode
    property string currentSection: "overview"
    readonly property var theme: appTheme

    function dp(value) {
        return Math.round(value * scaleFactor * 100) / 100
    }

    function setTheme(mode) {
        const normalizedMode = String(mode).toLowerCase()

        if (normalizedMode !== "light"
                && normalizedMode !== "dark"
                && normalizedMode !== "transparent") {
            console.warn("Unsupported theme mode:", mode)
            return
        }

        if (!backend.settings.setThemeMode(normalizedMode))
            console.warn("Could not persist theme mode:", backend.settings.lastError)
    }

    flags: Qt.Window | Qt.FramelessWindowHint

    width: designWidth
    height: designHeight
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: qsTr("AI Orchestrator")
    color: appTheme.windowBackground


    palette {
        window: appTheme.windowBackground
        windowText: appTheme.textPrimary
        base: appTheme.inputBackground
        alternateBase: appTheme.surfaceAlternative
        text: appTheme.textPrimary
        brightText: appTheme.textOnAccent
        button: appTheme.buttonBackground
        buttonText: appTheme.textPrimary
        highlight: appTheme.accent
        highlightedText: appTheme.textOnAccent
        placeholderText: appTheme.textMuted
        light: appTheme.surfaceRaised
        midlight: appTheme.surfaceAlternative
        mid: appTheme.border
        dark: appTheme.borderStrong
        shadow: appTheme.shadow
        toolTipBase: appTheme.tooltipBackground
        toolTipText: appTheme.tooltipText
        link: appTheme.link
        linkVisited: appTheme.linkVisited
    }

    AppTheme {
        id: appTheme
        mode: root.themeMode
        scaleFactor: root.scaleFactor
        compactMode: root.backend.settings.compactMode
        reduceAnimations: root.backend.settings.reduceAnimations
    }

    WindowTitleBar {
        id: windowTitleBar

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        z: 100
        hostWindow: root
        colors: appTheme
        titleText: root.title
    }

    NavigationMenu {
        id: navigationMenu

        anchors.top: windowTitleBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        hostWindow: root
        colors: appTheme
        themeMode: root.themeMode
        currentSection: root.currentSection

        onSectionRequested: sectionId => root.currentSection = sectionId
        onThemeModeRequested: mode => root.setTheme(mode)
    }

    PageHost {
        id: pageHost
        anchors.top: windowTitleBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: navigationMenu.right
        anchors.right: parent.right
        anchors.margins: appTheme.contentMargin
        hostWindow: root
        colors: appTheme
        backend: root.backend
        currentSection: root.currentSection

        // Page-originated navigation uses the same single section state as
        // the side menu, so Continue never creates a parallel router.
        onSectionRequested: sectionId => root.currentSection = sectionId
    }

    Rectangle {
        anchors.fill: parent
        z: 900
        color: "transparent"
        border.width: 1
        border.color: appTheme.borderStrong
    }

    WindowResizeHandler {
        anchors.fill: parent
        z: 1000
        hostWindow: root
        handleThickness: appTheme.windowResizeMargin
    }
}




// Debug Functions


// function printMainSurfaces() {
//     console.log("[WINDOW]",
//                 "color:", root.color,
//                 "width:", root.width,
//                 "height:", root.height)

//     console.log("[BACKGROUND]",
//                 "color:", root.background ? root.background.color : "none",
//                 "width:", root.background ? root.background.width : -1,
//                 "height:", root.background ? root.background.height : -1,
//                 "opacity:", root.background ? root.background.opacity : -1)

//     console.log("[THEME]",
//                 "mode:", appTheme.themeMode,
//                 "windowBackground:", appTheme.windowBackground,
//                 "surface:", appTheme.surface,
//                 "surfaceRaised:", appTheme.surfaceRaised)
// }

// printMainSurfaces()
// Qt.callLater(root.printPaintedItems)

// function debugObjectName(object) {
//     if (!object)
//         return "<null>"

//     if (object.objectName)
//         return object.objectName

//     const rawName = String(object)
//     const bracket = rawName.indexOf("(")

//     return bracket >= 0
//             ? rawName.substring(0, bracket)
//             : rawName
// }

// function readDebugProperty(object, propertyName) {
//     try {
//         const value = object[propertyName]

//         return {
//             exists: value !== undefined,
//             value: value
//         }
//     } catch (error) {
//         return {
//             exists: false,
//             value: undefined
//         }
//     }
// }

// function printPaintedItems() {
//     const visited = new Set()

//     function visit(item, depth) {
//         if (!item || visited.has(item))
//             return

//         visited.add(item)

//         const colorProperty = root.readDebugProperty(item, "color")

//         if (colorProperty.exists && item.visible) {
//             const position = item.mapToItem(root.contentItem, 0, 0)

//             console.log(
//                 "[PAINTED]",
//                 "name:", root.debugObjectName(item),
//                 "| parent:", root.debugObjectName(item.parent),
//                 "| x:", position.x,
//                 "| y:", position.y,
//                 "| width:", item.width,
//                 "| height:", item.height,
//                 "| color:", String(colorProperty.value),
//                 "| opacity:", item.opacity,
//                 "| z:", item.z
//             )

//         }

//         const children = item.children

//         for (let index = 0; index < children.length; ++index)
//             visit(children[index], depth + 1)
//     }

//     visit(root.contentItem, 0)
// }

// Component.onCompleted: {

//     console.log("New Start")
// }
