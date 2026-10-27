pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: panel

    required property var hostWindow
    required property var colors
    required property var controller

    signal retryAfterSetupRequested()

    objectName: "verificationEnvironmentSection"
    Layout.fillWidth: true
    visible: panel.controller !== null && panel.controller.hasVerificationEnvironment
    spacing: panel.hostWindow.dp(4)

    RowLayout {
        Layout.fillWidth: true
        Text {
            text: qsTranslate("ManifestPanel", "Verification environment")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(11)
            font.weight: Font.DemiBold
        }
        Text {
            text: panel.controller ? panel.stateLabel(panel.controller.verificationEnvironmentState) : ""
            color: panel.controller ? panel.stateColor(panel.controller.verificationEnvironmentState) : panel.colors.textMuted
            font.pixelSize: panel.hostWindow.dp(9)
            font.weight: Font.DemiBold
        }
        Text {
            visible: panel.controller && panel.controller.verificationEnvironmentIssue !== "none"
            text: panel.controller ? "· " + panel.issueLabel(panel.controller.verificationEnvironmentIssue) : ""
            color: panel.colors.warning
            font.pixelSize: panel.hostWindow.dp(9)
        }
    }
    Text {
        Layout.fillWidth: true
        visible: panel.controller && panel.controller.verificationEnvironmentDiagnostic.length > 0
        text: panel.controller ? panel.controller.verificationEnvironmentDiagnostic : ""
        color: panel.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(8.5)
    }

    // Discovered profile(s): source and frozen values, exactly as the
    // Backend recorded them.
    Repeater {
        model: panel.controller ? panel.controller.verificationProfiles : null
        delegate: ColumnLayout {
            id: profileItem
            required property var modelData
            Layout.fillWidth: true
            Layout.leftMargin: panel.hostWindow.dp(6)
            spacing: panel.hostWindow.dp(1)

            Text {
                Layout.fillWidth: true
                text: qsTranslate("ManifestPanel", "· %1 (%2, %3)")
                    .arg(profileItem.modelData.configurePresetName)
                    .arg(profileItem.modelData.configureSource)
                    .arg(profileItem.modelData.buildPresetsConfirmed
                         ? qsTranslate("ManifestPanel", "build presets confirmed") : qsTranslate("ManifestPanel", "build presets not confirmed"))
                color: panel.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8.5)
            }
            Text {
                Layout.fillWidth: true
                visible: profileItem.modelData.resolvedBinaryDir.length > 0
                text: qsTranslate("ManifestPanel", "  Build dir: %1").arg(profileItem.modelData.resolvedBinaryDir)
                color: panel.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8)
            }
            Repeater {
                model: profileItem.modelData.buildPresets
                delegate: Text {
                    id: buildPresetText
                    required property var modelData
                    Layout.fillWidth: true
                    text: qsTranslate("ManifestPanel", "  %1: %2")
                        .arg(buildPresetText.modelData.name)
                        .arg(buildPresetText.modelData.allowedTargetsConfirmed
                             ? (buildPresetText.modelData.allowedTargets.length > 0
                                ? buildPresetText.modelData.allowedTargets.join(", ")
                                : qsTranslate("ManifestPanel", "no targets"))
                             : qsTranslate("ManifestPanel", "targets not confirmed"))
                    color: panel.colors.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: panel.hostWindow.dp(8)
                }
            }
        }
    }

    // Bootstrap lifecycle (AG-18.3), unchanged here - only projected.
    ColumnLayout {
        Layout.fillWidth: true
        visible: panel.controller && panel.controller.hasBootstrapDecision
        spacing: panel.hostWindow.dp(1)

        Text {
            Layout.fillWidth: true
            text: panel.controller
                ? qsTranslate("ManifestPanel", "Bootstrap: %1 (%2)")
                    .arg(panel.controller.bootstrapDisposition)
                    .arg(panel.controller.bootstrapReasonCode)
                : ""
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(8.5)
        }
        Text {
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.bootstrapMessage.length > 0
            text: panel.controller ? panel.controller.bootstrapMessage : ""
            color: panel.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(8)
        }
        Text {
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.bootstrapSetupPaths.length > 0
            text: panel.controller
                ? qsTranslate("ManifestPanel", "Setup paths: %1").arg(panel.controller.bootstrapSetupPaths.join(", ")) : ""
            color: panel.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(8)
        }
        Text {
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.hasBootstrapTransition
            text: panel.controller
                ? qsTranslate("ManifestPanel", "Candidate: %1 → %2/%3%4")
                    .arg(panel.controller.bootstrapTransitionDisposition)
                    .arg(panel.controller.bootstrapCandidateProfileId)
                    .arg(panel.controller.bootstrapCandidateBuildProfileId)
                    .arg(panel.controller.bootstrapCandidateTargetId.length > 0
                         ? "/" + panel.controller.bootstrapCandidateTargetId : "")
                : ""
            color: panel.colors.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(8)
        }
    }

    // AG-18.4A hardening: both follow-up actions are always shown;
    // eligibility and its explanation are entirely Backend-computed.
    // Neither button here launches any setup work itself - the user
    // must first configure or restore a verification preset. The retry
    // command rechecks file presence at click time before creating any
    // Run, so the button may remain available without a file watcher.
    // Existing-Project configuration from Run Details and AG-18.4C's
    // scoped setup-Run launcher remain separate, unimplemented flows.
    Text {
        Layout.fillWidth: true
        text: qsTranslate("ManifestPanel", "Existing-Project verification setup from Run Details is not available yet. "
                   + "Create or restore a project/user preset externally, then choose "
                   + "\"Retry after setup\". Backend checks that a preset file exists before "
                   + "starting a new baseline-bound Run.")
        color: panel.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(8)
    }
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: panel.hostWindow.dp(4)
        spacing: panel.hostWindow.dp(8)

        RunActionButton {
            objectName: "retryAfterSetupButton"
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("ManifestPanel", "Retry after setup")
            primary: true
            enabled: panel.controller && panel.controller.canRetryAfterSetup
            onClicked: panel.retryAfterSetupRequested()
        }
        RunActionButton {
            objectName: "configureVerificationProfileButton"
            hostWindow: panel.hostWindow
            colors: panel.colors
            text: qsTranslate("ManifestPanel", "Configure verification profile")
            enabled: panel.controller && panel.controller.canConfigureBackendProfile
            onClicked: {}
        }
    }
    Text {
        Layout.fillWidth: true
        visible: panel.controller && !panel.controller.canRetryAfterSetup
            && panel.controller.canRetryAfterSetupReason.length > 0
        text: panel.controller ? panel.controller.canRetryAfterSetupReason : ""
        color: panel.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(8)
    }
    Text {
        Layout.fillWidth: true
        visible: panel.controller && panel.controller.canConfigureBackendProfileReason.length > 0
        text: panel.controller ? panel.controller.canConfigureBackendProfileReason : ""
        color: panel.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(8)
    }

    function stateLabel(state) {
        if (state === "existing_profile") return qsTranslate("ManifestPanel", "Existing profile")
        if (state === "proposed_bootstrap") return qsTranslate("ManifestPanel", "Bootstrap in progress")
        if (state === "setup_required") return qsTranslate("ManifestPanel", "Setup required")
        return state
    }

    function issueLabel(issue) {
        if (issue === "drifted") return qsTranslate("ManifestPanel", "workspace drifted since discovery")
        if (issue === "tool_unavailable") return qsTranslate("ManifestPanel", "CMake tool unavailable")
        return ""
    }

    function stateColor(state) {
        if (state === "existing_profile") return colors.success
        if (state === "proposed_bootstrap") return colors.warning
        return colors.error
    }
}
