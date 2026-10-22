import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "executionProfileDialog"

    required property var hostWindow
    required property var colors
    required property var providersModel
    required property var modelsModel

    property bool editMode: false
    property string profileId: ""
    property var originalAssignments: []
    property var originalProviderBudget: ({})
    property string errorText: ""
    readonly property string legacyWorkflowId: "execution-profile-v1"
    readonly property string manifestWorkflowId: "execution-manifest-v1"
    readonly property bool manifestMode: String(workflowCombo.currentValue || "")
                                         === manifestWorkflowId
    readonly property var workflowChoices: [
        { label: qsTr("Custom roster (Architect + fixed stages)"), value: legacyWorkflowId },
        { label: qsTr("Manifest (inspect, plan, then execute tasks)"), value: manifestWorkflowId }
    ]

    signal profileAccepted(bool editMode, string profileId, string name,
                           string workflowId, var assignments, var providerBudget)

    function enabledProviders() {
        return dialog.providersModel.filter(function(provider) {
            return provider.isEnabled
        })
    }

    function enabledModels(providerId) {
        return dialog.modelsModel.filter(function(model) {
            return model.providerId === providerId && model.isEnabled
        })
    }

    function assignment(stageType) {
        for (let i = 0; i < originalAssignments.length; ++i) {
            if (originalAssignments[i].stageType === stageType)
                return originalAssignments[i]
        }
        return null
    }

    function selectValue(combo, value) {
        const index = combo.indexOfValue(value)
        combo.currentIndex = index >= 0 ? index : (combo.count > 0 ? 0 : -1)
    }

    function resetStage(providerCombo, modelCombo, timeoutField, stageType) {
        const stored = assignment(stageType)
        selectValue(providerCombo, stored ? stored.providerId : "")
        timeoutField.value = stored
                ? Math.max(timeoutField.from,
                           Math.round(Number(stored.timeoutMs) / 1000))
                : timeoutField.value
        Qt.callLater(function() {
            selectValue(modelCombo, stored ? stored.modelId : "")
        })
    }

    function openForCreate() {
        editMode = false
        profileId = ""
        originalAssignments = []
        originalProviderBudget = ({})
        nameField.text = ""
        selectValue(workflowCombo, legacyWorkflowId)
        errorText = ""
        architectTimeout.value = 120
        implementationTimeout.value = 180
        documentationTimeout.value = 90
        resetBudgetFields()
        resetStage(architectProvider, architectModel, architectTimeout, "architect")
        resetStage(implementationProvider, implementationModel, implementationTimeout, "implementation")
        resetStage(documentationProvider, documentationModel, documentationTimeout, "documentation")
        open()
    }

    function openForEdit(profile) {
        editMode = true
        profileId = profile.profileId
        originalAssignments = profile.assignments || []
        originalProviderBudget = profile.providerBudget || ({})
        nameField.text = profile.name
        selectValue(workflowCombo, profile.workflowId)
        errorText = ""
        resetBudgetFields()
        resetStage(architectProvider, architectModel, architectTimeout, "architect")
        resetStage(implementationProvider, implementationModel, implementationTimeout, "implementation")
        resetStage(documentationProvider, documentationModel, documentationTimeout, "documentation")
        open()
    }

    function makeAssignment(stageType, providerCombo, modelCombo, timeoutField) {
        return {
            stageType: stageType,
            providerId: String(providerCombo.currentValue || ""),
            modelId: String(modelCombo.currentValue || ""),
            timeoutMs: Math.round(Number(timeoutField.value) * 1000)
        }
    }

    function resetBudgetFields() {
        const budget = originalProviderBudget || ({})
        maxInputTokens.value = Number(budget.maxInputTokens || 500000)
        maxOutputTokens.value = Number(budget.maxOutputTokens || 64000)
        maxTurns.value = Number(budget.maxTurns || 24)
        maxToolCalls.value = Number(budget.maxToolCalls || 128)
        maxToolResultMiB.value = Math.max(1, Math.round(Number(
            budget.maxToolResultBytes || 8388608) / 1048576))
        maxWallMinutes.value = Math.max(1, Math.round(Number(
            budget.maxWallTimeMs || 900000) / 60000))
        maxRequestSeconds.value = Math.max(1, Math.round(Number(
            budget.maxRequestTimeMs || 120000) / 1000))
    }

    function providerBudget() {
        return {
            maxInputTokens: Number(maxInputTokens.value),
            maxOutputTokens: Number(maxOutputTokens.value),
            maxTurns: Number(maxTurns.value),
            maxToolCalls: Number(maxToolCalls.value),
            maxToolResultBytes: Number(maxToolResultMiB.value) * 1048576,
            maxWallTimeMs: Number(maxWallMinutes.value) * 60000,
            maxRequestTimeMs: Number(maxRequestSeconds.value) * 1000
        }
    }

    function submit() {
        const assignments = manifestMode
                ? [makeAssignment("architect", architectProvider, architectModel, architectTimeout)]
                : [
                    makeAssignment("architect", architectProvider, architectModel, architectTimeout),
                    makeAssignment("implementation", implementationProvider, implementationModel, implementationTimeout),
                    makeAssignment("documentation", documentationProvider, documentationModel, documentationTimeout)
                ]
        if (nameField.text.trim().length === 0) {
            errorText = qsTr("Profile name is required")
            return
        }
        for (let i = 0; i < assignments.length; ++i) {
            if (assignments[i].providerId.length === 0 || assignments[i].modelId.length === 0) {
                errorText = qsTr("Choose an enabled provider and model for every stage")
                return
            }
        }
        profileAccepted(editMode, profileId, nameField.text.trim(),
                        String(workflowCombo.currentValue || ""), assignments,
                        providerBudget())
    }

    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - hostWindow.dp(32) : hostWindow.dp(680),
                    hostWindow.dp(680))
    modal: true
    title: editMode ? qsTr("Edit execution profile") : qsTr("New execution profile")
    standardButtons: Dialog.NoButton

    background: Rectangle {
        radius: dialog.colors.cornerRadiusMedium
        color: dialog.colors.popupBackground
        border.width: 1
        border.color: dialog.colors.border
    }

    contentItem: ColumnLayout {
        spacing: dialog.hostWindow.dp(10)

        Text {
            Layout.fillWidth: true
            text: dialog.manifestMode
                  ? qsTr("The selected model inspects the workspace, freezes a Manifest, and executes its bounded tasks. A saved profile holds one model; to plan with a lead and execute with several available workers, use New run > Configure agents > Manifest.")
                  : qsTr("Each selected provider/model pair becomes an immutable snapshot when a Run starts.")
            color: dialog.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(11)
        }

        TextField {
            id: nameField
            objectName: "profileNameField"
            Layout.fillWidth: true
            placeholderText: qsTr("Profile name")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(12)
        }

        ComboBox {
            id: workflowCombo
            objectName: "profileWorkflowCombo"
            Layout.fillWidth: true
            model: dialog.workflowChoices
            textRole: "label"
            valueRole: "value"
            font.pixelSize: dialog.hostWindow.dp(11)
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: dialog.hostWindow.dp(8)
            rowSpacing: dialog.hostWindow.dp(8)

            Text { text: qsTr("Stage"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            Text { text: qsTr("Provider"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            Text { text: qsTr("Model"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            Text { text: qsTr("Timeout, s"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }

            Text { text: dialog.manifestMode ? qsTr("Planner + workers") : qsTr("Architect"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(10) }
            ComboBox { id: architectProvider; objectName: "profileArchitectProvider"; Layout.fillWidth: true; model: dialog.enabledProviders(); textRole: "name"; valueRole: "providerId" }
            ComboBox { id: architectModel; objectName: "profileArchitectModel"; Layout.fillWidth: true; model: dialog.enabledModels(String(architectProvider.currentValue || "")); textRole: "name"; valueRole: "modelId" }
            SpinBox { id: architectTimeout; from: 5; to: 600; value: 120; editable: true }

            Text { objectName: "profileImplementationLabel"; visible: !dialog.manifestMode; text: qsTr("Implementation"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(10) }
            ComboBox { id: implementationProvider; visible: !dialog.manifestMode; Layout.fillWidth: true; model: dialog.enabledProviders(); textRole: "name"; valueRole: "providerId" }
            ComboBox { id: implementationModel; visible: !dialog.manifestMode; Layout.fillWidth: true; model: dialog.enabledModels(String(implementationProvider.currentValue || "")); textRole: "name"; valueRole: "modelId" }
            SpinBox { id: implementationTimeout; visible: !dialog.manifestMode; from: 5; to: 600; value: 180; editable: true }

            Text { visible: !dialog.manifestMode; text: qsTr("Documentation"); color: dialog.colors.textPrimary; font.pixelSize: dialog.hostWindow.dp(10) }
            ComboBox { id: documentationProvider; visible: !dialog.manifestMode; Layout.fillWidth: true; model: dialog.enabledProviders(); textRole: "name"; valueRole: "providerId" }
            ComboBox { id: documentationModel; visible: !dialog.manifestMode; Layout.fillWidth: true; model: dialog.enabledModels(String(documentationProvider.currentValue || "")); textRole: "name"; valueRole: "modelId" }
            SpinBox { id: documentationTimeout; visible: !dialog.manifestMode; from: 5; to: 600; value: 90; editable: true }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Provider spending limits (Backend-enforced per Run)")
            color: dialog.colors.textPrimary
            font.pixelSize: dialog.hostWindow.dp(10)
            font.weight: Font.DemiBold
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: dialog.hostWindow.dp(8)
            rowSpacing: dialog.hostWindow.dp(6)

            Text { text: qsTr("Input tokens"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            SpinBox { id: maxInputTokens; objectName: "profileMaxInputTokens"; from: 1; to: 1000000; value: 500000; editable: true; Layout.fillWidth: true }
            Text { text: qsTr("Output tokens"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            SpinBox { id: maxOutputTokens; objectName: "profileMaxOutputTokens"; from: 1; to: 128000; value: 64000; editable: true; Layout.fillWidth: true }

            Text { text: qsTr("Provider turns"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            SpinBox { id: maxTurns; objectName: "profileMaxTurns"; from: 1; to: 64; value: 24; editable: true; Layout.fillWidth: true }
            Text { text: qsTr("Tool calls"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            SpinBox { id: maxToolCalls; objectName: "profileMaxToolCalls"; from: 1; to: 256; value: 128; editable: true; Layout.fillWidth: true }

            Text { text: qsTr("Tool output, MiB"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            SpinBox { id: maxToolResultMiB; objectName: "profileMaxToolResultMiB"; from: 1; to: 16; value: 8; editable: true; Layout.fillWidth: true }
            Text { text: qsTr("Run time, min"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            SpinBox { id: maxWallMinutes; objectName: "profileMaxWallMinutes"; from: 1; to: 30; value: 15; editable: true; Layout.fillWidth: true }

            Text { text: qsTr("Request timeout, s"); color: dialog.colors.textMuted; font.pixelSize: dialog.hostWindow.dp(9) }
            SpinBox { id: maxRequestSeconds; objectName: "profileMaxRequestSeconds"; from: 1; to: 300; value: 120; editable: true; Layout.fillWidth: true }
            Item { Layout.fillWidth: true }
            Item { Layout.fillWidth: true }
        }

        Text {
            id: errorLabel
            Layout.fillWidth: true
            visible: dialog.errorText.length > 0
            text: dialog.errorText
            color: dialog.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: dialog.hostWindow.dp(10)
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ProviderActionButton {
                text: qsTr("Cancel")
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.reject()
            }
            ProviderActionButton {
                text: dialog.editMode ? qsTr("Save profile") : qsTr("Create profile")
                primary: true
                hostWindow: dialog.hostWindow
                colors: dialog.colors
                onClicked: dialog.submit()
            }
        }
    }
}
