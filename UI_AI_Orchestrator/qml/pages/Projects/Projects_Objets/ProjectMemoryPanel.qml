pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// AG-11 remaining product slice: viewer + provenance for the selected
// project's memory segments, compaction history, and the two operator
// actions (compact, destructive clear). Every list/action goes through
// backend.projectMemory (ProjectMemoryController) - this panel never touches
// SQLite or the Application Layer directly, and never shows raw prompt/
// content beyond the already-bounded segment summary the Backend returns.
ColumnLayout {
    id: panel
    objectName: "projectMemoryPanel"

    required property var hostWindow
    required property var colors
    required property var backend
    required property string projectId

    // Confirmation lives at the page level (mirroring ProjectsPage.qml's own
    // removeDialog) so this component's root can stay a plain ColumnLayout:
    // a Dialog declared as a direct child of a Layout is layout-managed,
    // which conflicts with the anchors/width a Dialog needs for its own
    // positioning (qmllint Quick.layout-positioning).
    signal clearMemoryRequested()

    property var segmentsSnapshot: []
    property var historySnapshot: []
    property var providerChoicesSnapshot: []
    property int selectedChoiceIndex: -1
    property string errorText: ""
    // The Flickable that scrolls this panel (ProjectDetailsPanel's). With it the segment list creates delegates
    // only for the part that is on screen plus a margin; without it every delegate is created, as a Repeater would.
    property Item scroller: null

    readonly property bool compactingThisProject:
        panel.backend.projectMemory.compacting
        && panel.backend.projectMemory.compactingProjectId === panel.projectId

    Layout.fillWidth: true
    spacing: hostWindow.dp(8)

    function refreshSegments() {
        panel.segmentsSnapshot = panel.projectId.length > 0
                ? panel.backend.projectMemory.segments(panel.projectId) : []
    }

    function refreshHistory() {
        panel.historySnapshot = panel.projectId.length > 0
                ? panel.backend.projectMemory.compactionHistory(panel.projectId) : []
    }

    function refreshProviderChoices() {
        const choices = panel.backend.projectMemory.providerChoices()
        panel.providerChoicesSnapshot = choices
        if (panel.selectedChoiceIndex >= choices.length)
            panel.selectedChoiceIndex = choices.length > 0 ? 0 : -1
    }

    function refreshAll() {
        panel.refreshSegments()
        panel.refreshHistory()
        panel.refreshProviderChoices()
    }

    function requestCompact() {
        if (panel.selectedChoiceIndex < 0 || panel.selectedChoiceIndex >= panel.providerChoicesSnapshot.length)
            return
        const choice = panel.providerChoicesSnapshot[panel.selectedChoiceIndex]
        panel.backend.projectMemory.compact(panel.projectId, choice.providerId, choice.modelId)
    }

    onProjectIdChanged: panel.refreshAll()
    // Defer the first synchronous backend read until the object tree has
    // fully constructed, matching ChatsPage's Qt.callLater(initializeBackendData).
    Component.onCompleted: Qt.callLater(panel.refreshAll)

    Connections {
        target: panel.backend.projectMemory
        function onSegmentsChanged(changedProjectId) {
            if (changedProjectId === panel.projectId)
                panel.refreshSegments()
        }
        function onCompactionHistoryChanged(changedProjectId) {
            if (changedProjectId === panel.projectId)
                panel.refreshHistory()
        }
        function onErrorOccurred(code, message) {
            panel.errorText = message
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: panel.hostWindow.dp(44)
        radius: panel.colors.cornerRadiusSmall
        color: panel.colors.inputBackground
        border.width: 1
        border.color: panel.colors.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: panel.hostWindow.dp(11)
            anchors.rightMargin: panel.hostWindow.dp(7)
            spacing: panel.hostWindow.dp(8)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text { text: qsTr("Project memory"); color: panel.colors.textPrimary; font.pixelSize: panel.hostWindow.dp(13); font.weight: Font.DemiBold }
                Text { text: qsTr("Deterministic run summaries and optional compaction"); color: panel.colors.textMuted; font.pixelSize: panel.hostWindow.dp(8.5) }
            }
            Rectangle {
                Layout.preferredWidth: segmentCount.implicitWidth + panel.hostWindow.dp(12)
                Layout.preferredHeight: panel.hostWindow.dp(22)
                radius: height / 2
                color: panel.colors.surface
                border.width: 1
                border.color: panel.colors.border
                Text { id: segmentCount; anchors.centerIn: parent; text: String(panel.segmentsSnapshot.length); color: panel.colors.textSecondary; font.pixelSize: panel.hostWindow.dp(9.5); font.weight: Font.DemiBold }
            }
            ProjectActionButton {
                objectName: "clearProjectMemoryButton"
                text: qsTr("Clear")
                danger: true
                enabled: panel.segmentsSnapshot.length > 0
                hostWindow: panel.hostWindow
                colors: panel.colors
                onClicked: panel.clearMemoryRequested()
            }
        }
    }

    Text {
        Layout.fillWidth: true
        visible: panel.errorText.length > 0
        text: panel.errorText
        color: panel.colors.error
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(10)
    }

    Text {
        Layout.fillWidth: true
        visible: panel.segmentsSnapshot.length === 0
        text: qsTr("No memory segments yet. Segments are written automatically after a Run completes.")
        color: panel.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(10)
    }

    // Memory segments: a virtualized list. It sits inside the panel's scrolling parent (ProjectDetailsPanel's
    // Flickable), so a ListView of its own would create every delegate. Instead the host reserves the full height of
    // the list in the parent's content, and a ListView only as tall as the parent's viewport slides along inside it,
    // showing the matching part of the list (non-interactive; the parent does the scrolling).
    Item {
        id: segmentsHost
        Layout.fillWidth: true
        Layout.preferredHeight: segmentsList.contentHeight
        visible: panel.segmentsSnapshot.length > 0

        function syncWindow() {
            const flick = panel.scroller
            if (!flick || !flick.contentItem) {
                segmentsList.height = segmentsList.contentHeight
                segmentsList.y = 0
                return
            }
            const sliceHeight = Math.min(segmentsList.contentHeight, flick.height)
            const hostTop = segmentsHost.mapToItem(flick.contentItem, 0, 0).y
            const offset = Math.max(0, Math.min(flick.contentY - hostTop,
                                                segmentsList.contentHeight - sliceHeight))
            segmentsList.height = sliceHeight
            segmentsList.y = offset
            segmentsList.contentY = segmentsList.originY + offset
        }

        onYChanged: syncWindow()
        onHeightChanged: syncWindow()
        onVisibleChanged: if (visible) syncWindow()
        Component.onCompleted: Qt.callLater(syncWindow)

        Connections {
            target: panel.scroller
            function onContentYChanged() { segmentsHost.syncWindow() }
            function onHeightChanged() { segmentsHost.syncWindow() }
        }
        Connections {
            target: panel
            function onYChanged() { segmentsHost.syncWindow() }
            function onScrollerChanged() { segmentsHost.syncWindow() }
        }
        Connections {
            target: panel.parent
            function onYChanged() { segmentsHost.syncWindow() }
        }

        ListView {
            id: segmentsList
            objectName: "projectMemorySegmentsList"
            width: segmentsHost.width
            interactive: false
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            spacing: panel.spacing
            cacheBuffer: panel.hostWindow.dp(300)
            model: panel.segmentsSnapshot
            onContentHeightChanged: segmentsHost.syncWindow()
            onHeightChanged: segmentsHost.syncWindow()

            delegate: Rectangle {
                id: segmentRow
                objectName: "projectMemorySegmentRow"
                required property var modelData
                width: ListView.view ? ListView.view.width : 0
                height: segmentContent.implicitHeight + panel.hostWindow.dp(16)
                radius: panel.colors.cornerRadiusSmall
                color: panel.colors.inputBackground
                border.width: 1
                border.color: panel.colors.border

                ColumnLayout {
                    id: segmentContent
                    anchors.fill: parent
                    anchors.margins: panel.hostWindow.dp(9)
                    spacing: panel.hostWindow.dp(3)

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: segmentRow.modelData.sourceRunId.length > 0
                                  ? qsTr("From run %1").arg(segmentRow.modelData.sourceRunId.substring(0, 8))
                                  : qsTr("Consolidated segment")
                            color: panel.colors.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: panel.hostWindow.dp(10.5)
                            font.weight: Font.Medium
                        }
                        Text {
                            text: segmentRow.modelData.createdAt
                            color: panel.colors.textMuted
                            font.pixelSize: panel.hostWindow.dp(9)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: segmentRow.modelData.summary
                        color: panel.colors.textSecondary
                        wrapMode: Text.WordWrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        font.pixelSize: panel.hostWindow.dp(9.5)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Source ~%1 tok · summary ~%2 tok")
                            .arg(segmentRow.modelData.sourceTokenCount)
                            .arg(segmentRow.modelData.summaryTokenCount)
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(8.5)
                    }
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        Layout.topMargin: panel.hostWindow.dp(4)
        text: qsTr("Compact memory")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(13)
        font.weight: Font.DemiBold
    }

    Text {
        Layout.fillWidth: true
        text: panel.segmentsSnapshot.length < 2
              ? qsTr("At least two segments are required before memory can be compacted.")
              : panel.providerChoicesSnapshot.length === 0
                ? qsTr("Enable a provider and model in Providers first.")
                : qsTr("Summarizes every current segment into one consolidated segment using the selected provider/model. This makes a real provider call.")
        color: panel.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: panel.hostWindow.dp(9.5)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: panel.hostWindow.dp(8)

        ComboBox {
            id: providerModelCombo
            objectName: "memoryProviderModelCombo"
            Layout.fillWidth: true
            Layout.preferredHeight: panel.hostWindow.dp(34)
            enabled: panel.providerChoicesSnapshot.length > 0 && !panel.compactingThisProject
            model: panel.providerChoicesSnapshot
            textRole: "modelName"
            font.pixelSize: panel.hostWindow.dp(11)
            currentIndex: panel.selectedChoiceIndex
            onActivated: selectedIndex => panel.selectedChoiceIndex = selectedIndex
        }

        ProjectActionButton {
            objectName: "compactProjectMemoryButton"
            text: panel.compactingThisProject ? qsTr("Compacting…") : qsTr("Compact")
            primary: true
            enabled: !panel.compactingThisProject
                     && panel.segmentsSnapshot.length >= 2
                     && panel.selectedChoiceIndex >= 0
                     && panel.selectedChoiceIndex < panel.providerChoicesSnapshot.length
            hostWindow: panel.hostWindow
            colors: panel.colors
            onClicked: panel.requestCompact()
        }

        ProjectActionButton {
            objectName: "stopCompactProjectMemoryButton"
            text: qsTr("Stop")
            visible: panel.compactingThisProject
            hostWindow: panel.hostWindow
            colors: panel.colors
            onClicked: panel.backend.projectMemory.stopCompaction()
        }
    }

    Text {
        Layout.fillWidth: true
        Layout.topMargin: panel.hostWindow.dp(4)
        visible: panel.historySnapshot.length > 0
        text: qsTr("Compaction history")
        color: panel.colors.textPrimary
        font.pixelSize: panel.hostWindow.dp(13)
        font.weight: Font.DemiBold
    }

    Repeater {
        model: panel.historySnapshot
        delegate: Rectangle {
            id: historyRow
            required property var modelData
            Layout.fillWidth: true
            Layout.preferredHeight: historyContent.implicitHeight + panel.hostWindow.dp(16)
            radius: panel.colors.cornerRadiusSmall
            color: panel.colors.inputBackground
            border.width: 1
            border.color: historyRow.modelData.status === "failed" || historyRow.modelData.status === "cancelled"
                          ? panel.colors.warning : panel.colors.border

            ColumnLayout {
                id: historyContent
                anchors.fill: parent
                anchors.margins: panel.hostWindow.dp(9)
                spacing: panel.hostWindow.dp(3)

                RowLayout {
                    Layout.fillWidth: true
                    Rectangle {
                        Layout.preferredWidth: statusLabel.implicitWidth + panel.hostWindow.dp(12)
                        Layout.preferredHeight: panel.hostWindow.dp(19)
                        radius: height / 2
                        color: historyRow.modelData.status === "completed"
                               ? Qt.rgba(0.27, 0.83, 0.60, 0.16)
                               : historyRow.modelData.status === "failed"
                                 ? Qt.rgba(0.79, 0.24, 0.28, 0.16)
                                 : Qt.rgba(0.96, 0.73, 0.30, 0.16)
                        Text {
                            id: statusLabel
                            anchors.centerIn: parent
                            text: historyRow.modelData.status === "completed" ? qsTr("Completed")
                                  : historyRow.modelData.status === "failed" ? qsTr("Failed")
                                  : qsTr("Cancelled")
                            color: historyRow.modelData.status === "completed" ? panel.colors.success
                                   : historyRow.modelData.status === "failed" ? panel.colors.error
                                   : panel.colors.warning
                            font.pixelSize: panel.hostWindow.dp(9)
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: historyRow.modelData.modelId
                        color: panel.colors.textSecondary
                        elide: Text.ElideRight
                        font.pixelSize: panel.hostWindow.dp(9.5)
                    }
                    Text {
                        text: historyRow.modelData.createdAt
                        color: panel.colors.textMuted
                        font.pixelSize: panel.hostWindow.dp(9)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: historyRow.modelData.status !== "completed"
                             && historyRow.modelData.errorMessage.length > 0
                    text: historyRow.modelData.errorMessage
                    color: panel.colors.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: panel.hostWindow.dp(9)
                }
                Text {
                    Layout.fillWidth: true
                    visible: historyRow.modelData.status === "completed"
                    text: qsTr("%1 in / %2 out tok · %3 · from %4 segment(s)")
                        .arg(historyRow.modelData.inputTokens)
                        .arg(historyRow.modelData.outputTokens)
                        .arg(historyRow.modelData.estimatedCost)
                        .arg(historyRow.modelData.sourceSegmentIds.length)
                    color: panel.colors.textMuted
                    font.pixelSize: panel.hostWindow.dp(8.5)
                }
            }
        }
    }
}
