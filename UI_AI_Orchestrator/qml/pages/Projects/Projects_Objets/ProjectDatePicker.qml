pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Visual date (+ optional time) picker field.
// selectedDate: valid JS Date | null | undefined | new Date(NaN)  →  no date = not set
Rectangle {
    id: root

    required property var hostWindow
    required property var colors
    property var    selectedDate:    undefined
    property bool   showTime:        false
    property string placeholderText: qsTr("Not set")

    implicitHeight: root.hostWindow.dp(36)
    implicitWidth:  root.hostWindow.dp(160)
    radius:         root.colors.cornerRadiusSmall
    color:          root.colors.inputBackground
    border.width:   calPopup.opened ? 1 : 0
    border.color:   root.colors.inputBorderFocused

    // ── private navigation / time state ───────────────────────────────────
    property int navYear:  (new Date()).getFullYear()
    property int navMonth: (new Date()).getMonth() + 1   // 1-12
    property int _hour:    0
    property int _minute:  0

    // ── helpers ────────────────────────────────────────────────────────────
    function _effectiveDate() {
        const d = root.selectedDate
        if (d === null || d === undefined) return null
        const t = new Date(d)
        return isNaN(t.getTime()) ? null : t
    }
    function _hasDate() { return root._effectiveDate() !== null }

    function _displayText() {
        const d = root._effectiveDate()
        if (!d) return ""
        let s = Qt.formatDate(d, "d MMM yyyy")
        if (root.showTime)
            s += " " + String(d.getHours()).padStart(2, "0")
                  + ":" + String(d.getMinutes()).padStart(2, "0")
        return s
    }

    function _daysInMonth(y, m) { return new Date(y, m, 0).getDate() }
    function _firstDow(y, m)    { return new Date(y, m - 1, 1).getDay() }

    function _monthName(m) {
        return [qsTr("January"),   qsTr("February"), qsTr("March"),
                qsTr("April"),     qsTr("May"),       qsTr("June"),
                qsTr("July"),      qsTr("August"),    qsTr("September"),
                qsTr("October"),   qsTr("November"),  qsTr("December")][m - 1] || ""
    }

    function _openPopup() {
        const d = root._effectiveDate()
        if (d) {
            root.navYear  = d.getFullYear()
            root.navMonth = d.getMonth() + 1
            root._hour    = d.getHours()
            root._minute  = d.getMinutes()
        } else {
            const t = new Date()
            root.navYear  = t.getFullYear()
            root.navMonth = t.getMonth() + 1
            root._hour    = 0
            root._minute  = 0
        }
        calPopup.open()
    }

    function _selectDay(day) {
        root.selectedDate = new Date(root.navYear, root.navMonth - 1, day, root._hour, root._minute, 0, 0)
        if (!root.showTime) calPopup.close()
    }

    function _setToday() {
        const t = new Date()
        root.navYear  = t.getFullYear()
        root.navMonth = t.getMonth() + 1
        if (root.showTime) { root._hour = t.getHours(); root._minute = t.getMinutes() }
        root._selectDay(t.getDate())
    }

    function _clearDate() { root.selectedDate = new Date(NaN); calPopup.close() }

    function _prevMonth() {
        if (root.navMonth === 1) { root.navYear -= 1; root.navMonth = 12 }
        else root.navMonth -= 1
    }
    function _nextMonth() {
        if (root.navMonth === 12) { root.navYear += 1; root.navMonth = 1 }
        else root.navMonth += 1
    }

    function _adjHour(delta) {
        root._hour = (root._hour + 24 + delta) % 24
        root._syncTime()
    }
    function _adjMinute(delta) {
        root._minute = (root._minute + 60 + delta) % 60
        root._syncTime()
    }
    function _syncTime() {
        const sd = root._effectiveDate()
        if (!sd) return
        root.selectedDate = new Date(sd.getFullYear(), sd.getMonth(), sd.getDate(),
                                     root._hour, root._minute, 0, 0)
    }

    // ── field (click to open popup) ────────────────────────────────────────
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root._openPopup()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin:  root.hostWindow.dp(10)
        anchors.rightMargin: root.hostWindow.dp(6)
        spacing: root.hostWindow.dp(5)

        Text {
            text: "📅"    // 📅
            font.pixelSize: root.hostWindow.dp(11)
            color: root.colors.textMuted
        }

        Text {
            Layout.fillWidth: true
            text:  root._hasDate() ? root._displayText() : root.placeholderText
            color: root._hasDate() ? root.colors.textPrimary : root.colors.textMuted
            font.pixelSize: root.hostWindow.dp(11)
            elide: Text.ElideRight
        }

        // ×  clear button
        Rectangle {
            visible:        root._hasDate()
            implicitWidth:  root.hostWindow.dp(18)
            implicitHeight: root.hostWindow.dp(18)
            radius: root.hostWindow.dp(9)
            color:   xHov.containsMouse ? root.colors.buttonHover : "transparent"

            Text {
                anchors.centerIn: parent
                text: "×"
                color: root.colors.textMuted
                font.pixelSize: root.hostWindow.dp(14)
            }

            MouseArea {
                id: xHov
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root._clearDate()
            }
        }
    }

    // ── calendar popup ─────────────────────────────────────────────────────
    Popup {
        id: calPopup
        parent: root
        x: 0
        y: root.height + root.hostWindow.dp(4)
        width: root.hostWindow.dp(278)
        modal: false
        focus: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: root.hostWindow.dp(10)

        background: Rectangle {
            radius: root.colors.cornerRadiusMedium
            color:  root.colors.popupBackground
            border.width: 1
            border.color: root.colors.border
        }

        contentItem: ColumnLayout {
            spacing: root.hostWindow.dp(5)

            // ── month navigation ──────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: 0

                Rectangle {
                    implicitWidth: root.hostWindow.dp(28); implicitHeight: root.hostWindow.dp(28)
                    radius: root.colors.cornerRadiusSmall
                    color: prevHov.containsMouse ? root.colors.buttonHover : "transparent"
                    Text { anchors.centerIn: parent; text: "‹"; color: root.colors.textPrimary; font.pixelSize: root.hostWindow.dp(18) }
                    MouseArea { id: prevHov; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root._prevMonth() }
                }

                Text {
                    Layout.fillWidth: true
                    text: root._monthName(root.navMonth) + " " + root.navYear
                    color: root.colors.textPrimary
                    font.pixelSize: root.hostWindow.dp(12)
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                }

                Rectangle {
                    implicitWidth: root.hostWindow.dp(28); implicitHeight: root.hostWindow.dp(28)
                    radius: root.colors.cornerRadiusSmall
                    color: nextHov.containsMouse ? root.colors.buttonHover : "transparent"
                    Text { anchors.centerIn: parent; text: "›"; color: root.colors.textPrimary; font.pixelSize: root.hostWindow.dp(18) }
                    MouseArea { id: nextHov; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root._nextMonth() }
                }
            }

            // ── day-of-week header ────────────────────────────────────────
            Grid {
                Layout.alignment: Qt.AlignHCenter
                columns: 7
                spacing: root.hostWindow.dp(1)

                Repeater {
                    model: [qsTr("Su"), qsTr("Mo"), qsTr("Tu"),
                            qsTr("We"), qsTr("Th"), qsTr("Fr"), qsTr("Sa")]
                    delegate: Text {
                        required property string modelData
                        width:  root.hostWindow.dp(36)
                        height: root.hostWindow.dp(22)
                        text:   modelData
                        color:  root.colors.textMuted
                        font.pixelSize: root.hostWindow.dp(9)
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment:   Text.AlignVCenter
                    }
                }
            }

            // ── day grid (6 rows × 7 cols = 42 cells) ────────────────────
            Grid {
                Layout.alignment: Qt.AlignHCenter
                columns: 7
                spacing: root.hostWindow.dp(1)

                Repeater {
                    model: 42
                    delegate: Rectangle {
                        id: dayCell
                        required property int index

                        readonly property int cellDay: {
                            const fd = root._firstDow(root.navYear, root.navMonth)
                            const d  = dayCell.index - fd + 1
                            const mx = root._daysInMonth(root.navYear, root.navMonth)
                            return (d >= 1 && d <= mx) ? d : 0
                        }

                        readonly property bool isToday: {
                            if (dayCell.cellDay === 0) return false
                            const t = new Date()
                            return t.getFullYear() === root.navYear
                                && t.getMonth() + 1 === root.navMonth
                                && t.getDate() === dayCell.cellDay
                        }

                        readonly property bool isSelected: {
                            if (dayCell.cellDay === 0) return false
                            const sd = root._effectiveDate()
                            if (!sd) return false
                            return sd.getFullYear() === root.navYear
                                && sd.getMonth() + 1 === root.navMonth
                                && sd.getDate() === dayCell.cellDay
                        }

                        width:  root.hostWindow.dp(36)
                        height: root.hostWindow.dp(30)
                        radius: root.colors.cornerRadiusSmall
                        color:  dayCell.isSelected                          ? root.colors.accent
                              : dcArea.containsMouse && dayCell.cellDay > 0 ? root.colors.buttonHover
                              :                                                "transparent"

                        Text {
                            anchors.centerIn: parent
                            text:  dayCell.cellDay > 0 ? String(dayCell.cellDay) : ""
                            color: dayCell.isSelected ? root.colors.textOnAccent
                                 : dayCell.isToday    ? root.colors.accent
                                 :                      root.colors.textPrimary
                            font.pixelSize: root.hostWindow.dp(11)
                            font.weight:    dayCell.isSelected || dayCell.isToday ? Font.DemiBold : Font.Normal
                        }

                        MouseArea {
                            id: dcArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled:     dayCell.cellDay > 0
                            cursorShape: dayCell.cellDay > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked:   root._selectDay(dayCell.cellDay)
                        }
                    }
                }
            }

            // ── time row (showTime only) ──────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                visible: root.showTime
                spacing: root.hostWindow.dp(4)

                Text {
                    text: qsTr("Time")
                    color: root.colors.textMuted
                    font.pixelSize: root.hostWindow.dp(10)
                }
                Item { Layout.fillWidth: true }

                // hour −
                Rectangle {
                    implicitWidth: root.hostWindow.dp(24); implicitHeight: root.hostWindow.dp(24)
                    radius: root.colors.cornerRadiusSmall
                    color: hdA.containsMouse ? root.colors.buttonHover : root.colors.inputBackground
                    border.width: 1; border.color: root.colors.border
                    Text { anchors.centerIn: parent; text: "−"; color: root.colors.textPrimary; font.pixelSize: root.hostWindow.dp(13) }
                    MouseArea { id: hdA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root._adjHour(-1) }
                }
                Text {
                    text: String(root._hour).padStart(2, "0")
                    color: root.colors.textPrimary
                    font.pixelSize: root.hostWindow.dp(12); font.weight: Font.DemiBold
                    Layout.preferredWidth: root.hostWindow.dp(20); horizontalAlignment: Text.AlignHCenter
                }
                // hour +
                Rectangle {
                    implicitWidth: root.hostWindow.dp(24); implicitHeight: root.hostWindow.dp(24)
                    radius: root.colors.cornerRadiusSmall
                    color: hiA.containsMouse ? root.colors.buttonHover : root.colors.inputBackground
                    border.width: 1; border.color: root.colors.border
                    Text { anchors.centerIn: parent; text: "+"; color: root.colors.textPrimary; font.pixelSize: root.hostWindow.dp(12) }
                    MouseArea { id: hiA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root._adjHour(1) }
                }

                Text {
                    text: ":"
                    color: root.colors.textMuted
                    font.pixelSize: root.hostWindow.dp(13); font.weight: Font.DemiBold
                    Layout.preferredWidth: root.hostWindow.dp(8); horizontalAlignment: Text.AlignHCenter
                }

                // minute −
                Rectangle {
                    implicitWidth: root.hostWindow.dp(24); implicitHeight: root.hostWindow.dp(24)
                    radius: root.colors.cornerRadiusSmall
                    color: mdA.containsMouse ? root.colors.buttonHover : root.colors.inputBackground
                    border.width: 1; border.color: root.colors.border
                    Text { anchors.centerIn: parent; text: "−"; color: root.colors.textPrimary; font.pixelSize: root.hostWindow.dp(13) }
                    MouseArea { id: mdA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root._adjMinute(-5) }
                }
                Text {
                    text: String(root._minute).padStart(2, "0")
                    color: root.colors.textPrimary
                    font.pixelSize: root.hostWindow.dp(12); font.weight: Font.DemiBold
                    Layout.preferredWidth: root.hostWindow.dp(20); horizontalAlignment: Text.AlignHCenter
                }
                // minute +
                Rectangle {
                    implicitWidth: root.hostWindow.dp(24); implicitHeight: root.hostWindow.dp(24)
                    radius: root.colors.cornerRadiusSmall
                    color: miA.containsMouse ? root.colors.buttonHover : root.colors.inputBackground
                    border.width: 1; border.color: root.colors.border
                    Text { anchors.centerIn: parent; text: "+"; color: root.colors.textPrimary; font.pixelSize: root.hostWindow.dp(12) }
                    MouseArea { id: miA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root._adjMinute(5) }
                }
            }

            // ── separator ─────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: root.colors.separator
            }

            // ── actions ───────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: root.hostWindow.dp(6)

                ProjectActionButton {
                    text: qsTr("Today")
                    hostWindow: root.hostWindow
                    colors: root.colors
                    onClicked: root._setToday()
                }

                Item { Layout.fillWidth: true }

                ProjectActionButton {
                    text: qsTr("Clear")
                    hostWindow: root.hostWindow
                    colors: root.colors
                    onClicked: root._clearDate()
                }

                ProjectActionButton {
                    visible: root.showTime
                    text: qsTr("Done")
                    primary: true
                    enabled: root._hasDate()
                    hostWindow: root.hostWindow
                    colors: root.colors
                    onClicked: calPopup.close()
                }
            }
        }
    }
}
