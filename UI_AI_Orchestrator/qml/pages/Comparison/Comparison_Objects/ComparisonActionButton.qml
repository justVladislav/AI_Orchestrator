import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Text and/or icon action button for the Comparison page (toolbar Refresh,
// card actions, summary "Send to synthesis", etc.). Supports an icon-only
// mode (empty text) for compact card actions, an externally driven `toggled`
// state for toggles ("select for synthesis", "mark as best"), and
// primary/danger accents built entirely from AppTheme tokens.
Button {
    id: control

    required property var hostWindow
    required property var colors

    property string iconGlyph: ""
    property bool primary: false
    property bool danger: false

    // Deliberately NOT using AbstractButton's built-in checkable/checked:
    // a checkable Button self-toggles `checked` on click, which permanently
    // overwrites any external declarative binding on that property after the
    // first click. `toggled` is a plain property the parent sets from its
    // own state instead, so this button never mutates its own toggle state.
    property bool toggled: false

    // Falls back to `text` when empty; required for icon-only buttons.
    property string accessibleLabel: ""

    readonly property bool iconOnly: text.length === 0 && iconGlyph.length > 0

    hoverEnabled: true
    implicitHeight: hostWindow.dp(32)
    implicitWidth: iconOnly
                   ? implicitHeight
                   : contentRow.implicitWidth + hostWindow.dp(20)
    leftPadding: iconOnly ? 0 : hostWindow.dp(10)
    rightPadding: iconOnly ? 0 : hostWindow.dp(10)
    opacity: enabled ? 1.0 : 0.55

    Accessible.name: accessibleLabel.length > 0 ? accessibleLabel : text
    ToolTip.visible: iconOnly && hovered
    ToolTip.delay: 500
    ToolTip.text: Accessible.name

    readonly property color contentColor: control.toggled
                                          ? control.colors.accent
                                          : control.primary
                                            ? control.colors.textOnAccent
                                            : control.danger
                                              ? (control.hovered || control.down
                                                 ? control.colors.textOnAccent
                                                 : control.colors.error)
                                              : control.colors.textPrimary

    contentItem: RowLayout {
        id: contentRow
        spacing: control.hostWindow.dp(6)

        Text {
            visible: control.iconGlyph.length > 0
            text: control.iconGlyph
            color: control.contentColor
            font.pixelSize: control.hostWindow.dp(13)
            horizontalAlignment: Text.AlignHCenter
            Layout.alignment: Qt.AlignCenter
        }

        Text {
            visible: control.text.length > 0
            text: control.text
            color: control.contentColor
            font.pixelSize: control.hostWindow.dp(12.5)
            font.weight: Font.Medium
        }
    }

    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.toggled
               ? control.colors.accentSoft
               : control.primary
                 ? (control.down ? control.colors.accentPressed
                                 : control.hovered ? control.colors.accentHover
                                                   : control.colors.accent)
                 : control.danger
                   ? (control.down || control.hovered
                      ? control.colors.error
                      : control.colors.errorSoft)
                   : control.down
                     ? control.colors.buttonPressed
                     : control.hovered
                       ? control.colors.buttonHover
                       : control.colors.buttonBackground
        border.width: control.toggled
                      ? 1
                      : (!control.primary && !control.danger) || (control.danger && !control.hovered && !control.down)
                        ? 1
                        : 0
        border.color: control.toggled
                      ? control.colors.accent
                      : control.danger
                        ? control.colors.error
                        : control.colors.border
    }
}
