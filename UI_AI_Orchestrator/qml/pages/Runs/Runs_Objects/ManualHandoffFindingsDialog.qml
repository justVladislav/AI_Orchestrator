pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The verification findings of a manual handoff in a window of their own, in a readable size. The handoff panel prints the same lists
// inline in a very small font among many other lines; here they get the whole window, grouped exactly as the Backend grouped them
// (this file classifies nothing, drops nothing and edits nothing), each group with its count and a plain sentence about what it means.
Dialog {
    id: dialog
    objectName: "manualHandoffFindingsDialog"

    required property var hostWindow
    required property var colors

    // "Attempt 2 - failed", or what the findings belong to.
    property string heading: ""
    // The only paths this handoff lets the user change.
    property var editablePaths: []
    // The Backend's groups, each a list of plain strings.
    property var editableFindings: []
    property var outsideScopeFindings: []
    property var newActionableFindings: []
    property var newOutsideFindings: []
    property var generalFindings: []
    property var advisoryFindings: []
    property var baselineDebtFindings: []
    // The shared recorded-findings block reuses this window for its bounded long list. Existing attempt
    // dialogs keep the default mode and their established wording.
    property bool recordedFindingsMode: false
    property string scopeSummary: ""

    readonly property int blockingCandidates: editableFindings.length + newActionableFindings.length + newOutsideFindings.length
                                              + outsideScopeFindings.length

    function groups() {
        return [
            { "id": "editable",
              "title": dialog.recordedFindingsMode
                       ? qsTr("Blocking findings in files the repair workspace may edit")
                       : qsTr("Findings in the files you may edit"),
              "tone": "error",
              "note": dialog.recordedFindingsMode
                      ? qsTr("The exact editable list is authoritative only after the workspace is created.")
                      : qsTr("Fix these in the repair workspace, then verify again."),
              "items": dialog.editableFindings },
            { "id": "newActionable", "title": qsTr("New findings to fix"), "tone": "error",
              "note": qsTr("Introduced by the candidate, in the files you may edit."), "items": dialog.newActionableFindings },
            { "id": "outside",
              "title": dialog.recordedFindingsMode
                       ? qsTr("Blocking findings in files the repair workspace cannot edit")
                       : qsTr("Findings in files you may NOT edit here"),
              "tone": "warning",
              "note": dialog.recordedFindingsMode
                      ? qsTr("These are proven outside this handoff's authority; creating or editing its workspace does not widen that authority.")
                      : qsTr("This handoff cannot change these files. If verification still fails once everything above is fixed, the cause is here: it has to be fixed outside this handoff."),
              "items": dialog.outsideScopeFindings },
            { "id": "newOutside", "title": qsTr("New findings outside the Manifest's write scope"), "tone": "warning",
              "note": qsTr("New in this verification and not in a file the handoff lets you edit."), "items": dialog.newOutsideFindings },
            { "id": "general",
              "title": dialog.recordedFindingsMode
                       ? qsTr("Blocking findings with unknown file authority")
                       : qsTr("General verification messages"),
              "tone": "warning",
              "note": "", "items": dialog.generalFindings },
            { "id": "advisory", "title": qsTr("Advisory findings (do not block)"), "tone": "muted",
              "note": "", "items": dialog.advisoryFindings },
            { "id": "baseline", "title": qsTr("Older warnings that were already there (not blocking)"), "tone": "muted",
              "note": qsTr("Unchanged by this work; they do not make verification fail."), "items": dialog.baselineDebtFindings }
        ]
    }

    function toneColor(tone) {
        return tone === "error" ? dialog.colors.error : tone === "warning" ? dialog.colors.warning : dialog.colors.textSecondary
    }

    anchors.centerIn: Overlay.overlay
    width: Math.min(hostWindow.dp(860), Math.max(hostWindow.dp(340), hostWindow.width - hostWindow.dp(32)))
    height: Math.min(hostWindow.dp(720), hostWindow.height - hostWindow.dp(32))
    modal: true
    closePolicy: Popup.CloseOnEscape
    title: dialog.heading.length > 0 ? qsTr("Verification findings - %1").arg(dialog.heading) : qsTr("Verification findings")
    standardButtons: Dialog.NoButton
    font.pixelSize: hostWindow.dp(13)

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    header: Rectangle {
        implicitHeight: dialog.hostWindow.dp(50)
        color: dialog.colors.surfaceAlternative

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: dialog.hostWindow.dp(18)
            anchors.rightMargin: dialog.hostWindow.dp(18)
            text: dialog.title
            color: dialog.colors.textPrimary
            elide: Text.ElideRight
            font.pixelSize: dialog.hostWindow.dp(15)
            font.weight: Font.DemiBold
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: dialog.colors.separator
        }
    }

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(10)

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            ColumnLayout {
                id: body
                width: scroller.width - dialog.hostWindow.dp(14)
                spacing: dialog.hostWindow.dp(12)

                Text {
                    objectName: "manualHandoffFindingsEditable"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: dialog.scopeSummary.length > 0
                          ? dialog.scopeSummary
                          : dialog.editablePaths.length > 0
                          ? qsTr("Files this handoff lets you edit: %1").arg(dialog.editablePaths.join(", "))
                          : qsTr("No file is listed as editable.")
                    color: dialog.colors.textPrimary
                    font.pixelSize: dialog.hostWindow.dp(13)
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: dialog.groups()
                    delegate: ColumnLayout {
                        id: group
                        required property var modelData
                        objectName: "manualHandoffFindingsGroup_" + group.modelData.id
                        Layout.fillWidth: true
                        visible: group.modelData.items.length > 0
                        spacing: dialog.hostWindow.dp(5)

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("%1 (%2)").arg(group.modelData.title).arg(group.modelData.items.length)
                            color: dialog.toneColor(group.modelData.tone)
                            font.pixelSize: dialog.hostWindow.dp(14)
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: group.modelData.note.length > 0
                            wrapMode: Text.WordWrap
                            text: group.modelData.note
                            color: dialog.colors.textSecondary
                            font.pixelSize: dialog.hostWindow.dp(12)
                        }
                        Repeater {
                            model: group.modelData.items
                            delegate: Rectangle {
                                id: finding
                                required property string modelData
                                Layout.fillWidth: true
                                implicitHeight: findingText.implicitHeight + dialog.hostWindow.dp(16)
                                radius: dialog.colors.cornerRadiusSmall
                                color: dialog.colors.surface
                                border.width: 1
                                border.color: dialog.colors.border

                                Text {
                                    id: findingText
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.margins: dialog.hostWindow.dp(8)
                                    wrapMode: Text.WrapAnywhere
                                    textFormat: Text.PlainText
                                    text: finding.modelData
                                    color: dialog.colors.textPrimary
                                    font.pixelSize: dialog.hostWindow.dp(12.5)
                                }
                            }
                        }
                    }
                }

                Text {
                    objectName: "manualHandoffFindingsNone"
                    Layout.fillWidth: true
                    visible: dialog.blockingCandidates + dialog.generalFindings.length + dialog.advisoryFindings.length
                             + dialog.baselineDebtFindings.length === 0
                    wrapMode: Text.WordWrap
                    text: qsTr("There are no findings to show.")
                    color: dialog.colors.textMuted
                    font.pixelSize: dialog.hostWindow.dp(13)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            RunActionButton {
                objectName: "manualHandoffFindingsCloseButton"
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                text: qsTr("Close")
                primary: true
                onClicked: dialog.close()
            }
        }
    }
}
