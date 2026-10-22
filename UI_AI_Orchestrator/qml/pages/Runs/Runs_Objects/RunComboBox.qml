pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

Basic.ComboBox {
    id: control
    required property var hostWindow
    required property var colors
    required property var options
    required property string accessibleLabel
    property string selectedValue: ""
    signal valueSelected(string value)

    model: options
    implicitHeight: hostWindow.dp(40)
    leftPadding: hostWindow.dp(11)
    rightPadding: hostWindow.dp(30)
    font.pixelSize: hostWindow.dp(11.5)
    Accessible.name: accessibleLabel

    function syncIndex() {
        for (let i = 0; i < options.length; ++i) {
            if (options[i].value === selectedValue) { currentIndex = i; return }
        }
        // No match - e.g. selectedValue is a real id whose entry has not
        // reached this (possibly still-loading) options list yet. Falling
        // back to index 0 would silently display whatever happens to be
        // first (RunsPage's own project filter puts "All projects" there),
        // lying about the actual filter in effect. -1 renders as an empty
        // label (see contentItem.text below) until a real match exists.
        currentIndex = -1
    }
    Component.onCompleted: syncIndex()
    onSelectedValueChanged: syncIndex()
    // options can arrive after selectedValue is already set to a real value
    // (e.g. RunsPage restores a persisted project filter before its own
    // project list has finished loading asynchronously) - re-sync whenever
    // the option set itself changes, not only when selectedValue does.
    // Deferred: Basic.ComboBox's own base implementation reacts to the
    // "model" binding (also driven by options) changing by reconciling
    // currentIndex itself (its own default currentText/role-based match
    // against the new model, which - since this component sets neither
    // textRole nor currentText - finds nothing and resets to 0) within the
    // same synchronous call; that reconciliation clobbers an immediate
    // syncIndex() call here. Qt.callLater runs this once that has already
    // settled, so it wins instead of being silently overwritten.
    onOptionsChanged: Qt.callLater(syncIndex)
    onActivated: index => valueSelected(options[index].value)

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        text: control.currentIndex >= 0 && control.currentIndex < control.options.length ? control.options[control.currentIndex].label : ""
        color: control.colors.textPrimary
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Text {
        x: control.width - width - control.hostWindow.dp(10)
        anchors.verticalCenter: parent.verticalCenter
        text: "⌄"
        color: control.colors.textSecondary
        font.pixelSize: control.hostWindow.dp(14)
    }
    background: Rectangle {
        radius: control.colors.cornerRadiusSmall
        color: control.colors.inputBackground
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? control.colors.inputBorderFocused : control.colors.inputBorder
    }
    delegate: Basic.ItemDelegate {
        id: optionItem
        required property int index
        width: control.width
        text: control.options[index].label
        highlighted: control.highlightedIndex === index
        font.pixelSize: control.hostWindow.dp(11.5)
        contentItem: Text { text: optionItem.text; color: control.colors.textPrimary; font: optionItem.font; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: optionItem.highlighted ? control.colors.accentSoft : control.colors.popupBackground }
    }
    popup: Basic.Popup {
        y: control.height + control.hostWindow.dp(4)
        width: control.width
        padding: control.hostWindow.dp(4)
        implicitHeight: Math.min(contentItem.implicitHeight + control.hostWindow.dp(8), control.hostWindow.dp(260))
        contentItem: ListView {
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            clip: true
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        background: Rectangle { radius: control.colors.cornerRadiusSmall; color: control.colors.popupBackground; border.width: 1; border.color: control.colors.border }
    }
}
