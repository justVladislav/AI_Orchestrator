import QtQuick
import QtQuick.Controls

// Small icon-only action button used across the Chats page (header actions,
// message actions, composer actions). Mirrors the hover/pressed/focus
// conventions of ThemeModeButton and WindowControlButton, but is local to
// the Chats feature since those components live in Common_Objects and are
// tied to window-chrome specifics.
AbstractButton {
    id: control

    required property var hostWindow
    required property var colors
    required property string iconGlyph
    required property string accessibleLabel

    property bool danger: false
    // A visible frame for an action that is easy to miss as a bare glyph (the composer's "+").
    property bool outlined: false

    implicitWidth: hostWindow.dp(30)
    implicitHeight: hostWindow.dp(30)
    hoverEnabled: true

    Accessible.name: accessibleLabel
    ToolTip.visible: hovered
    ToolTip.delay: 500
    ToolTip.text: accessibleLabel

    contentItem: Text {
        text: control.iconGlyph
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.danger
               ? (control.hovered || control.down
                  ? control.colors.textOnAccent
                  : control.colors.error)
               : control.enabled
                 ? control.colors.textSecondary
                 : control.colors.textDisabled
        font.pixelSize: control.hostWindow.dp(14)
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.danger
               ? (control.hovered || control.down
                  ? control.colors.error
                  : "transparent")
               : control.down
                 ? control.colors.buttonPressed
                 : control.hovered
                   ? control.colors.buttonHover
                   : control.outlined ? control.colors.buttonBackground : "transparent"
        border.width: control.visualFocus || control.outlined ? 1 : 0
        border.color: control.visualFocus ? control.colors.focusRing : control.colors.border
    }
}
