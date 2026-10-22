pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

// Month-grid calendar for browsing run history by date. Each day cell shows
// a couple of that day's run titles as a preview; clicking a cell selects
// it (dateSelected) so the caller can filter its own list to that day.
// Purely a picker - it owns no run-filtering logic itself.
ColumnLayout {
    id: calendar
    required property var hostWindow
    required property var colors
    // Dictionary: "yyyy-MM-dd" -> array of run objects (each needs .title).
    required property var runsByDay
    property string selectedDateKey: ""
    // Full-page mode increases navigation, typography and day-cell height
    // while preserving the same month/data/selection behavior.
    property bool expanded: false

    signal dateSelected(string dateKey)

    property date viewedMonth: new Date(new Date().getFullYear(), new Date().getMonth(), 1)
    property bool initializedFromSelection: false

    // The header exposes month and year independently. Years containing runs
    // are always included, together with a practical window around today.
    readonly property var monthOptions: {
        const options = []
        for (let month = 0; month < 12; ++month) {
            options.push({
                "label": Qt.formatDate(new Date(2024, month, 1), "MMMM"),
                "value": String(month)
            })
        }
        return options
    }
    readonly property var yearOptions: {
        const values = {}
        const currentYear = new Date().getFullYear()
        for (let year = currentYear - 5; year <= currentYear + 5; ++year)
            values[year] = true
        for (const key in calendar.runsByDay) {
            const runYear = Number(key.slice(0, 4))
            if (Number.isFinite(runYear))
                values[runYear] = true
        }
        values[calendar.viewedMonth.getFullYear()] = true
        const years = Object.keys(values).map(Number)
        years.sort(function(a, b) { return a - b })
        return years.map(function(year) {
            return { "label": String(year), "value": String(year) }
        })
    }

    // Jump to the selected date's month the first time a selection arrives
    // (RunListPanel sets one shortly after construction), so the calendar
    // doesn't default to "now" when the most recent run was last month.
    onSelectedDateKeyChanged: {
        if (calendar.initializedFromSelection || calendar.selectedDateKey.length === 0)
            return
        const parts = calendar.selectedDateKey.split("-")
        calendar.viewedMonth = new Date(Number(parts[0]), Number(parts[1]) - 1, 1)
        calendar.initializedFromSelection = true
    }

    function keyFor(year, month, day) {
        return Qt.formatDate(new Date(year, month, day), "yyyy-MM-dd")
    }

    // Weeks (Monday-first) covering the viewed month, padded with null
    // cells for the leading/trailing days of adjacent months.
    readonly property var weeks: {
        const year = calendar.viewedMonth.getFullYear()
        const month = calendar.viewedMonth.getMonth()
        const daysInMonth = new Date(year, month + 1, 0).getDate()
        const firstWeekday = (new Date(year, month, 1).getDay() + 6) % 7
        const today = new Date()
        const todayKey = calendar.keyFor(today.getFullYear(), today.getMonth(), today.getDate())

        const cells = []
        for (let i = 0; i < firstWeekday; ++i) cells.push(null)
        for (let day = 1; day <= daysInMonth; ++day) {
            const key = calendar.keyFor(year, month, day)
            cells.push({ day: day, key: key, runs: calendar.runsByDay[key] || [], isToday: key === todayKey })
        }
        while (cells.length % 7 !== 0) cells.push(null)

        const rows = []
        for (let i = 0; i < cells.length; i += 7) rows.push(cells.slice(i, i + 7))
        return rows
    }

    spacing: calendar.hostWindow.dp(calendar.expanded ? 10 : 4)

    // ─── Month header with prev/next navigation ─────────────────────────
    RowLayout {
        Layout.fillWidth: true
        spacing: calendar.hostWindow.dp(6)

        Basic.AbstractButton {
            id: prevMonthBtn
            Layout.preferredWidth: calendar.hostWindow.dp(calendar.expanded ? 38 : 22)
            Layout.preferredHeight: calendar.hostWindow.dp(calendar.expanded ? 38 : 22)
            hoverEnabled: true
            onClicked: calendar.viewedMonth = new Date(calendar.viewedMonth.getFullYear(), calendar.viewedMonth.getMonth() - 1, 1)
            Accessible.name: qsTr("Previous month")
            background: Rectangle {
                radius: calendar.colors.cornerRadiusSmall
                color: prevMonthBtn.hovered ? calendar.colors.buttonHover : "transparent"
            }
            contentItem: Text {
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "‹"
                color: calendar.colors.textPrimary
                font.pixelSize: calendar.hostWindow.dp(calendar.expanded ? 22 : 13)
            }
        }
        Item { Layout.fillWidth: true }
        RunComboBox {
            Layout.preferredWidth: calendar.hostWindow.dp(calendar.expanded ? 170 : 130)
            Layout.preferredHeight: calendar.hostWindow.dp(calendar.expanded ? 42 : 34)
            hostWindow: calendar.hostWindow
            colors: calendar.colors
            options: calendar.monthOptions
            accessibleLabel: qsTr("Calendar month")
            selectedValue: String(calendar.viewedMonth.getMonth())
            onValueSelected: value => calendar.viewedMonth = new Date(
                                 calendar.viewedMonth.getFullYear(), Number(value), 1)
        }
        RunComboBox {
            Layout.preferredWidth: calendar.hostWindow.dp(calendar.expanded ? 108 : 90)
            Layout.preferredHeight: calendar.hostWindow.dp(calendar.expanded ? 42 : 34)
            hostWindow: calendar.hostWindow
            colors: calendar.colors
            options: calendar.yearOptions
            accessibleLabel: qsTr("Calendar year")
            selectedValue: String(calendar.viewedMonth.getFullYear())
            onValueSelected: value => calendar.viewedMonth = new Date(
                                 Number(value), calendar.viewedMonth.getMonth(), 1)
        }
        Item { Layout.fillWidth: true }
        Basic.AbstractButton {
            id: nextMonthBtn
            Layout.preferredWidth: calendar.hostWindow.dp(calendar.expanded ? 38 : 22)
            Layout.preferredHeight: calendar.hostWindow.dp(calendar.expanded ? 38 : 22)
            hoverEnabled: true
            onClicked: calendar.viewedMonth = new Date(calendar.viewedMonth.getFullYear(), calendar.viewedMonth.getMonth() + 1, 1)
            Accessible.name: qsTr("Next month")
            background: Rectangle {
                radius: calendar.colors.cornerRadiusSmall
                color: nextMonthBtn.hovered ? calendar.colors.buttonHover : "transparent"
            }
            contentItem: Text {
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "›"
                color: calendar.colors.textPrimary
                font.pixelSize: calendar.hostWindow.dp(calendar.expanded ? 22 : 13)
            }
        }
    }

    // ─── Weekday header ───────────────────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        spacing: calendar.hostWindow.dp(2)
        Repeater {
            model: [qsTr("Mon"), qsTr("Tue"), qsTr("Wed"), qsTr("Thu"), qsTr("Fri"), qsTr("Sat"), qsTr("Sun")]
            delegate: Text {
                required property string modelData
                Layout.fillWidth: true
                // A zero content-based preference keeps every weekday column
                // exactly one seventh of the row, regardless of label width.
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                horizontalAlignment: Text.AlignHCenter
                text: modelData
                color: calendar.colors.textMuted
                font.pixelSize: calendar.hostWindow.dp(calendar.expanded ? 12.5 : 8.5)
                font.weight: Font.DemiBold
            }
        }
    }

    // ─── Weeks grid ───────────────────────────────────────────────────
    Repeater {
        model: calendar.weeks
        delegate: RowLayout {
            id: weekRow
            required property var modelData
            Layout.fillWidth: true
            spacing: calendar.hostWindow.dp(2)

            Repeater {
                model: weekRow.modelData
                delegate: Basic.AbstractButton {
                    id: dayCell
                    required property var modelData
                    Layout.fillWidth: true
                    // Ignore title implicit widths: the month is a strict
                    // seven-column grid, not a content-sized collection.
                    Layout.preferredWidth: 1
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: calendar.expanded
                                            ? Math.max(calendar.hostWindow.dp(72),
                                                       (calendar.height - calendar.hostWindow.dp(120))
                                                       / Math.max(calendar.weeks.length, 1))
                                            : calendar.hostWindow.dp(50)
                    enabled: dayCell.modelData !== null
                    hoverEnabled: dayCell.enabled
                    clip: true
                    opacity: dayCell.modelData === null ? 0 : 1
                    Accessible.name: dayCell.modelData
                        ? qsTr("%1, %2 run(s)").arg(dayCell.modelData.key).arg(dayCell.modelData.runs.length)
                        : ""
                    onClicked: calendar.dateSelected(dayCell.modelData.key)

                    background: Rectangle {
                        radius: calendar.colors.cornerRadiusSmall
                        color: dayCell.modelData && dayCell.modelData.key === calendar.selectedDateKey
                               ? calendar.colors.accentSoft
                               : dayCell.hovered ? calendar.colors.buttonHover : "transparent"
                        border.width: dayCell.modelData && dayCell.modelData.isToday ? 1 : 0
                        border.color: calendar.colors.accent
                    }

                    contentItem: ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: calendar.hostWindow.dp(calendar.expanded ? 8 : 3)
                        spacing: calendar.hostWindow.dp(calendar.expanded ? 3 : 1)
                        visible: dayCell.modelData !== null

                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: dayCell.modelData ? dayCell.modelData.day : ""
                            color: dayCell.modelData && dayCell.modelData.key === calendar.selectedDateKey
                                   ? calendar.colors.accent : calendar.colors.textSecondary
                            font.pixelSize: calendar.hostWindow.dp(calendar.expanded ? 14 : 9.5)
                            font.weight: Font.DemiBold
                        }
                        Repeater {
                            model: dayCell.modelData ? dayCell.modelData.runs.slice(0, 2) : []
                            delegate: Text {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: "• " + modelData.title
                                color: calendar.colors.textMuted
                                font.pixelSize: calendar.hostWindow.dp(calendar.expanded ? 9 : 6.5)
                                elide: Text.ElideRight
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: dayCell.modelData && dayCell.modelData.runs.length > 2
                            text: dayCell.modelData && dayCell.modelData.runs.length > 2
                                  ? qsTr("+%1 more").arg(dayCell.modelData.runs.length - 2) : ""
                            color: calendar.colors.textMuted
                            font.pixelSize: calendar.hostWindow.dp(calendar.expanded ? 9 : 6.5)
                        }
                    }
                }
            }
        }
    }
}
