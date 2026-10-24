import QtQuick
import "../Common_Objects"

// One page slot of PageHost.
//
// Lifecycle: the page is created the first time its slot is selected and the instance is kept afterwards, so the
// selection, typed input, tabs, scroll position and open dialogs of a page survive navigation. Nothing is created
// in advance and the Loader is never deactivated.
//
// Resize isolation: a page does not see the real window and theme. It gets `pageWindow` (dp(), scaleFactor, width,
// height - the members pages use) and `pageColors` (an AppTheme driven by the page's own scale). Both follow the
// window only while the slot is selected; a hidden page keeps its last values and catches up when it is shown.
// The page item is sized from pageWindow.pageWidth/pageHeight instead of being anchored or laid out, so a hidden
// page does not receive the size changes of PageHost either.
FocusScope {
    id: root

    required property bool selected
    required property var hostWindow
    required property var colors
    property Component sourceComponent

    readonly property var pageWindow: pageScale
    readonly property var pageColors: pageTheme
    readonly property var item: loader.item
    readonly property bool opened: loader.active

    property bool openRequested: false

    function syncNow() {
        pageScale.scaleFactor = root.hostWindow.scaleFactor
        pageScale.width = root.hostWindow.width
        pageScale.height = root.hostWindow.height
        pageScale.pageWidth = root.width
        pageScale.pageHeight = root.height
    }

    // Open on the first selection, once the slot has a real size (StackLayout sizes it after construction), so the
    // page is built a single time at its final geometry and scale.
    function tryOpen() {
        if (root.openRequested || !root.selected || root.width <= 0 || root.height <= 0)
            return
        root.syncNow()
        root.openRequested = true
    }

    onSelectedChanged: {
        if (root.selected) {
            if (root.openRequested)
                root.syncNow()
            else
                root.tryOpen()
        }
    }
    onWidthChanged: root.tryOpen()
    onHeightChanged: root.tryOpen()
    Component.onCompleted: root.tryOpen()

    QtObject {
        id: pageScale

        property real scaleFactor: 1.0
        property real width: 0
        property real height: 0
        property real pageWidth: 0
        property real pageHeight: 0

        function dp(value) {
            return Math.round(value * pageScale.scaleFactor * 100) / 100
        }
    }

    // Same type and the same settings as the application theme, but the scaled metrics (radii, control heights,
    // spacing, margins) follow the page's own scale.
    AppTheme {
        id: pageTheme

        mode: root.colors.mode
        scaleFactor: pageScale.scaleFactor
        compactMode: root.colors.compactMode
        reduceAnimations: root.colors.reduceAnimations
    }

    // Follow the window only while the slot is selected.
    Connections {
        target: root.hostWindow
        enabled: root.selected && root.openRequested
        function onScaleFactorChanged() { pageScale.scaleFactor = root.hostWindow.scaleFactor }
        function onWidthChanged() { pageScale.width = root.hostWindow.width }
        function onHeightChanged() { pageScale.height = root.hostWindow.height }
    }

    Connections {
        target: root
        enabled: root.selected && root.openRequested
        function onWidthChanged() { pageScale.pageWidth = root.width }
        function onHeightChanged() { pageScale.pageHeight = root.height }
    }

    // No size on the Loader: it then takes the size of the page item, which is set explicitly by the page
    // component from pageWindow.pageWidth/pageHeight, and never pushes the slot size into a hidden page.
    Loader {
        id: loader

        active: root.openRequested
        focus: true
        sourceComponent: root.sourceComponent
    }
}
