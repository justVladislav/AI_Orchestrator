import QtQuick

// Small text-link "copy to clipboard" affordance. Uses a hidden TextEdit as a
// real system-clipboard bridge (TextEdit.copy() needs no extra import) - the
// same convention as ChatMessageDelegate.qml. Briefly flips to "Copied" so the
// click is visibly acknowledged.
Text {
    id: control

    required property var hostWindow
    required property var colors
    property string copyText: ""
    property string label: qsTr("Copy")
    property bool copied: false

    text: control.copied ? qsTr("Copied ✓") : control.label
    color: control.copied ? control.colors.success
           : copyArea.containsMouse ? control.colors.textPrimary : control.colors.accent
    font.pixelSize: control.hostWindow.dp(9)
    opacity: control.copyText.length > 0 ? 1.0 : 0.4
    Accessible.role: Accessible.Button
    Accessible.name: control.label

    function copy() {
        if (control.copyText.length === 0)
            return
        clipboardBridge.text = control.copyText
        clipboardBridge.selectAll()
        clipboardBridge.copy()
        control.copied = true
        resetTimer.restart()
    }

    TextEdit {
        id: clipboardBridge
        visible: false
        text: ""
    }

    Timer {
        id: resetTimer
        interval: 1500
        onTriggered: control.copied = false
    }

    MouseArea {
        id: copyArea
        anchors.fill: parent
        anchors.margins: -control.hostWindow.dp(4)
        enabled: control.copyText.length > 0
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: control.copy()
    }
}
