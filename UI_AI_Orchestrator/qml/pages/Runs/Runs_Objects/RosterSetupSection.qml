import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The per-Run roster setup of NewRunDialog: Manifest switch, agent count, parallelism,
// bulk model choice and one RosterMemberRow per roster entry. The dialog keeps the
// roster model and the request building; this section shows what it is given and
// reports edits through signals.
//
// A roster agent can be given read-only workspace tools +
// propose_change_set individually (see the per-agent "Workspace
// access" toggle) - the model inspects the project and
// produces a proposal.v1 change proposal that is only ever staged
// for review, never applied to the workspace by this run itself.
ColumnLayout {
    id: section
    objectName: "rosterSetupSection"

    required property var hostWindow
    required property var colors
    required property var historyStore
    required property var rosterModel
    required property var agentChoices
    required property var fallbackChoices
    required property bool manifestRoster
    required property bool rosterModelsMissing
    required property bool wideLayout
    required property string duplicateRoleWarning
    required property int defaultMaxParallelAgents
    // function(index) -> bool: whether the row at index offers the instructions field.
    required property var instructionsApplicable

    property alias manifestChecked: manifestRosterCheck.checked
    property alias agentCount: agentCountSpin.value
    property alias maxParallelism: maxParallelismSpin.value
    readonly property int bulkModelIndex: bulkModelCombo.currentIndex

    signal manifestToggled(bool checked)
    signal agentCountEdited(int count)
    signal applyModelToAllRequested(int choiceIndex)
    signal memberFieldChanged(int index, string field, var value)

    spacing: section.hostWindow.dp(8)

    CheckBox {
        id: manifestRosterCheck
        objectName: "manifestRosterCheck"
        Layout.fillWidth: true
        text: qsTr("Manifest: the lead plans, available workers execute")
        checked: false
        font.pixelSize: section.hostWindow.dp(11)
        ToolTip.visible: hovered
        ToolTip.text: qsTr("The lead inspects the workspace and freezes a task plan; each task is assigned to one of the available workers. Several tasks may go to one worker and a worker may get none - an unused worker is normal, not an error.")
        onToggled: section.manifestToggled(checked)
    }

    Text {
        objectName: "newRunRosterSetupHint"
        Layout.fillWidth: true
        visible: section.rosterModelsMissing
        text: qsTr("This draft has no roster yet. Choose a model for the lead and for every worker below; the Run cannot start until you do. A plan never chooses models or cost.")
        color: section.colors.warning
        wrapMode: Text.WordWrap
        font.pixelSize: section.hostWindow.dp(10)
    }

    Text {
        objectName: "manifestRosterHint"
        Layout.fillWidth: true
        visible: manifestRosterCheck.checked
        text: qsTr("Row 1 is the lead (plans only). The other rows are the available workers. Role: a short name for each worker, for example recent-alerts. The lead assigns tasks by role, so give every worker its own role and use the same names in the task text. Instructions (workers only, optional): what the worker is good at - the lead reads them when assigning, and the worker receives them as its own instructions. Workers always get workspace access, one attempt and no provider fallback.")
        color: section.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: section.hostWindow.dp(9)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: section.hostWindow.dp(8)
        Text {
            // A fixed width: the label changes with the mode, and it must not move the
            // controls next to it.
            Layout.preferredWidth: section.hostWindow.dp(104)
            text: section.manifestRoster ? qsTr("Lead + workers") : qsTr("Agents")
            color: section.colors.textPrimary
            elide: Text.ElideRight
            font.pixelSize: section.hostWindow.dp(12)
        }
        SpinBox {
            id: agentCountSpin
            objectName: "agentCountSpin"
            from: section.manifestRoster ? 2 : 1
            to: 20
            value: 1
            editable: true
            onValueModified: section.agentCountEdited(value)
        }
        Text {
            text: qsTr("Parallel")
            color: section.colors.textSecondary
            font.pixelSize: section.hostWindow.dp(10)
        }
        SpinBox {
            id: maxParallelismSpin
            from: 1
            to: 20
            value: Math.min(section.defaultMaxParallelAgents,
                            agentCountSpin.value)
            editable: true
        }
        Item { Layout.fillWidth: true }
    }

    // Its own row: next to the count and parallelism it did not fit, and the
    // "Apply to all" button was cut off.
    RowLayout {
        Layout.fillWidth: true
        spacing: section.hostWindow.dp(8)
        visible: agentCountSpin.value > 1
        ComboBox {
            id: bulkModelCombo
            // A single agent already has its own model picker in the
            // roster row below; a second, duplicate control here is
            // just noise, so it only appears once there's more than
            // one agent to bulk-assign.
            visible: agentCountSpin.value > 1
            Layout.fillWidth: true
            Layout.minimumWidth: section.hostWindow.dp(120)
            model: section.agentChoices
            textRole: "label"
            font.pixelSize: section.hostWindow.dp(10)
        }
        RunActionButton {
            visible: agentCountSpin.value > 1
            hostWindow: section.hostWindow
            colors: section.colors
            text: qsTr("Apply to all")
            enabled: bulkModelCombo.currentIndex >= 0
            onClicked: section.applyModelToAllRequested(bulkModelCombo.currentIndex)
        }
    }

    ScrollView {
        id: rosterScroll
        Layout.fillWidth: true
        // Wide: every row is shown and the dialog's own scroll area scrolls (no nested
        // scrollbars). Narrow: a bounded list that scrolls on its own.
        Layout.preferredHeight: section.wideLayout
                                ? rosterColumn.implicitHeight + section.hostWindow.dp(4)
                                : section.hostWindow.dp(190)
        contentWidth: availableWidth
        clip: true
        rightPadding: section.hostWindow.dp(12)
        ColumnLayout {
            id: rosterColumn
            width: rosterScroll.availableWidth
            spacing: section.hostWindow.dp(6)
            Repeater {
                model: section.rosterModel
                delegate: RosterMemberRow {
                    hostWindow: section.hostWindow
                    colors: section.colors
                    historyStore: section.historyStore
                    agentChoices: section.agentChoices
                    fallbackChoices: section.fallbackChoices
                    manifestRoster: section.manifestRoster
                    instructionsApplicable: section.instructionsApplicable(index)
                    onFieldChanged: (field, value) => section.memberFieldChanged(index, field, value)
                }
            }
        }
    }

    Text {
        objectName: "manifestRoleWarning"
        Layout.fillWidth: true
        visible: section.duplicateRoleWarning.length > 0
        text: section.duplicateRoleWarning
        color: section.colors.warning
        wrapMode: Text.WordWrap
        font.pixelSize: section.hostWindow.dp(9)
    }
}
