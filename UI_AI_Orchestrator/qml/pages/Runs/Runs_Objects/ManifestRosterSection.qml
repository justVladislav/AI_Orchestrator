pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: panel

    required property var hostWindow
    required property var colors
    required property var controller

    objectName: "rosterSection"
    Layout.fillWidth: true
    visible: panel.controller !== null && panel.controller.hasManifest
             && panel.controller.routingMode === "roster"
    spacing: panel.hostWindow.dp(3)

    Text {
        Layout.fillWidth: true
        text: qsTranslate("ManifestPanel", "Available workers")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(11)
        font.weight: Font.DemiBold
    }
    Repeater {
        model: panel.controller ? panel.controller.routingMembers : null
        delegate: ColumnLayout {
            id: rosterMemberRow
            objectName: "rosterMemberRow"
            required property var modelData
            Layout.fillWidth: true
            spacing: panel.hostWindow.dp(1)

            RowLayout {
                Layout.fillWidth: true
                spacing: panel.hostWindow.dp(8)

                Text {
                    Layout.fillWidth: true
                    text: rosterMemberRow.modelData.memberId + " · "
                          + rosterMemberRow.modelData.role + " · "
                          + rosterMemberRow.modelData.label
                    color: rosterMemberRow.modelData.lead
                           ? panel.colors.accent : panel.colors.textPrimary
                    elide: Text.ElideRight
                    font.pixelSize: panel.hostWindow.dp(9)
                }
                Text {
                    objectName: "rosterParticipationText"
                    text: panel.participationText(rosterMemberRow.modelData)
                    color: rosterMemberRow.modelData.participation === "assigned"
                           ? panel.colors.success : panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(9)
                    font.weight: Font.DemiBold
                }
            }
            Text {
                objectName: "rosterUsageText"
                Layout.fillWidth: true
                visible: panel.controller !== null && panel.controller.usageAvailable
                text: panel.usageSummary(rosterMemberRow.modelData.usage)
                color: panel.colors.textMuted
                elide: Text.ElideRight
                font.pixelSize: panel.hostWindow.dp(8)
            }
        }
    }
    Text {
        objectName: "rosterUsageTotalText"
        Layout.fillWidth: true
        visible: panel.controller !== null && panel.controller.usageAvailable
        text: qsTranslate("ManifestPanel", "Run total: %1").arg(
                  panel.controller ? panel.usageSummary(panel.controller.usageRunTotal) : "")
        color: panel.colors.textMuted
        elide: Text.ElideRight
        font.pixelSize: panel.hostWindow.dp(8)
        font.weight: Font.DemiBold
    }

    function participationText(member) {
        if (member.participation === "lead") return qsTranslate("ManifestPanel", "Lead · plans")
        if (member.participation === "assigned")
            return qsTranslate("ManifestPanel", "%n task(s)", "", member.assignedTaskCount)
        if (member.participation === "unused") return qsTranslate("ManifestPanel", "Unused")
        if (member.participation === "not_assignable")
            return qsTranslate("ManifestPanel", "Not assignable (no workspace access)")
        return member.participation
    }

    function usageSummary(usage) {
        // The Backend sends no usage map before a Manifest is selected or when usage is
        // unavailable (these bindings are still evaluated while the lines are hidden).
        if (!usage || usage.priceState === undefined) return ""
        if (usage.priceState === "none") return qsTranslate("ManifestPanel", "No executions")
        let cost
        if (usage.priceState === "known") cost = usage.costText
        else if (usage.priceState === "partial")
            cost = qsTranslate("ManifestPanel", "at least %1 (some price unset for %n execution(s))", "",
                        Number(usage.unknownPriceExecutions)).arg(usage.costText)
        else cost = qsTranslate("ManifestPanel", "price unknown")
        let text = qsTranslate("ManifestPanel", "%n execution(s)", "", Number(usage.executions))
                   + " · " + qsTranslate("ManifestPanel", "%1 in / %2 out").arg(usage.inputTokens).arg(usage.outputTokens)
                   + " · " + cost
        if (usage.failed > 0) {
            text += " · " + qsTranslate("ManifestPanel", "%1 failed").arg(usage.failed)
            if (usage.interrupted > 0) text += " (" + qsTranslate("ManifestPanel", "%1 interrupted").arg(usage.interrupted) + ")"
        }
        if (usage.inFlight > 0) text += " · " + qsTranslate("ManifestPanel", "%1 running").arg(usage.inFlight)
        return text
    }
}
