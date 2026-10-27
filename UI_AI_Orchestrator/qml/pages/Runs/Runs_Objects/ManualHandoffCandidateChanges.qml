pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel

    required property var hostWindow
    required property var colors
    required property var controller
    required property bool confirmingReset

    signal refreshRequested()
    signal resetConfirmationRequested(bool confirming)
    signal resetRequested()

    objectName: "manualHandoffCandidateChanges"
    Layout.fillWidth: true
    spacing: panel.hostWindow.dp(5)

    Text {
        Layout.fillWidth: true
        text: qsTranslate("ManualHandoffPanel", "Candidate changes")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(9)
        font.weight: Font.DemiBold
    }

    ColumnLayout {
        objectName: "manualHandoffEditableChangesSection"
        Layout.fillWidth: true
        spacing: panel.hostWindow.dp(2)
        visible: panel.controller
                 && panel.controller.editableChangedRelativePaths.length > 0
        Text {
            Layout.fillWidth: true
            text: qsTranslate("ManualHandoffPanel", "Editable changes")
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(8.5)
            font.weight: Font.DemiBold
        }
        Repeater {
            model: panel.controller ? panel.controller.editableChangedRelativePaths : []
            delegate: Text {
                required property string modelData
                Layout.fillWidth: true
                text: "• " + modelData
                color: panel.colors.success
                wrapMode: Text.WrapAnywhere
                font.pixelSize: panel.hostWindow.dp(8.5)
            }
        }
    }

    ColumnLayout {
        objectName: "manualHandoffResetRequiredSection"
        Layout.fillWidth: true
        spacing: panel.hostWindow.dp(2)
        visible: panel.controller
                 && panel.controller.resetRequiredRelativePaths.length > 0
        Text {
            Layout.fillWidth: true
            text: qsTranslate("ManualHandoffPanel", "Reset required before Reverify")
            color: panel.colors.warning
            font.pixelSize: panel.hostWindow.dp(8.5)
            font.weight: Font.DemiBold
        }
        Repeater {
            model: panel.controller ? panel.controller.resetRequiredRelativePaths : []
            delegate: Text {
                required property string modelData
                Layout.fillWidth: true
                text: "• " + modelData
                color: panel.colors.warning
                wrapMode: Text.WrapAnywhere
                font.pixelSize: panel.hostWindow.dp(8.5)
            }
        }
    }

    Text {
        Layout.fillWidth: true
        text: qsTranslate("ManualHandoffPanel", "Only the editable changes above will be carried into a "
                   + "successor proposal. Every other file present is build/lint "
                   + "context only. Reverify stays fail-closed until any change "
                   + "requiring reset is cleared.")
        color: panel.colors.textSecondary
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(8)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: panel.hostWindow.dp(7)
        visible: !panel.confirmingReset
        RunActionButton {
            objectName: "manualHandoffRefreshCandidateButton"
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("ManualHandoffPanel", "Refresh candidate changes")
            enabled: panel.controller && !panel.controller.busy
            onClicked: panel.refreshRequested()
        }
        RunActionButton {
            objectName: "manualHandoffResetButton"
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("ManualHandoffPanel", "Reset non-editable changes")
            danger: true
            visible: panel.controller
                     && panel.controller.resetRequiredRelativePaths.length > 0
            enabled: visible && !panel.controller.busy
            onClicked: panel.resetConfirmationRequested(true)
        }
        Item { Layout.fillWidth: true }
    }

    Text {
        objectName: "manualHandoffResetConfirmationText"
        Layout.fillWidth: true
        visible: panel.confirmingReset
        text: panel.controller
              ? qsTranslate("ManualHandoffPanel", "Reset the following non-editable path(s)?\n%1\n\nEvery "
                     + "authorized editable change above is preserved. The live "
                     + "project is never modified.")
                    .arg(panel.controller.resetRequiredRelativePaths.join("\n"))
              : ""
        color: panel.colors.warning
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(9)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: panel.hostWindow.dp(7)
        visible: panel.confirmingReset
        RunActionButton {
            objectName: "manualHandoffConfirmResetButton"
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("ManualHandoffPanel", "Confirm reset")
            danger: true
            enabled: panel.controller && !panel.controller.busy
            onClicked: panel.resetRequested()
        }
        RunActionButton {
            objectName: "manualHandoffCancelResetButton"
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("ManualHandoffPanel", "Cancel")
            enabled: panel.controller && !panel.controller.busy
            onClicked: panel.resetConfirmationRequested(false)
        }
        Item { Layout.fillWidth: true }
    }

    Text {
        objectName: "manualHandoffResetResultText"
        Layout.fillWidth: true
        visible: panel.controller && panel.controller.resetResultText.length > 0
        text: panel.controller ? panel.controller.resetResultText : ""
        color: panel.colors.information
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(9)
    }
}
