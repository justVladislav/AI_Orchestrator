pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Rectangle {
    id: panel
    objectName: "providerDetailsPanel"

    required property var hostWindow
    required property var colors
    required property var modelsModel
    property var providerData: null

    signal editRequested()
    signal testRequested(string providerId)
    signal removeRequested()
    signal providerEnabledChangeRequested(string providerId, bool enabled)
    signal addCatalogModelsRequested(string providerId, string providerKind, string providerName)
    signal addCustomModelRequested(string providerId)
    signal removeModelRequested(string modelId)
    signal defaultModelRequested(string modelId)
    signal modelEnabledChanged(string modelId, bool enabled)
    signal modelPricingRequested(var modelData)
    signal modelEditRequested(var modelData)
    signal configureCredentialRequested(string providerId, string providerName)
    signal removeCredentialRequested(string providerId)

    readonly property int visibleModelCount: {
        const countDependency = modelsModel.length
        if (!providerData)
            return countDependency * 0
        let result = 0
        for (let i = 0; i < modelsModel.length; ++i) {
            if (modelsModel[i].providerId === providerData.providerId)
                ++result
        }
        return result
    }
    readonly property bool hasBuiltInCatalog: providerData !== null
                                                   && (providerData.kind === "openai"
                                                       || providerData.kind === "anthropic")

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border
    clip: true

    Flickable {
        id: detailsFlickable
        anchors.fill: parent
        anchors.margins: panel.hostWindow.dp(16)
        contentWidth: width
        contentHeight: detailsLayout.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { id: detailsScrollBar }

        ColumnLayout {
            id: detailsLayout
            width: Math.max(0, detailsFlickable.width - detailsScrollBar.width
                            - panel.hostWindow.dp(4))
            spacing: panel.hostWindow.dp(14)

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(10)

                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.alignment: Qt.AlignVCenter
                    text: panel.providerData ? panel.providerData.name : qsTr("Select a provider")
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(20)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                Rectangle {
                    objectName: "providerStatusChip"
                    visible: panel.providerData !== null
                    Layout.preferredWidth: providerStatus.implicitWidth + panel.hostWindow.dp(16)
                    Layout.preferredHeight: panel.hostWindow.dp(25)
                    Layout.alignment: Qt.AlignVCenter
                    radius: height / 2
                    color: panel.providerData && panel.providerData.isEnabled
                           ? Qt.rgba(panel.colors.success.r, panel.colors.success.g, panel.colors.success.b, 0.14)
                           : Qt.rgba(panel.colors.textMuted.r, panel.colors.textMuted.g, panel.colors.textMuted.b, 0.14)

                    Text {
                        id: providerStatus
                        anchors.centerIn: parent
                        text: panel.providerData && panel.providerData.isEnabled ? qsTr("Active") : qsTr("Inactive")
                        color: panel.providerData && panel.providerData.isEnabled ? panel.colors.success : panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(9)
                        font.weight: Font.DemiBold
                    }
                }

                ProviderActionButton {
                    text: qsTr("Test (soon)")
                    // ProviderCatalog currently stores metadata only; no
                    // network health-check contract exists in Backend yet.
                    enabled: false
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.testRequested(panel.providerData.providerId)
                }

                ProviderActionButton {
                    objectName: "providerEditAction"
                    text: qsTr("Edit")
                    enabled: panel.providerData !== null
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.editRequested()
                }

                ProviderActionButton {
                    text: qsTr("Remove")
                    danger: true
                    // Deletion semantics are intentionally absent from the
                    // current provider catalog service.
                    visible: false
                    enabled: false
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.removeRequested()
                }
            }

            Text {
                Layout.fillWidth: true
                text: panel.providerData ? panel.providerData.description : qsTr("Connection and model details will appear here.")
                color: panel.colors.textSecondary
                font.pixelSize: panel.hostWindow.dp(11)
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: connectionGrid.implicitHeight + panel.hostWindow.dp(24)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.inputBackground
                border.width: 1
                border.color: panel.colors.border

                GridLayout {
                    id: connectionGrid
                    anchors.fill: parent
                    anchors.margins: panel.hostWindow.dp(12)
                    columns: 2
                    columnSpacing: panel.hostWindow.dp(18)
                    rowSpacing: panel.hostWindow.dp(9)

                    Text { text: qsTr("Type"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                    Text { Layout.fillWidth: true; text: panel.providerData ? panel.providerData.type : "—"; color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(10); elide: Text.ElideRight }

                    Text { text: qsTr("Base URL"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                    Text { Layout.fillWidth: true; text: panel.providerData ? panel.providerData.baseUrl : "—"; color: panel.colors.textSecondary; font.pixelSize: panel.hostWindow.dp(10); elide: Text.ElideMiddle }

                    Text { text: qsTr("Credential"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: panel.hostWindow.dp(7)
                        Text { text: panel.providerData && panel.providerData.credentialState === "configured" ? "••••••••" : qsTr("Not configured"); color: panel.providerData && panel.providerData.credentialState === "configured" ? panel.colors.textPrimary : panel.colors.warning; font.pixelSize: panel.hostWindow.dp(10) }
                        Text { visible: panel.providerData && panel.providerData.credentialState === "configured"; text: qsTr("Configured"); color: panel.colors.success; font.pixelSize: panel.hostWindow.dp(9) }
                    }

                    Text { text: qsTr("Last check"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                    Text { Layout.fillWidth: true; text: panel.providerData ? panel.providerData.lastCheck : "—"; color: panel.colors.textSecondary; font.pixelSize: panel.hostWindow.dp(10); elide: Text.ElideRight }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Credentials are write-only and never displayed after saving.")
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(9)
                    wrapMode: Text.WordWrap
                }

                ProviderActionButton {
                    text: panel.providerData
                          && panel.providerData.credentialState === "configured"
                          ? qsTr("Replace credential") : qsTr("Set credential")
                    enabled: panel.providerData !== null
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.configureCredentialRequested(
                        panel.providerData.providerId, panel.providerData.name)
                }

                ProviderActionButton {
                    text: qsTr("Remove")
                    danger: true
                    visible: panel.providerData
                             && panel.providerData.credentialState === "configured"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.removeCredentialRequested(panel.providerData.providerId)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(10)

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(72)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(10)
                        spacing: panel.hostWindow.dp(3)
                        Text { text: qsTr("Monthly cost"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                        Text { text: panel.providerData ? panel.providerData.monthlyCost : "$0.00"; color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(16); font.weight: Font.DemiBold }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(72)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(10)
                        spacing: panel.hostWindow.dp(3)
                        Text { text: qsTr("Tokens this month"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                        Text { text: panel.providerData ? panel.providerData.tokens : "0"; color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(16); font.weight: Font.DemiBold }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(72)
                    radius: panel.colors.cornerRadiusSmall
                    color: panel.colors.inputBackground
                    border.width: 1
                    border.color: panel.colors.border
                    objectName: "providerEnabledCard"
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: panel.hostWindow.dp(10)
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: panel.hostWindow.dp(3)
                            Text { text: qsTr("Provider enabled"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                            Text { text: panel.providerData && panel.providerData.isEnabled ? qsTr("Available") : qsTr("Disabled"); color: panel.providerData && panel.providerData.isEnabled ? panel.colors.success : panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(11); font.weight: Font.DemiBold }
                        }
                        Basic.Switch {
                            id: providerSwitch
                            objectName: "providerEnabledSwitch"
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            implicitWidth: panel.hostWindow.dp(38)
                            implicitHeight: panel.hostWindow.dp(24)
                            padding: 0
                            spacing: 0
                            enabled: panel.providerData !== null
                            checked: panel.providerData ? panel.providerData.isEnabled : false
                            onToggled: panel.providerEnabledChangeRequested(panel.providerData.providerId, checked)
                            indicator: Rectangle {
                                objectName: "providerEnabledIndicator"
                                x: providerSwitch.leftPadding
                                y: providerSwitch.topPadding + (providerSwitch.availableHeight - height) / 2
                                implicitWidth: panel.hostWindow.dp(38)
                                implicitHeight: panel.hostWindow.dp(20)
                                radius: height / 2
                                color: providerSwitch.checked ? panel.colors.accent : panel.colors.buttonBackground
                                border.width: 1
                                border.color: providerSwitch.checked ? panel.colors.accent : panel.colors.borderStrong
                                Rectangle {
                                    width: panel.hostWindow.dp(14)
                                    height: width
                                    radius: width / 2
                                    x: providerSwitch.checked ? parent.width - width - panel.hostWindow.dp(3) : panel.hostWindow.dp(3)
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: panel.colors.textOnAccent
                                }
                            }
                            contentItem: Item { }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(2)
                    Text { Layout.fillWidth: true; text: qsTr("Models"); color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(14); font.weight: Font.DemiBold }
                    Text { Layout.fillWidth: true; text: qsTr("%1 configured for this provider").arg(panel.visibleModelCount); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9) }
                }

                ProviderActionButton {
                    text: qsTr("From catalog")
                    iconText: "+"
                    visible: panel.hasBuiltInCatalog
                    enabled: panel.providerData !== null
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.addCatalogModelsRequested(
                        panel.providerData.providerId,
                        panel.providerData.kind,
                        panel.providerData.name)
                }

                ProviderActionButton {
                    text: panel.hasBuiltInCatalog ? qsTr("Custom model") : qsTr("Add model")
                    iconText: panel.hasBuiltInCatalog ? "" : "+"
                    enabled: panel.providerData !== null
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    onClicked: panel.addCustomModelRequested(panel.providerData.providerId)
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: modelsColumn.implicitHeight + panel.hostWindow.dp(2)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.inputBackground
                border.width: 1
                border.color: panel.colors.border
                clip: true

                ColumnLayout {
                    id: modelsColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: panel.hostWindow.dp(34)
                        Layout.leftMargin: panel.hostWindow.dp(12)
                        Layout.rightMargin: panel.hostWindow.dp(12)
                        spacing: panel.hostWindow.dp(8)
                        Text { Layout.fillWidth: true; text: qsTr("Model"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                        Text { Layout.preferredWidth: panel.hostWindow.dp(66); text: qsTr("Context"); color: panel.colors.textMuted; horizontalAlignment: Text.AlignHCenter; font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                        Text { Layout.preferredWidth: panel.hostWindow.dp(130); text: qsTr("$/1M input / output"); color: panel.colors.textMuted; horizontalAlignment: Text.AlignRight; font.pixelSize: panel.hostWindow.dp(9); font.weight: Font.DemiBold }
                        Item { Layout.preferredWidth: panel.hostWindow.dp(106) }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: panel.colors.separator }

                    Repeater {
                        model: panel.modelsModel

                        delegate: RowLayout {
                            id: modelRow

                            required property var modelData
                            required property string modelId
                            required property string providerId
                            required property string name
                            required property string contextLabel
                            required property string inputPrice
                            required property string outputPrice
                            required property var inputPriceMicros
                            required property var outputPriceMicros
                            required property bool longContextEnabled
                            required property bool isEnabled
                            required property bool isDefault
                            property bool deleteConfirming: false

                            readonly property bool belongsToProvider: panel.providerData !== null && providerId === panel.providerData.providerId

                            visible: belongsToProvider
                            Layout.fillWidth: true
                            Layout.preferredHeight: belongsToProvider ? panel.hostWindow.dp(52) : 0
                            Layout.leftMargin: panel.hostWindow.dp(12)
                            Layout.rightMargin: panel.hostWindow.dp(12)
                            spacing: panel.hostWindow.dp(8)

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: panel.hostWindow.dp(6)
                                    Text { Layout.fillWidth: true; text: modelRow.name; color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(10); font.weight: Font.Medium; elide: Text.ElideRight }
                                    // Data-driven: shown for any model whose Backend record has a long-context tier.
                                    Rectangle {
                                        objectName: "longContextTierChip"
                                        visible: modelRow.longContextEnabled
                                        implicitWidth: tierChipLabel.implicitWidth + panel.hostWindow.dp(12)
                                        implicitHeight: tierChipLabel.implicitHeight + panel.hostWindow.dp(4)
                                        radius: height / 2
                                        color: panel.colors.accentSoft
                                        Text {
                                            id: tierChipLabel
                                            anchors.centerIn: parent
                                            text: qsTr("Long-context tier")
                                            color: panel.colors.accent
                                            font.pixelSize: panel.hostWindow.dp(8)
                                            font.weight: Font.Medium
                                        }
                                    }
                                }
                                Text { Layout.fillWidth: true; text: modelRow.modelId; color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8); elide: Text.ElideMiddle }
                            }

                            Text { Layout.preferredWidth: panel.hostWindow.dp(66); text: modelRow.contextLabel; color: panel.colors.textSecondary; horizontalAlignment: Text.AlignHCenter; font.pixelSize: panel.hostWindow.dp(9) }
                            Text { Layout.preferredWidth: panel.hostWindow.dp(130); text: modelRow.inputPrice + " / " + modelRow.outputPrice; color: Number(modelRow.inputPriceMicros) > 0 || Number(modelRow.outputPriceMicros) > 0 ? panel.colors.textSecondary : panel.colors.warning; horizontalAlignment: Text.AlignRight; font.pixelSize: panel.hostWindow.dp(9) }

                            RowLayout {
                                Layout.preferredWidth: panel.hostWindow.dp(128)
                                spacing: panel.hostWindow.dp(7)

                                Text {
                                    text: "✎"
                                    color: panel.colors.accent
                                    font.pixelSize: panel.hostWindow.dp(12)
                                    Accessible.name: qsTr("Edit model")
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -panel.hostWindow.dp(4)
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: panel.modelEditRequested(modelRow.modelData)
                                    }
                                }

                                Text {
                                    text: "$"
                                    color: panel.colors.accent
                                    font.pixelSize: panel.hostWindow.dp(13)
                                    font.weight: Font.DemiBold
                                    Accessible.name: qsTr("Edit model pricing")
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -panel.hostWindow.dp(4)
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: panel.modelPricingRequested(modelRow.modelData)
                                    }
                                }

                                Text {
                                    text: modelRow.isDefault ? "★" : "☆"
                                    color: modelRow.isDefault ? panel.colors.warning : panel.colors.textMuted
                                    font.pixelSize: panel.hostWindow.dp(15)
                                    Accessible.name: modelRow.isDefault ? qsTr("Default model") : qsTr("Set as default model")
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: panel.defaultModelRequested(modelRow.modelId)
                                    }
                                }

                                Text {
                                    text: modelRow.isEnabled ? "●" : "○"
                                    color: modelRow.isEnabled ? panel.colors.success : panel.colors.textMuted
                                    font.pixelSize: panel.hostWindow.dp(13)
                                    Accessible.name: modelRow.isEnabled ? qsTr("Disable model") : qsTr("Enable model")
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: panel.modelEnabledChanged(modelRow.modelId, !modelRow.isEnabled)
                                    }
                                }

                                Text {
                                    text: modelRow.deleteConfirming ? "?" : "×"
                                    color: panel.colors.error
                                    font.pixelSize: panel.hostWindow.dp(15)
                                    font.weight: modelRow.deleteConfirming ? Font.Bold : Font.Normal
                                    // Two-click confirm (click again within 3s) rather than a
                                    // modal dialog, matching DraftsDialog's delete pattern -
                                    // deletion is backend-rejected outright when the model is
                                    // in use (execution history/profiles/drafts/chats) or is
                                    // the provider's current default, so this only ever
                                    // removes a genuinely unused model.
                                    Accessible.name: modelRow.deleteConfirming
                                        ? qsTr("Confirm remove model") : qsTr("Remove model")
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -panel.hostWindow.dp(4)
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (modelRow.deleteConfirming) {
                                                panel.removeModelRequested(modelRow.modelId)
                                                modelRow.deleteConfirming = false
                                            } else {
                                                modelRow.deleteConfirming = true
                                                modelDeleteResetTimer.restart()
                                            }
                                        }
                                    }
                                    Timer {
                                        id: modelDeleteResetTimer
                                        interval: 3000
                                        onTriggered: modelRow.deleteConfirming = false
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.preferredHeight: panel.visibleModelCount === 0 ? panel.hostWindow.dp(54) : 0
                        visible: panel.visibleModelCount === 0
                        text: qsTr("No models configured")
                        color: panel.colors.textMuted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: panel.hostWindow.dp(10)
                    }
                }
            }
        }
    }
}
