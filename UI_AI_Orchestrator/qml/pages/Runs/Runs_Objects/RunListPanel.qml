pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var hostWindow
    required property var colors
    required property var runsModel
    // "All projects" or the selected project's name - RunsPage.qml resolves
    // this from its own project filter combo, so the history header below
    // always names whatever scope filteredRuns is actually restricted to.
    required property string projectFilterLabel
    property string selectedRunId: ""
    property bool historyCollapsed: false
    // Owned by RunsPage: this panel is instantiated only after a calendar
    // date is selected and never changes the browsing mode by itself.
    property string selectedDateKey: ""

    signal runSelected(string runId)
    signal calendarRequested()

    radius: colors.cornerRadiusMedium
    color: colors.surfaceAlternative
    border.width: 1
    border.color: colors.border
    clip: true
    implicitHeight: mainColumn.implicitHeight + panel.hostWindow.dp(24)

    // The reference instant for sorting/grouping a run: when it finished, or
    // failing that when it started, or failing that when it was created -
    // the same fallback chain RunsPage.qml already uses to compute the
    // "updatedAt" label shown on each card, so a run always lands on the
    // calendar day matching the date printed on its own card.
    function referenceDate(run) {
        return run.finishedAt || run.startedAt || run.createdAt || null
    }

    // "yyyy-MM-dd", locale-independent (explicit format characters ignore
    // locale field ordering) - the shared key both this panel's grouping and
    // RunCalendarView's cell keys are built from, so a click always matches.
    function dateKey(d) {
        return d ? Qt.formatDate(d, "yyyy-MM-dd") : ""
    }

    function formatSelectedDate(key) {
        if (key.length === 0) return ""
        const parts = key.split("-")
        return Qt.formatDate(new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2])), "MMM d, yyyy")
    }

    readonly property var activeRuns: {
        const out = []
        for (let i = 0; i < panel.runsModel.length; ++i) {
            const r = panel.runsModel[i]
            if (r.status === "prepared" || r.status === "queued"
                    || r.status === "running" || r.status === "cancelling")
                out.push(r)
        }
        out.sort(function (a, b) {
            const da = panel.referenceDate(a), db = panel.referenceDate(b)
            return (db ? db.getTime() : 0) - (da ? da.getTime() : 0)
        })
        return out
    }
    // Newest first: the previous implementation left this in whatever order
    // the backend returned (oldest created_at first), so the runs anyone
    // actually wants to look at were buried at the bottom of a long list.
    readonly property var historyRuns: {
        const out = []
        for (let i = 0; i < panel.runsModel.length; ++i) {
            const r = panel.runsModel[i]
            if (r.status !== "prepared" && r.status !== "queued"
                    && r.status !== "running" && r.status !== "cancelling")
                out.push(r)
        }
        out.sort(function (a, b) {
            const da = panel.referenceDate(a), db = panel.referenceDate(b)
            return (db ? db.getTime() : 0) - (da ? da.getTime() : 0)
        })
        return out
    }

    // historyRuns bucketed by calendar day - both RunCalendarView's per-cell
    // title previews and the filtered card list below read from this.
    readonly property var historyByDay: {
        const map = {}
        for (let i = 0; i < panel.historyRuns.length; ++i) {
            const run = panel.historyRuns[i]
            const key = panel.dateKey(panel.referenceDate(run))
            if (key.length === 0) continue
            if (!map[key]) map[key] = []
            map[key].push(run)
        }
        return map
    }

    readonly property var selectedDayRuns: panel.historyByDay[panel.selectedDateKey] || []

    function statusColor(status) {
        if (status === "prepared")   return colors.warning
        if (status === "running")    return colors.information
        if (status === "completed")  return colors.success
        if (status === "failed")     return colors.error
        if (status === "queued")     return colors.warning
        if (status === "cancelling") return colors.warning
        return colors.textMuted
    }

    // Shared by both Repeaters below (active runs, and the selected day's
    // history) so the card markup only exists once.
    Component {
        id: runCardDelegate
        Basic.AbstractButton {
            id: runItem
            required property var modelData
            Layout.fillWidth: true
            Layout.preferredHeight: panel.hostWindow.dp(100)
            hoverEnabled: true
            Accessible.name: qsTr("Open run %1").arg(runItem.modelData.title)
            onClicked: panel.runSelected(runItem.modelData.runId)

            background: Rectangle {
                radius: panel.colors.cornerRadiusSmall
                color: runItem.modelData.runId === panel.selectedRunId
                       ? panel.colors.accentSoft
                       : runItem.hovered ? panel.colors.buttonHover : panel.colors.surface
                border.width: runItem.modelData.runId === panel.selectedRunId ? 1 : 0
                border.color: panel.colors.accent
            }

            contentItem: ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: panel.hostWindow.dp(12)
                anchors.rightMargin: panel.hostWindow.dp(12)
                anchors.topMargin: panel.hostWindow.dp(10)
                anchors.bottomMargin: panel.hostWindow.dp(10)
                spacing: panel.hostWindow.dp(5)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: panel.hostWindow.dp(7)
                    Rectangle {
                        Layout.preferredWidth: panel.hostWindow.dp(7)
                        Layout.preferredHeight: panel.hostWindow.dp(7)
                        radius: width / 2
                        color: panel.statusColor(runItem.modelData.status)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: runItem.modelData.title
                        color: panel.colors.textPrimary
                        font.pixelSize: panel.hostWindow.dp(10.5)
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Text {
                        text: runItem.modelData.statusLabel
                        color: panel.statusColor(runItem.modelData.status)
                        font.pixelSize: panel.hostWindow.dp(8.5)
                        font.weight: Font.DemiBold
                    }
                    Rectangle {
                        visible: runItem.modelData.manualHandoffResolution !== "not_applicable"
                        Layout.preferredWidth: resolutionText.implicitWidth
                                               + panel.hostWindow.dp(10)
                        Layout.preferredHeight: panel.hostWindow.dp(18)
                        radius: height / 2
                        color: runItem.modelData.manualHandoffResolution
                               === "resolved_manually_applied"
                               ? panel.colors.successSoft : panel.colors.warningSoft
                        border.width: 1
                        border.color: runItem.modelData.manualHandoffResolution
                                      === "resolved_manually_applied"
                                      ? panel.colors.success : panel.colors.warning
                        Text {
                            id: resolutionText
                            anchors.centerIn: parent
                            text: runItem.modelData.manualHandoffResolutionLabel
                            color: runItem.modelData.manualHandoffResolution
                                   === "resolved_manually_applied"
                                   ? panel.colors.success : panel.colors.warning
                            font.pixelSize: panel.hostWindow.dp(7.5)
                            font.weight: Font.DemiBold
                        }
                    }
                    // AG-17.7C-3: this Run was created by converting a legacy roster Run (the
                    // Backend's marker, worded by RunsPage); a record that cannot be read is
                    // flagged as a fault rather than hidden.
                    Rectangle {
                        objectName: "legacyConversionChip"
                        visible: String(runItem.modelData.legacyConversion || "") !== ""
                        Layout.preferredWidth: conversionChipText.implicitWidth
                                               + panel.hostWindow.dp(10)
                        Layout.preferredHeight: panel.hostWindow.dp(18)
                        radius: height / 2
                        color: runItem.modelData.legacyConversion === "unreadable"
                               ? panel.colors.warningSoft : panel.colors.accentSoft
                        border.width: 1
                        border.color: runItem.modelData.legacyConversion === "unreadable"
                                      ? panel.colors.warning : panel.colors.accent
                        Text {
                            id: conversionChipText
                            anchors.centerIn: parent
                            text: runItem.modelData.legacyConversionLabel
                            textFormat: Text.PlainText
                            color: runItem.modelData.legacyConversion === "unreadable"
                                   ? panel.colors.warning : panel.colors.accent
                            font.pixelSize: panel.hostWindow.dp(7.5)
                            font.weight: Font.DemiBold
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: runItem.modelData.projectName + " · " + runItem.modelData.provider + " / " + runItem.modelData.model
                    color: panel.colors.textSecondary
                    font.pixelSize: panel.hostWindow.dp(8.5)
                    elide: Text.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: runItem.modelData.runId
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8)
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: runItem.modelData.updatedAt
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8)
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: panel.hostWindow.dp(4)
                    radius: height / 2
                    color: panel.colors.buttonBackground
                    Rectangle {
                        width: parent.width * runItem.modelData.progress
                        height: parent.height
                        radius: height / 2
                        color: panel.statusColor(runItem.modelData.status)
                    }
                }
            }
        }
    }

    ColumnLayout {
        id: mainColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: panel.hostWindow.dp(12)
        spacing: panel.hostWindow.dp(6)

        // ─── Empty state ──────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: panel.hostWindow.dp(40)
            Layout.bottomMargin: panel.hostWindow.dp(40)
            Layout.alignment: Qt.AlignHCenter
            spacing: panel.hostWindow.dp(6)
            visible: panel.activeRuns.length === 0 && panel.selectedDayRuns.length === 0

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "⌕"
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(28)
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: panel.selectedDateKey.length > 0
                      ? qsTr("No runs on this date")
                      : qsTr("No runs found")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(12)
                font.weight: Font.Medium
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: panel.selectedDateKey.length > 0
                      ? qsTr("Start the first run for this project or choose another date.")
                      : qsTr("Change the search or filters.")
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(9)
            }
        }

        // ─── Active runs header ───────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            visible: panel.activeRuns.length > 0
            spacing: panel.hostWindow.dp(6)

            Rectangle {
                Layout.preferredWidth: panel.hostWindow.dp(7)
                Layout.preferredHeight: panel.hostWindow.dp(7)
                radius: Layout.preferredWidth / 2
                color: panel.colors.information
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Active / needs action")
                color: panel.colors.textPrimary
                font.pixelSize: panel.hostWindow.dp(12)
                font.weight: Font.DemiBold
            }
            Text {
                text: qsTr("%1").arg(panel.activeRuns.length)
                color: panel.colors.information
                font.pixelSize: panel.hostWindow.dp(9)
                font.weight: Font.DemiBold
            }
        }

        // ─── Active run cards ─────────────────────────────────────────────
        Repeater {
            model: panel.activeRuns
            delegate: runCardDelegate
        }

        // ─── Separator between sections ───────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            Layout.topMargin: panel.hostWindow.dp(2)
            Layout.bottomMargin: panel.hostWindow.dp(2)
            color: panel.colors.separator
            visible: panel.activeRuns.length > 0 && panel.selectedDayRuns.length > 0
        }

        // ─── Calendar: pick a date to browse ────────────────────────────────
        // ─── Selected-day header ─────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            visible: panel.selectedDateKey.length > 0
            spacing: panel.hostWindow.dp(6)

            // Returning clears the page-owned date key, which swaps this
            // entire two-pane layer back to the full-size calendar.
            Basic.AbstractButton {
                id: calendarBackButton
                Layout.preferredHeight: panel.hostWindow.dp(24)
                Layout.preferredWidth: calendarBackLabel.implicitWidth + panel.hostWindow.dp(14)
                hoverEnabled: true
                Accessible.name: qsTr("Back to calendar")
                onClicked: panel.calendarRequested()

                background: Rectangle {
                    radius: panel.colors.cornerRadiusSmall
                    color: calendarBackButton.hovered
                           ? panel.colors.buttonHover : "transparent"
                }
                contentItem: Text {
                    id: calendarBackLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: "\u2039 " + qsTr("Calendar")
                    color: panel.colors.accent
                    font.pixelSize: panel.hostWindow.dp(9)
                    font.weight: Font.DemiBold
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 0

                // Project and date have separate lines so a long project name
                // can elide without ever hiding the selected calendar date.
                Text {
                    Layout.fillWidth: true
                    text: panel.projectFilterLabel
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(8)
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: panel.formatSelectedDate(panel.selectedDateKey)
                    color: panel.colors.textPrimary
                    font.pixelSize: panel.hostWindow.dp(12)
                    font.weight: Font.DemiBold
                }
            }
            Text {
                text: qsTr("%1 runs").arg(panel.selectedDayRuns.length)
                color: panel.colors.textMuted
                font.pixelSize: panel.hostWindow.dp(9)
            }
            Basic.AbstractButton {
                id: collapseBtn
                Layout.preferredWidth: panel.hostWindow.dp(22)
                Layout.preferredHeight: panel.hostWindow.dp(22)
                hoverEnabled: true
                onClicked: panel.historyCollapsed = !panel.historyCollapsed
                Accessible.name: panel.historyCollapsed ? qsTr("Expand run history") : qsTr("Collapse run history")

                background: Rectangle {
                    radius: panel.colors.cornerRadiusSmall
                    color: collapseBtn.hovered ? panel.colors.buttonHover : "transparent"
                }
                contentItem: Text {
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: panel.historyCollapsed ? "▶" : "▼"
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(8)
                }
            }
        }

        // ─── Selected day's run cards ────────────────────────────────────
        Repeater {
            model: panel.historyCollapsed ? [] : panel.selectedDayRuns
            delegate: runCardDelegate
        }

        // Bottom spacer so last card doesn't clip the border
        Item { Layout.preferredHeight: panel.hostWindow.dp(2) }
    }
}
