import QtQuick
import QtQuick.Window

Item {
    id: resizeHandler

    required property var hostWindow
    property real handleThickness: 6

    readonly property bool resizeEnabled: hostWindow.visibility !== Window.Maximized

    WindowResizeArea {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: resizeHandler.handleThickness
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.LeftEdge
        cursorShape: Qt.SizeHorCursor
    }

    WindowResizeArea {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: resizeHandler.handleThickness
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.RightEdge
        cursorShape: Qt.SizeHorCursor
    }

    WindowResizeArea {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: resizeHandler.handleThickness
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.TopEdge
        cursorShape: Qt.SizeVerCursor
    }

    WindowResizeArea {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: resizeHandler.handleThickness
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.BottomEdge
        cursorShape: Qt.SizeVerCursor
    }

    WindowResizeArea {
        anchors.top: parent.top
        anchors.left: parent.left
        width: resizeHandler.handleThickness * 2
        height: resizeHandler.handleThickness * 2
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.TopEdge | Qt.LeftEdge
        cursorShape: Qt.SizeFDiagCursor
    }

    WindowResizeArea {
        anchors.top: parent.top
        anchors.right: parent.right
        width: resizeHandler.handleThickness * 2
        height: resizeHandler.handleThickness * 2
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.TopEdge | Qt.RightEdge
        cursorShape: Qt.SizeBDiagCursor
    }

    WindowResizeArea {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: resizeHandler.handleThickness * 2
        height: resizeHandler.handleThickness * 2
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.BottomEdge | Qt.LeftEdge
        cursorShape: Qt.SizeBDiagCursor
    }

    WindowResizeArea {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: resizeHandler.handleThickness * 2
        height: resizeHandler.handleThickness * 2
        enabled: resizeHandler.resizeEnabled
        hostWindow: resizeHandler.hostWindow
        edges: Qt.BottomEdge | Qt.RightEdge
        cursorShape: Qt.SizeFDiagCursor
    }
}
