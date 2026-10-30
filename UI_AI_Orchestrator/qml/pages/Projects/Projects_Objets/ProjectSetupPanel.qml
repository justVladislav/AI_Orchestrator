pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// AG-18.4B-R1: the selected project's durable verification setup - is it configured and buildable on this computer? Everything
// shown here is what the Backend RECORDED (a cheap read); "Check again" runs the real, provider-free check (a configure) on
// demand, and when several configure/build pairs are usable the user chooses one here. The panel never starts anything by
// itself and never talks to the build tools or the database: it only calls backend.projectVerificationSetup.
ColumnLayout {
    id: panel
    objectName: "projectSetupPanel"

    required property var hostWindow
    required property var colors
    required property var backend
    required property string projectId
    required property string projectPath

    property var status: ({exists: false, checking: false, candidates: []})
    property string checkError: ""
    property double activeCheckId: 0

    readonly property string stateName: panel.status.exists === true ? panel.status.state : "unverified"
    readonly property bool checking: panel.status.checking === true
    readonly property bool choiceNeeded: panel.status.exists === true
        && panel.status.failureCode === "project_setup.preset_selection_required"
    readonly property string headline: {
        if (panel.checking) return qsTr("Checking…")
        if (panel.stateName === "ready")
            return qsTr("Ready — configured, %1 build targets confirmed").arg(panel.status.targetCount)
        if (panel.stateName === "stale") return qsTr("Needs a new check — the project changed since the last one")
        if (panel.stateName === "failed") return qsTr("Not ready")
        return panel.choiceNeeded ? qsTr("Choose a preset pair") : qsTr("Not checked yet")
    }
    readonly property color headlineColor: {
        if (panel.stateName === "ready") return panel.colors.success
        if (panel.stateName === "failed") return panel.colors.error
        return panel.colors.warning
    }
    readonly property string detail: {
        if (panel.stateName === "ready") return ""
        if (panel.status.exists === true && panel.status.failureMessage.length > 0) return panel.status.failureMessage
        if (panel.stateName === "stale")
            return qsTr("It is checked again automatically before the next Run, or press Check again.")
        return qsTr("The first Run proves the setup (it configures the project, at no provider cost), or press Check again.")
    }

    Layout.fillWidth: true
    spacing: hostWindow.dp(8)

    function refresh() {
        panel.status = panel.projectId.length > 0
            ? panel.backend.projectVerificationSetup.projectSetupStatus(panel.projectId)
            : ({exists: false, checking: false, candidates: []})
    }

    function requestCheck() {
        panel.checkError = ""
        panel.activeCheckId = panel.backend.projectVerificationSetup.checkProjectSetup(panel.projectId, panel.projectPath)
        panel.refresh()
    }

    function chooseCandidate(pairText) {
        const parts = pairText.split(" / ")
        if (parts.length !== 2) return
        const chosen = panel.backend.projectVerificationSetup.selectProjectSetupPair(
            panel.projectId, panel.projectPath, parts[0], parts[1])
        if (chosen.success === true) panel.requestCheck()
        else panel.checkError = chosen.errorCode + ": " + chosen.errorText
    }

    onProjectIdChanged: panel.refresh()
    onVisibleChanged: if (panel.visible) panel.refresh()
    Component.onCompleted: Qt.callLater(panel.refresh)

    Connections {
        target: panel.backend.projectVerificationSetup
        function onProjectSetupChanged(changedProjectId) {
            if (changedProjectId === panel.projectId) panel.refresh()
        }
        function onProjectSetupCheckFinished(checkId, changedProjectId, ready, errorCode, errorText) {
            if (checkId !== panel.activeCheckId) return
            panel.activeCheckId = 0
            panel.checkError = ready || errorCode === "project_setup.preset_selection_required" ? "" : errorCode + ": " + errorText
            panel.refresh()
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: setupContent.implicitHeight + panel.hostWindow.dp(22)
        radius: panel.colors.cornerRadiusSmall
        color: panel.colors.inputBackground
        border.width: 1
        border.color: panel.colors.border

        ColumnLayout {
            id: setupContent
            anchors.fill: parent
            anchors.margins: panel.hostWindow.dp(11)
            spacing: panel.hostWindow.dp(7)

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: qsTr("Verification setup")
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(13)
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "projectSetupHeadline"
                        Layout.fillWidth: true
                        text: panel.headline
                        color: panel.headlineColor
                        wrapMode: Text.WordWrap
                        font.pixelSize: panel.hostWindow.dp(10)
                        font.weight: Font.DemiBold
                    }
                }

                ProjectActionButton {
                    objectName: "projectSetupCheckButton"
                    text: panel.checking ? qsTr("Cancel") : qsTr("Check again")
                    enabled: panel.projectId.length > 0 && panel.projectPath.length > 0
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: {
                        if (panel.checking) panel.backend.projectVerificationSetup.cancelProjectSetupCheck()
                        else panel.requestCheck()
                    }
                }
            }

            Text {
                objectName: "projectSetupPair"
                Layout.fillWidth: true
                visible: panel.status.exists === true && panel.status.configurePreset.length > 0
                text: qsTr("Presets: %1 / %2").arg(panel.status.configurePreset).arg(panel.status.buildPreset)
                color: panel.colors.textSecondary
                elide: Text.ElideMiddle
                font.pixelSize: panel.hostWindow.dp(10)
            }

            Text {
                objectName: "projectSetupDetail"
                Layout.fillWidth: true
                visible: text.length > 0
                text: panel.detail
                textFormat: Text.PlainText
                color: panel.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(10)
            }

            Text {
                objectName: "projectSetupError"
                Layout.fillWidth: true
                visible: panel.checkError.length > 0 && panel.stateName !== "failed"
                text: panel.checkError
                textFormat: Text.PlainText
                color: panel.colors.error
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(10)
            }

            Flow {
                Layout.fillWidth: true
                visible: panel.choiceNeeded && panel.status.candidates.length > 0
                spacing: panel.hostWindow.dp(6)

                Repeater {
                    model: panel.status.candidates

                    ProjectActionButton {
                        required property string modelData
                        objectName: "projectSetupCandidate"
                        text: modelData
                        hostWindow: panel.hostWindow
                        colors: panel.colors
                        onClicked: panel.chooseCandidate(modelData)
                    }
                }
            }
        }
    }

    // P3e: the folders the user declares to hold generated output. Its own lifecycle (it follows this panel's visibility, which is the
    // Projects page's, since pages are kept alive when left).
    ProjectOutputFoldersSection {
        hostWindow: panel.hostWindow
        colors: panel.colors
        backend: panel.backend
        projectId: panel.projectId
        projectPath: panel.projectPath
    }
}
