import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One participant of the per-Run roster in NewRunDialog. It shows the entry it is
// given and reports every edit through fieldChanged(field, value); the dialog owns
// the roster model and writes the change back, so this row never touches it.
ColumnLayout {
    id: row
    objectName: "rosterMemberRow"

    required property var hostWindow
    required property var colors
    required property var historyStore
    required property var agentChoices
    required property var fallbackChoices
    required property bool manifestRoster
    // Whether this row offers the instructions field (a Manifest lead has none).
    required property bool instructionsApplicable
    // Roles of the roster model entry this row shows.
    required property int index
    required property string role
    required property string instructions
    required property int modelChoiceIndex
    required property int maxAttempts
    required property int fallbackChoiceIndex
    required property bool fallbackMissing
    required property bool workspaceAccess

    signal fieldChanged(string field, var value)

    Layout.fillWidth: true
    spacing: row.hostWindow.dp(4)
    RowLayout {
        Layout.fillWidth: true
        spacing: row.hostWindow.dp(6)
        Text {
            text: row.manifestRoster
                  ? (row.index === 0 ? qsTr("Lead") : qsTr("Worker %1").arg(row.index))
                  : (row.index === 0 ? qsTr("Architect") : qsTr("Agent %1").arg(row.index + 1))
            color: row.index === 0 ? row.colors.accent : row.colors.textSecondary
            font.pixelSize: row.hostWindow.dp(10)
            Layout.preferredWidth: row.hostWindow.dp(70)
        }
        TextField {
            Layout.fillWidth: true
            Layout.preferredWidth: row.hostWindow.dp(120)
            text: row.role
            placeholderText: row.manifestRoster && row.index > 0
                             ? qsTr("Role, e.g. recent-alerts")
                             : qsTr("Role")
            color: row.colors.textPrimary
            font.pixelSize: row.hostWindow.dp(10)
            onEditingFinished: row.fieldChanged("role", text.trim())
        }
        ComboBox {
            Layout.fillWidth: true
            Layout.preferredWidth: row.hostWindow.dp(210)
            Layout.minimumWidth: row.hostWindow.dp(120)
            model: row.agentChoices
            textRole: "label"
            currentIndex: row.modelChoiceIndex
            font.pixelSize: row.hostWindow.dp(9)
            onActivated: selectedIndex => row.fieldChanged("modelChoiceIndex", selectedIndex)
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: row.hostWindow.dp(6)
        // A Manifest lead only plans, so it has no workspace toggle; a
        // Manifest worker always has workspace access (required to be
        // assignable), shown checked and fixed.
        visible: !(row.manifestRoster && row.index === 0)
        Item { Layout.preferredWidth: row.hostWindow.dp(70) }
        CheckBox {
            id: workspaceAccessCheck
            objectName: "workspaceAccessCheck"
            text: row.manifestRoster
                  ? qsTr("Workspace access (required for workers)")
                  : qsTr("Workspace access")
            enabled: !row.manifestRoster
            checked: row.manifestRoster || row.workspaceAccess
            font.pixelSize: row.hostWindow.dp(9)
            // Read-only workspace tools + propose_change_set for
            // this agent, instead of a single-shot completion -
            // does not support retry attempts or a fallback
            // model (see the Attempts/Fallback row below, which
            // this disables - kept in its own row rather than
            // that one, since a control cannot sit inside the
            // row it disables).
            onToggled: row.fieldChanged("workspaceAccess", checked)
        }
        Item { Layout.fillWidth: true }
    }
    SuggestionTextField {
        Layout.fillWidth: true
        visible: row.instructionsApplicable
        hostWindow: row.hostWindow
        colors: row.colors
        historyStore: row.historyStore
        text: row.instructions
        placeholderText: row.manifestRoster
                         ? qsTr("Instructions for this worker: the lead reads them when assigning, the worker receives them (optional)")
                         : (row.index === 0
                            ? qsTr("Architect context or orchestration constraints (optional)")
                            : qsTr("Agent-specific task context (optional)"))
        color: row.colors.textPrimary
        font.pixelSize: row.hostWindow.dp(9)
        onCommitted: value => row.fieldChanged("instructions", value)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: row.hostWindow.dp(6)
        // A workspace-access agent does not support
        // retry attempts or a fallback model (see
        // rosterPayload()'s run.workspace_access_no_retry
        // note) - visibly disabled rather than hidden so
        // it stays clear these values are ignored, not lost.
        // A Manifest roster never uses attempts or a fallback (one attempt
        // per task, no provider fallback), so the row is not offered at all.
        visible: !row.manifestRoster
        enabled: !row.workspaceAccess
        opacity: row.workspaceAccess ? 0.5 : 1.0
        Item { Layout.preferredWidth: row.hostWindow.dp(70) }
        Text {
            text: qsTr("Attempts")
            color: row.colors.textSecondary
            font.pixelSize: row.hostWindow.dp(9)
        }
        SpinBox {
            from: 1
            to: 5
            value: row.maxAttempts
            editable: true
            onValueModified: row.fieldChanged("maxAttempts", value)
        }
        Text {
            text: qsTr("Fallback")
            color: row.colors.textSecondary
            font.pixelSize: row.hostWindow.dp(9)
        }
        ComboBox {
            Layout.fillWidth: true
            model: row.fallbackChoices
            textRole: "label"
            currentIndex: row.fallbackChoiceIndex + 1
            font.pixelSize: row.hostWindow.dp(9)
            onActivated: selectedIndex => {
                row.fieldChanged("fallbackChoiceIndex", selectedIndex - 1)
                row.fieldChanged("fallbackMissing", false)
            }
        }
    }
}
