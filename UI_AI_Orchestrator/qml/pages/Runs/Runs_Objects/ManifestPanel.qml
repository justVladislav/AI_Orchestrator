pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Displays the frozen execution manifest (if any) for the currently
// selected run: its tasks with role/dependencies/owned paths/capability and
// each task's live execution status, error, proposal link, and evidence
// summary.
//
// Nothing this component renders is an absolute filesystem path or a secret -
// every string comes from ManifestController, which only ever exposes the
// UI-safe ExecutionManifestReadModelService projection.
ColumnLayout {
    id: panel
    objectName: "manifestPanel"

    required property var hostWindow
    required property var colors
    required property var controller
    // backend.runs (RunController). Its runUpdated(runId) signal fires on
    // every RunEvent for this run, including the per-task StageProgress
    // ticks ManifestWorkflowRunner now publishes as each task finishes -
    // see AgentSessionPanel.qml's own doc comment on the same mechanism.
    required property var runsController
    property string runId: ""

    onRunIdChanged: {
        if (panel.controller) panel.controller.selectRun(panel.runId)
    }

    Connections {
        target: panel.runsController
        function onRunUpdated(runId) {
            if (runId === panel.runId && panel.controller) panel.controller.refresh()
        }
    }

    Connections {
        target: panel.controller
        function onVerificationContinuationFinished(childRunId, status) {
            if (panel.runsController) panel.runsController.refresh()
            if (childRunId.length > 0 && status.length > 0 && panel.runsController)
                panel.runsController.selectRun(childRunId)
        }
    }

    // AG-18.4A hardening: calls only ManifestController::retryAfterSetup(),
    // which itself calls only
    // VerificationBootstrapRetryService::createLinkedRetry() - this panel
    // never invents or launches the setup work itself. This is a retry
    // AFTER the user has already configured or created a usable profile or
    // preset manually or externally - never a "create a scoped setup Run"
    // action (that launcher does not exist yet - AG-18.4C). Navigates to
    // the new, freshly baseline-bound Run on success, exactly like
    // RunController::retryRun already does for an ordinary retry.
    function retryAfterSetup() {
        if (!panel.controller) return
        var newRunId = panel.controller.retryAfterSetup()
        if (newRunId.length > 0 && panel.runsController) {
            panel.runsController.selectRun(newRunId)
            panel.runsController.refresh()
        }
    }

    spacing: hostWindow.dp(10)
    // AG-18.4A: this panel is now also visible for a Run that never froze a
    // Manifest at all - a terminal ActionRequired disposition, or a Run
    // still in its bootstrap lifecycle - as long as it has a
    // verification-environment projection worth showing.
    visible: controller !== null && (controller.hasManifest
             || controller.hasVerificationEnvironment
             || controller.hasVerificationContinuation)



    // AG-18.4A: verification-environment/bootstrap-lifecycle projection -
    // entirely Backend-computed (ManifestController::populateVerificationEnvironment
    // from ExecutionManifestReadModelService::verificationEnvironmentViewForRun).
    // This QML file never reclassifies state, parses diagnostic text, or
    // decides action eligibility - it only renders the strings/booleans the
    // Backend already sent.
    ManifestEnvironmentSection {
        hostWindow: panel.hostWindow
        colors: panel.colors
        controller: panel.controller
        onRetryAfterSetupRequested: panel.retryAfterSetup()
    }

    ColumnLayout {
        id: continuationSection
        objectName: "verificationContinuationSection"
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.hasVerificationContinuation
        spacing: panel.hostWindow.dp(5)

        // Keep bindings total even while selection changes clear the
        // controller map between two refreshes. This is display-only
        // fallback data; eligibility still comes exclusively from the
        // Backend-owned canContinue field.
        readonly property var detail: panel.controller
            && panel.controller.hasVerificationContinuation
            ? panel.controller.verificationContinuation
            : ({ selectedRole: "", sourceRunId: "", childRunId: "",
                 sourceStatus: "", childStatus: "", plannerSkipped: false,
                 canContinue: false, canStartRepair: false,
                 blockCode: "", blockMessage: "",
                 criteria: [], findings: [] })
        // MW-P5.6B: a Prepared child (childRunId set, childStatus exactly
        // "prepared") means the free step already ran and a durable child
        // Run is waiting - only then may the paid repair step even be
        // offered. Backend-owned fields only; QML never derives this from
        // anything else.
        readonly property bool hasPreparedChild:
            continuationSection.detail.childRunId.length > 0
            && continuationSection.detail.childStatus === "prepared"
        readonly property string preparedChildId:
            continuationSection.hasPreparedChild
            ? continuationSection.detail.childRunId : ""
        // The paid continuation is single-use. A terminal continuation child
        // may expose the separate manual-handoff panel, but it must never show
        // the source Run's old Prepare action as a disabled look-alike.
        readonly property bool selectedContinuationChild:
            continuationSection.detail.selectedRole === "child"
        // Local UI-only confirmation gate for the paid step - never sent to
        // or read from the Backend. Resets whenever the prepared child
        // changes (a fresh selection/refresh) so a stale confirmation can
        // never carry over to a different child.
        property bool confirmingPaidRepair: false
        onPreparedChildIdChanged: confirmingPaidRepair = false

        Text {
            Layout.fillWidth: true
            text: qsTr("Failed-verification continuation")
            color: panel.colors.textPrimary
            font.pixelSize: panel.hostWindow.dp(11)
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "verificationContinuationLineage"
            Layout.fillWidth: true
            text: continuationSection.detail.selectedRole === "child"
                  ? qsTr("Child of %1 · child status: %2")
                        .arg(continuationSection.detail.sourceRunId)
                        .arg(continuationSection.detail.childStatus)
                  : (continuationSection.detail.childRunId.length > 0
                     ? qsTr("Source Run · linked child: %1 (%2)")
                           .arg(continuationSection.detail.childRunId)
                           .arg(continuationSection.detail.childStatus)
                     : qsTr("Source Run · no continuation child yet"))
            color: panel.colors.textSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }
        Text {
            objectName: "verificationContinuationPlannerSkipped"
            Layout.fillWidth: true
            visible: continuationSection.detail.plannerSkipped === true
            text: qsTr("Planner skipped: the child uses the frozen source checkpoint and exact proposal hashes.")
            color: panel.colors.information
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }
        Text {
            objectName: "verificationContinuationBlockReason"
            Layout.fillWidth: true
            visible: continuationSection.detail.canContinue !== true
                     && continuationSection.detail.blockMessage.length > 0
            text: continuationSection.detail.blockCode + ": "
                  + continuationSection.detail.blockMessage
            color: panel.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }
        // QML-P0: a truthful, additional projection layered on top of the
        // child's own terminal Failed status - never a substitute for it,
        // never Apply-eligible. Shown prominently (not buried below a long
        // diagnostic list, the exact cc48001 lesson) because it names real,
        // safety-clean work worth a human's review.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(4)
            visible: continuationSection.detail.contentOutcome === "needs_manual_action"
            Text {
                objectName: "verificationContinuationNeedsManualAction"
                Layout.fillWidth: true
                text: qsTr("Needs manual action: proposal safety and all other configured checks "
                           + "passed; only bounded, advisory QML lint findings remain. This does "
                           + "not authorize Apply - review the findings below.")
                color: panel.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
            Repeater {
                model: continuationSection.detail.remainingFindings || []
                delegate: Text {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.relativePath + ":" + modelData.line + ":" + modelData.column
                          + " [" + modelData.tag + "] "
                          + (modelData.message.length > 0 ? modelData.message : modelData.diagnosticCode)
                    color: panel.colors.textSecondary
                    wrapMode: Text.WordWrap
                    font.pixelSize: panel.hostWindow.dp(8)
                }
            }
        }
        // MW-P5.6B: Prepare (free - real qmllint/cmake reconstruction and
        // attribution, never a provider call) and Start paid repair (a real
        // provider call) are two separate, explicitly confirmed actions -
        // never one click that silently does both. A single shared "Cancel
        // continuation" button covers whichever of the two is in flight.
        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)
            visible: !continuationSection.hasPreparedChild
                     && !continuationSection.selectedContinuationChild
            RunActionButton {
                objectName: "prepareVerificationContinuationButton"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: panel.controller && panel.controller.verificationContinuationBusy
                      ? qsTr("Preparing…") : qsTr("Prepare paid verification repair")
                primary: true
                enabled: panel.controller
                         && continuationSection.detail.canContinue === true
                         && !panel.controller.verificationContinuationBusy
                onClicked: panel.controller.prepareVerificationContinuation()
            }
            Item { Layout.fillWidth: true }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(6)
            visible: continuationSection.hasPreparedChild
            Text {
                Layout.fillWidth: true
                visible: !continuationSection.confirmingPaidRepair
                text: continuationSection.detail.canStartRepair === true
                      ? qsTr("Continuation prepared: %1. Starting repair makes a real, paid "
                             + "provider call and materializes new proposed changes. It does not "
                             + "apply them to the live workspace.")
                            .arg(continuationSection.detail.childRunId)
                      : qsTr("Continuation child %1 is retained, but paid repair is blocked by "
                             + "the Backend reason shown above.")
                            .arg(continuationSection.detail.childRunId)
                color: panel.colors.information
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(9)
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)
                visible: !continuationSection.confirmingPaidRepair
                         && continuationSection.detail.canStartRepair === true
                RunActionButton {
                    objectName: "startVerificationContinuationRepairButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: qsTr("Start paid repair")
                    primary: true
                    enabled: panel.controller && !panel.controller.verificationContinuationBusy
                    onClicked: continuationSection.confirmingPaidRepair = true
                }
                Item { Layout.fillWidth: true }
            }
            Text {
                Layout.fillWidth: true
                visible: continuationSection.confirmingPaidRepair
                text: qsTr("This will spend real provider budget and create new repair "
                           + "proposals. Applying them to the live workspace remains a separate "
                           + "reviewed action. Confirm?")
                color: panel.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(9)
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)
                visible: continuationSection.confirmingPaidRepair
                RunActionButton {
                    objectName: "confirmPaidRepairButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: panel.controller && panel.controller.verificationContinuationBusy
                          ? qsTr("Repairing…") : qsTr("Confirm paid repair")
                    primary: true
                    enabled: panel.controller && !panel.controller.verificationContinuationBusy
                    onClicked: {
                        continuationSection.confirmingPaidRepair = false
                        panel.controller.startVerificationContinuationRepair()
                    }
                }
                RunActionButton {
                    objectName: "cancelPaidRepairConfirmButton"
                    hostWindow: panel.hostWindow
                    colors: panel.colors
                    text: qsTr("Cancel")
                    visible: !panel.controller || !panel.controller.verificationContinuationBusy
                    enabled: visible
                    onClicked: continuationSection.confirmingPaidRepair = false
                }
                Item { Layout.fillWidth: true }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(8)
            visible: panel.controller && panel.controller.verificationContinuationBusy
            RunActionButton {
                objectName: "cancelVerificationContinuationButton"
                hostWindow: panel.hostWindow
                colors: panel.colors
                text: qsTr("Cancel continuation")
                visible: panel.controller && panel.controller.verificationContinuationBusy
                enabled: visible
                onClicked: panel.controller.cancelVerificationContinuation()
            }
            Item { Layout.fillWidth: true }
        }
        // P5.6A: prepare()/repair() failures (baseline_drift, attribution
        // codes, etc.) were already being computed and stored on
        // controller.errorText, but this panel never displayed it - a real
        // failure looked identical to "nothing happened" at all. Every other
        // action-bearing panel in this app (AgentSessionPanel, dialogs, ...)
        // surfaces controller.errorText the same way.
        Text {
            objectName: "verificationContinuationErrorText"
            Layout.fillWidth: true
            visible: panel.controller && panel.controller.errorText.length > 0
            text: panel.controller ? panel.controller.errorText : ""
            color: panel.colors.error
            wrapMode: Text.WordWrap
            font.pixelSize: panel.hostWindow.dp(9)
        }
        // Keep the next operator action above the potentially long diagnostic
        // list. A successful Prepare can add many findings at once; placing
        // those findings first pushed "Start paid repair" below the fold and
        // made a completed Prepare look as if every action had disappeared.
        Text {
            Layout.fillWidth: true
            visible: (continuationSection.detail.criteria || []).length > 0
                     || (continuationSection.detail.findings || []).length > 0
            text: qsTr("Verification details")
            color: panel.colors.textSecondary
            font.pixelSize: panel.hostWindow.dp(9)
            font.weight: Font.DemiBold
        }
        Repeater {
            model: continuationSection.detail.criteria || []
            delegate: Text {
                required property var modelData
                Layout.fillWidth: true
                text: modelData.requirementId + ": " + modelData.sourceStatus
                      + (modelData.finalStatus.length > 0
                         ? qsTr(" → %1").arg(modelData.finalStatus) : "")
                color: modelData.finalStatus === "passed"
                       ? panel.colors.success
                       : (modelData.sourceStatus === "failed"
                          || modelData.finalStatus === "failed")
                         ? panel.colors.error : panel.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(9)
            }
        }
        Repeater {
            model: continuationSection.detail.findings || []
            delegate: Text {
                required property var modelData
                Layout.fillWidth: true
                text: modelData.diagnosticCode + " · " + modelData.attributionKind
                      + (modelData.relativePath.length > 0 ? " · " + modelData.relativePath : "")
                      + (modelData.ownerTaskId.length > 0 ? " · " + modelData.ownerTaskId : "")
                color: modelData.attributionKind === "pre_existing_verification_debt"
                       ? panel.colors.warning : panel.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8)
            }
        }
    }

    // QML-lint advisory-severity policy (2026-09-28): entirely
    // Backend-computed (ManifestController::hasVerificationAdvisory/
    // verificationAdvisoryPassed/hasAdvisories/advisoryCount/advisories,
    // sourced from ExecutionManifestReadModelService::viewForRun's
    // verificationAdvisory projection). This QML file never classifies a
    // finding's tag and never derives Apply eligibility from advisory
    // presence - a real hard failure still renders through the ordinary
    // task/verification failure paths below, entirely unaffected by this
    // section. Shown only once the check actually passed with advisories
    // remaining (verificationAdvisoryPassed) - a mixed advisory+hard result
    // stays Failed and is never labeled "passed" here.
    ColumnLayout {
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.hasVerificationAdvisory
            && panel.controller.verificationAdvisoryPassed && panel.controller.hasAdvisories
        spacing: panel.hostWindow.dp(2)

        Text {
            Layout.fillWidth: true
            text: qsTr("QML lint passed with %1 advisories").arg(panel.controller ? panel.controller.advisoryCount : 0)
            color: panel.colors.warning
            font.pixelSize: panel.hostWindow.dp(9)
            font.weight: Font.DemiBold
        }
        Repeater {
            model: panel.controller ? panel.controller.advisories : null
            delegate: Text {
                id: advisoryText
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: panel.hostWindow.dp(6)
                text: qsTr("· %1:%2:%3 [%4] %5")
                    .arg(advisoryText.modelData.relativePath)
                    .arg(advisoryText.modelData.line)
                    .arg(advisoryText.modelData.column)
                    .arg(advisoryText.modelData.tag)
                    .arg(advisoryText.modelData.message)
                color: panel.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8)
            }
        }
    }

    ManifestRosterSection {
        hostWindow: panel.hostWindow
        colors: panel.colors
        controller: panel.controller
    }

    // AG-17.7B: incomplete attribution. Execution rows the usage reconstruction could not
    // attribute to any member, or a partition that does not add up, mean the position
    // allocation drifted from the persisted rows. They are counted in the Run total, never
    // hidden, and this warning makes the gap visible instead of silently under-reporting a
    // member. It speaks ONLY about attribution: other diagnostics (an unreadable ledger, a
    // saturated counter) never claim rows were unattributed - they appear in the block below.
    Text {
        id: usageIncompleteText
        objectName: "usageIncompleteText"
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.hasManifest
                 && panel.controller.usageAvailable
                 && (panel.controller.usageUnattributedCount > 0
                     || !panel.controller.usagePartitionExact)
        text: panel.controller && panel.controller.usageUnattributedCount > 0
              ? qsTr("Usage attribution is incomplete: %n execution(s) could not be attributed to a member",
                     "", panel.controller.usageUnattributedCount)
              : qsTr("Usage attribution is inexact: the member totals do not add up to the Run total")
        color: panel.colors.warning
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(8)
        font.weight: Font.DemiBold
    }

    // AG-17.7B: every usage diagnostic the Backend produced, shown whenever there is one -
    // whether or not usage could be attributed (a ledger read failure has no attribution at
    // all) and whether or not the partition is exact (a saturated counter leaves it exact).
    ColumnLayout {
        id: usageDiagnosticsSection
        objectName: "usageDiagnosticsSection"
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.hasManifest
                 && panel.controller.usageDiagnostics.length > 0
        spacing: panel.hostWindow.dp(2)

        Text {
            Layout.fillWidth: true
            text: qsTr("Usage diagnostics")
            color: panel.colors.textMuted
            font.pixelSize: panel.hostWindow.dp(8)
            font.weight: Font.DemiBold
        }
        Repeater {
            model: panel.controller ? panel.controller.usageDiagnostics : null
            delegate: Text {
                id: usageDiagnosticText
                required property var modelData
                Layout.fillWidth: true
                text: usageDiagnosticText.modelData
                color: panel.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8)
            }
        }
    }

    Text {
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.hasManifest
        text: qsTr("Manifest tasks")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(11)
        font.weight: Font.DemiBold
    }

    // AG-15.3: Run-level file-level dependency edges (Backend-derived, never
    // Planner-declared - see RunWorkspaceScopeService's own doc comment).
    ColumnLayout {
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.hasWorkspaceScope
            && panel.controller.scopeDependencyEdges.length > 0
        spacing: panel.hostWindow.dp(2)

        Text {
            Layout.fillWidth: true
            text: qsTr("Workspace scope dependencies")
            color: panel.colors.textMuted
            font.pixelSize: panel.hostWindow.dp(8)
            font.weight: Font.DemiBold
        }
        Repeater {
            model: panel.controller ? panel.controller.scopeDependencyEdges : null
            delegate: Text {
                id: edgeText
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: panel.hostWindow.dp(6)
                text: qsTr("· %1 requires %2 (owned by %3)")
                    .arg(edgeText.modelData.dependentTaskId)
                    .arg(edgeText.modelData.relativePath)
                    .arg(edgeText.modelData.providerTaskId)
                color: panel.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: panel.hostWindow.dp(8)
            }
        }
    }

    Repeater {
        id: tasksRepeater
        model: panel.controller ? panel.controller.tasks : null
        delegate: ManifestTaskCard {
            hostWindow: panel.hostWindow
            colors: panel.colors
            controller: panel.controller
        }
    }
    Text {
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.hasManifest && tasksRepeater.count === 0
        text: qsTr("No manifest tasks recorded.")
        color: panel.colors.textMuted
        font.pixelSize: panel.hostWindow.dp(9)
    }
}
