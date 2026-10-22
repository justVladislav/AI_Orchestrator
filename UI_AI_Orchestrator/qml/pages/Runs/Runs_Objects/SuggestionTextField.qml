pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Drop-in TextField with a Google-search-style MRU suggestions popup: shows
// every previously entered value on focus, filters as the user types, and
// hides entirely when nothing matches. History is local-machine only
// (QtCore.Settings), shared across every instance of this component so all
// roster instruction fields in NewRunDialog pool the same suggestion list.
TextField {
    id: control
    objectName: "suggestionTextField"

    required property var hostWindow
    required property var colors
    // NewRunDialog owns one Settings object and shares it across every
    // roster row. Creating a persistent settings backend per delegate made
    // first-time dialog/agent initialization needlessly expensive.
    required property var historyStore

    // Fires after onEditingFinished's own history bookkeeping runs. A plain
    // onEditingFinished handler declared where this component is used would
    // silently replace this file's own handler (both bind to the same root
    // TextField signal slot) instead of adding to it, so callers must listen
    // for this signal rather than onEditingFinished directly.
    signal committed(string value)

    function historyList() {
        try {
            const parsed = JSON.parse(control.historyStore.history)
            return Array.isArray(parsed) ? parsed : []
        } catch (e) {
            return []
        }
    }

    function rememberCurrentText() {
        const value = control.text.trim()
        if (value.length === 0)
            return
        let list = control.historyList().filter(entry => entry !== value)
        list.unshift(value)
        if (list.length > 20)
            list = list.slice(0, 20)
        control.historyStore.history = JSON.stringify(list)
    }

    function refreshSuggestions() {
        const query = control.text.trim().toLowerCase()
        const all = control.historyList()
        const matches = query.length === 0
                ? all
                : all.filter(entry => entry.toLowerCase().includes(query))
        suggestionPopup.suggestions = matches.slice(0, 8)
        suggestionPopup.visible = control.activeFocus && matches.length > 0
    }

    onTextChanged: control.refreshSuggestions()
    onActiveFocusChanged: {
        if (control.activeFocus)
            control.refreshSuggestions()
        else
            suggestionPopup.visible = false
    }
    onEditingFinished: {
        control.rememberCurrentText()
        control.committed(control.text.trim())
    }

    Popup {
        id: suggestionPopup
        property var suggestions: []
        y: control.height
        x: 0
        width: control.width
        implicitHeight: Math.min(suggestionList.contentHeight, control.hostWindow.dp(160))
        padding: 1

        background: Rectangle {
            radius: control.colors.cornerRadiusSmall
            color: control.colors.popupBackground
            border.width: 1
            border.color: control.colors.border
        }

        contentItem: ListView {
            id: suggestionList
            implicitHeight: contentHeight
            clip: true
            model: suggestionPopup.suggestions
            delegate: ItemDelegate {
                id: suggestionDelegate
                required property string modelData
                width: suggestionList.width
                text: modelData
                font.pixelSize: control.hostWindow.dp(9)
                onClicked: {
                    control.text = suggestionDelegate.modelData
                    control.rememberCurrentText()
                    control.committed(suggestionDelegate.modelData)
                    suggestionPopup.visible = false
                    control.forceActiveFocus()
                }
            }
        }
    }
}
