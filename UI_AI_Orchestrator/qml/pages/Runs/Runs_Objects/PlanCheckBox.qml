import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// PC-5D: a check box that can be SEEN on the dark dialog surface (the default indicator is a dark square on a dark card): a bordered box
// that fills with the accent colour and shows a check mark when ticked, a dimmed one when it cannot be changed, and a label that wraps
// instead of running out of the dialog. Presentation only; the dialogs that use it own what ticking means.
CheckBox {
    id: control

    required property var hostWindow
    required property var colors

    spacing: control.hostWindow.dp(10)
    padding: 0
    topPadding: control.hostWindow.dp(4)
    bottomPadding: control.hostWindow.dp(4)
    font.pixelSize: control.hostWindow.dp(10.5)

    indicator: Rectangle {
        implicitWidth: control.hostWindow.dp(18)
        implicitHeight: control.hostWindow.dp(18)
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: control.hostWindow.dp(4)
        color: control.checked ? (control.enabled ? control.colors.accent : control.colors.borderStrong) : control.colors.inputBackground
        border.width: 1
        border.color: control.checked ? (control.enabled ? control.colors.accent : control.colors.borderStrong)
                                      : (control.visualFocus ? control.colors.inputBorderFocused : control.colors.borderStrong)
        opacity: control.enabled ? 1.0 : 0.65

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: control.colors.textOnAccent
            font.pixelSize: control.hostWindow.dp(12)
            font.weight: Font.Bold
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        textFormat: Text.PlainText
        wrapMode: Text.WrapAnywhere
        color: control.enabled ? control.colors.textPrimary : control.colors.textMuted
        font: control.font
        verticalAlignment: Text.AlignVCenter
    }
}
