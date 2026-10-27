pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel

    required property var hostWindow
    required property var colors
    required property var runData
    required property int contractCount

    objectName: "runContractsPanel"
    Layout.fillWidth: true
    visible: panel.contractCount > 0
    Layout.preferredHeight: runContractsColumn.implicitHeight + panel.hostWindow.dp(20)
    radius: panel.colors.cornerRadiusSmall
    color: panel.colors.surface
    border.width: 1
    border.color: panel.colors.border

    ColumnLayout {
        id: runContractsColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.hostWindow.dp(11)
        anchors.rightMargin: panel.hostWindow.dp(11)
        spacing: panel.hostWindow.dp(4)

        Text {
            objectName: "runContractsSummary"
            Layout.fillWidth: true
            text: qsTranslate("RunDetailsPanel", "%1 frozen with this run · fingerprint %2")
                  .arg(panel.contractCount)
                  .arg(panel.runData ? String(panel.runData.interfaceContractsFingerprint || "").slice(0, 12) : "")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(9.5)
            wrapMode: Text.WordWrap
        }

        Repeater {
            objectName: "runContractsRepeater"
            model: panel.runData ? panel.runData.interfaceContracts : []

            delegate: ColumnLayout {
                id: contractItem
                required property var modelData
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(3)

                Text {
                    objectName: "runContractLine"
                    Layout.fillWidth: true
                    text: qsTranslate("RunDetailsPanel", "%1 · %2 · %3 · applies to %4")
                          .arg(contractItem.modelData.id)
                          .arg(contractItem.modelData.kind === "constraint"
                               ? qsTranslate("RunDetailsPanel", "constraint") : qsTranslate("RunDetailsPanel", "interface contract"))
                          .arg(contractItem.modelData.title)
                          .arg(contractItem.modelData.appliesToPaths.length > 0
                               ? contractItem.modelData.appliesToPaths.join(", ")
                               : qsTranslate("RunDetailsPanel", "every worker"))
                    color: panel.colors.textSecondary
                    font.pixelSize: panel.hostWindow.dp(8.5)
                    wrapMode: Text.WordWrap
                }

                // The exact text the Backend hands to the workers, byte for byte (the Backend only
                // normalizes line endings). Read-only but selectable, so it can be compared with a
                // task's output or copied.
                TextEdit {
                    objectName: "runContractText"
                    Layout.fillWidth: true
                    Layout.leftMargin: panel.hostWindow.dp(8)
                    readOnly: true
                    selectByMouse: true
                    text: String(contractItem.modelData.text || "")
                    color: panel.colors.textPrimary
                    selectionColor: panel.colors.accent
                    selectedTextColor: panel.colors.textOnAccent
                    font.family: "monospace"
                    font.pixelSize: panel.hostWindow.dp(8.5)
                    wrapMode: TextEdit.Wrap
                }
            }
        }
    }
}
