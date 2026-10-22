import QtQuick

MouseArea {
    id: resizeArea

    required property var hostWindow
    required property int edges

    onPressed: mouse => {
        mouse.accepted = resizeArea.hostWindow.startSystemResize(resizeArea.edges)
    }
}
