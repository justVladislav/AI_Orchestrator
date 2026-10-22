import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Projects_Objets"

Item {
    id: page

    required property var hostWindow
    required property var colors
    // Qt Adapter composition root supplied by PageHost.
    required property var backend

    signal projectOpenRequested(string projectId)
    signal projectCreateRequested(string name, string path, string description)
    signal projectRemoveRequested(string projectId)
    signal orchestrationRequested(string projectId)

    property string selectedProjectId: ""
    property string clearMemoryProjectId: ""
    property string notificationText: ""
    readonly property string activeProjectId: page.backend.workspace.activeProjectId

    // Plain-JS projections of the live Qt Adapter models (backend.projects.model,
    // backend.runs.model). They exist only so the existing list/detail panels can
    // keep consuming simple objects; every field still comes from the adapter and
    // is rebuilt whenever the underlying C++ model resets.
    property var projectsSnapshot: []
    property var runsSnapshot: []
    property var tasksSnapshot: []
    property bool runsSnapshotRebuildScheduled: false

    readonly property var selectedProjectTasks: tasksSnapshot.filter(function(task) {
        return task.projectId === page.selectedProjectId && task.status !== "cancelled"
    })

    readonly property var selectedProject: {
        for (let i = 0; i < projectsSnapshot.length; ++i) {
            if (projectsSnapshot[i].projectId === selectedProjectId)
                return projectsSnapshot[i]
        }
        return null
    }

    function formatDateTime(value) {
        return value ? Qt.formatDateTime(value, "yyyy-MM-dd HH:mm") : "—"
    }

    function runStatusLabel(status) {
        switch (status) {
        case "queued": return qsTr("Queued")
        case "running": return qsTr("Running")
        case "cancelling": return qsTr("Cancelling")
        case "completed": return qsTr("Completed")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        case "interrupted": return qsTr("Interrupted")
        default: return status
        }
    }

    function taskStatusLabel(status) {
        switch (status) {
        case "backlog": return qsTr("Backlog")
        case "planned": return qsTr("Planned")
        case "in_progress": return qsTr("In progress")
        case "blocked": return qsTr("Blocked")
        case "done": return qsTr("Done")
        case "cancelled": return qsTr("Cancelled")
        default: return status
        }
    }

    function rebuildTasksSnapshot() {
        const items = []
        for (let i = 0; i < tasksMirror.count; ++i) {
            const it = tasksMirror.itemAt(i)
            if (!it) continue
            const parts = [page.taskStatusLabel(it.status), it.priority]
            if (it.dueAt) parts.push(qsTr("due %1").arg(Qt.formatDate(it.dueAt, "yyyy-MM-dd")))
            if (it.plannedRunAt) parts.push(qsTr("run %1").arg(Qt.formatDate(it.plannedRunAt, "yyyy-MM-dd")))
            items.push({ taskId: it.taskId, projectId: it.projectId, title: it.title,
                description: it.description, status: it.status,
                statusLabel: page.taskStatusLabel(it.status), priority: it.priority,
                requiresAttention: it.requiresAttention, dueAt: it.dueAt,
                plannedRunAt: it.plannedRunAt, metaLabel: parts.join(" · ") })
        }
        page.tasksSnapshot = items
        page.rebuildProjectsSnapshot()
    }

    function rebuildProjectsSnapshot() {
        const items = []
        for (let i = 0; i < projectsMirror.count; ++i) {
            const it = projectsMirror.itemAt(i)
            if (!it)
                continue
            const projectRuns = page.runsSnapshot.filter(function (run) { return run.projectId === it.projectId })
            const projectTasks = page.tasksSnapshot.filter(function (task) { return task.projectId === it.projectId })
            const lastRun = projectRuns.length > 0 ? projectRuns[projectRuns.length - 1] : null
            items.push({
                projectId: it.projectId,
                name: it.name,
                description: it.description,
                path: it.rootPath,
                status: it.status,
                createdLabel: page.formatDateTime(it.createdAt),
                updatedLabel: page.formatDateTime(it.updatedAt),
                activeTasks: projectTasks.filter(function(task) {
                    return task.status !== "done" && task.status !== "cancelled"
                }).length,
                tasksTotal: projectTasks.length,
                lastActivity: page.formatDateTime(it.updatedAt),
                runsTotal: projectRuns.length,
                runsRunning: projectRuns.filter(function (r) { return r.status === "running" || r.status === "queued" }).length,
                runsCompleted: projectRuns.filter(function (r) { return r.status === "completed" }).length,
                runsFailed: projectRuns.filter(function (r) {
                    return r.status === "failed" || r.status === "cancelled" || r.status === "interrupted"
                }).length,
                lastRunTitle: lastRun ? lastRun.title : "",
                lastRunStatusLabel: lastRun ? lastRun.statusLabel : ""
            })
        }
        page.projectsSnapshot = items

        // Keep the stable-ID selection valid: preserve it if the project still
        // exists, otherwise fall back to the first project (or no selection).
        if (!items.some(function (p) { return p.projectId === page.selectedProjectId })) {
            const activeIsVisible = items.some(function (p) {
                return p.projectId === page.activeProjectId
            })
            page.selectedProjectId = activeIsVisible
                    ? page.activeProjectId
                    : (items.length > 0 ? items[0].projectId : "")
        }
    }

    Connections {
        target: page.backend.workspace
        function onActiveProjectIdChanged() {
            if (page.projectsSnapshot.some(function (project) {
                    return project.projectId === page.activeProjectId
                })) {
                page.selectedProjectId = page.activeProjectId
            }
        }
    }

    function rebuildRunsSnapshot() {
        const items = []
        for (let i = 0; i < runsMirror.count; ++i) {
            const it = runsMirror.itemAt(i)
            if (!it)
                continue
            items.push({
                runId: it.runId,
                projectId: it.projectId,
                title: it.title,
                status: it.status,
                statusLabel: page.runStatusLabel(it.status),
                createdAt: it.createdAt
            })
        }
        items.sort(function (a, b) { return a.createdAt - b.createdAt })
        page.runsSnapshot = items
        page.rebuildProjectsSnapshot()
    }

    function scheduleRunsSnapshotRebuild() {
        if (page.runsSnapshotRebuildScheduled)
            return
        page.runsSnapshotRebuildScheduled = true
        Qt.callLater(function() {
            page.runsSnapshotRebuildScheduled = false
            page.rebuildRunsSnapshot()
        })
    }

    // Non-visual mirrors: they expose backend.projects.model / backend.runs.model
    // role data to JS. No business state is invented here.
    Repeater {
        id: projectsMirror
        model: page.backend.projects.model
        onCountChanged: page.rebuildProjectsSnapshot()
        delegate: Item {
            required property string projectId
            required property string name
            required property string description
            required property string rootPath
            required property string status
            required property var createdAt
            required property var updatedAt
            Component.onCompleted: page.rebuildProjectsSnapshot()
        }
    }

    Repeater {
        id: runsMirror
        model: page.backend.runs.model
        onCountChanged: page.scheduleRunsSnapshotRebuild()
        delegate: Item {
            required property string runId
            required property string projectId
            required property string title
            required property string status
            required property var createdAt
            Component.onCompleted: page.scheduleRunsSnapshotRebuild()
            // RunListModel updates an existing run's row in place
            // (dataChanged) rather than a full reset, so a status change no
            // longer recreates this delegate (which used to be what
            // triggered a rebuild here via Component.onCompleted) - needs
            // its own handler to keep runsSnapshot's statusLabel current.
            onStatusChanged: page.scheduleRunsSnapshotRebuild()
        }
    }

    Repeater {
        id: tasksMirror
        model: page.backend.tasks.model
        onCountChanged: page.rebuildTasksSnapshot()
        delegate: Item {
            required property string taskId
            required property string projectId
            required property string title
            required property string description
            required property string status
            required property string priority
            required property bool requiresAttention
            required property var dueAt
            required property var plannedRunAt
            Component.onCompleted: page.rebuildTasksSnapshot()
            onStatusChanged: page.rebuildTasksSnapshot()
            onRequiresAttentionChanged: page.rebuildTasksSnapshot()
        }
    }

    function createProject(name, path, description, verificationSetup) {
        // AG-18.4B: the preset is written FIRST, before the Project row -
        // this is a combined operation with compensation, not a true
        // distributed transaction (SQLite and the filesystem cannot be
        // joined into one atomic commit). If Project-row creation then
        // fails for ANY reason (duplicate path, storage failure, ...), the
        // just-written preset is now orphaned - no Project references it -
        // so it is explicitly discarded below before surfacing the error,
        // rather than left behind as a permanent preset_already_exists
        // trap on retry. Only verificationSetup.mode === "create" ever
        // writes anything; the other two modes are pure no-ops here,
        // matching prior behavior.
        let presetCleanupToken = 0
        if (verificationSetup && verificationSetup.mode === "create") {
            const presetResult = page.backend.projectVerificationSetup.createLocalPreset(
                path, verificationSetup.generator, verificationSetup.configuration,
                verificationSetup.qtPrefixPath, verificationSetup.configurePresetName,
                verificationSetup.buildPresetName, verificationSetup.binaryDirRelative)
            if (!presetResult.success) {
                newProjectDialog.errorText = presetResult.errorCode + ": " + presetResult.errorText
                return
            }
            presetCleanupToken = presetResult.cleanupToken
        }

        const projectId = page.backend.projects.createProject(name, description, path)
        if (projectId.length > 0) {
            if (presetCleanupToken !== 0)
                page.backend.projectVerificationSetup.releaseGeneratedPreset(presetCleanupToken)
            // AG-18.4B-R1: the pair the user named is the project's selected pair from now on (a Run proves it before use).
            if (verificationSetup && verificationSetup.configurePresetName && verificationSetup.buildPresetName
                    && (verificationSetup.mode === "create" || verificationSetup.mode === "existing")) {
                page.backend.projectVerificationSetup.selectProjectSetupPair(projectId, path,
                    verificationSetup.configurePresetName, verificationSetup.buildPresetName)
            }
            newProjectDialog.close()
            page.selectedProjectId = projectId
            page.projectCreateRequested(name, path, description)
            page.showNotification(qsTr("Project created"))
        } else {
            if (presetCleanupToken !== 0) {
                const cleanup = page.backend.projectVerificationSetup.discardGeneratedPreset(
                    presetCleanupToken)
                if (!cleanup.success) {
                    newProjectDialog.errorText = page.backend.projects.lastError + "\n"
                        + cleanup.errorCode + ": " + cleanup.errorText
                    return
                }
            }
            newProjectDialog.errorText = page.backend.projects.lastError
        }
    }

    function archiveSelectedProject() {
        if (!page.selectedProject)
            return
        const projectId = page.selectedProject.projectId
        if (page.backend.projects.archiveProject(projectId)) {
            page.projectRemoveRequested(projectId)
            page.showNotification(qsTr("Project archived"))
        } else {
            page.showNotification(page.backend.projects.lastError)
        }
    }

    function continueProject(projectId) {
        // Continue only establishes the Backend-owned application context.
        // Starting a workflow remains an explicit action on the Runs page.
        if (page.backend.workspace.selectProject(projectId)) {
            page.projectOpenRequested(projectId)
            page.showNotification(qsTr("Project selected as active"))
        } else {
            page.showNotification(page.backend.workspace.lastError)
        }
    }

    function startOrchestration(projectId) {
        // Establish the selected project context first, then let PageHost open
        // the real New Run flow. Starting an invisible, sub-second Mock run
        // from the project card made the action look broken and bypassed the
        // provider/agent configuration the user expects from orchestration.
        if (page.backend.workspace.selectProject(projectId)) {
            page.orchestrationRequested(projectId)
            page.showNotification(qsTr("Project selected for a new run"))
        } else {
            page.showNotification(page.backend.workspace.lastError)
        }
    }

    function showNotification(message) {
        notificationText = message
        notificationTimer.restart()
    }

    function saveTask(taskId, projectId, title, description, status, priority,
                      requiresAttention, dueAt, plannedRunAt) {
        const ok = taskId.length > 0
            ? page.backend.tasks.updateTask(taskId, title, description, status, priority,
                                            requiresAttention, dueAt, plannedRunAt)
            : page.backend.tasks.createTask(projectId, title, description, status, priority,
                                            requiresAttention, dueAt, plannedRunAt).length > 0
        page.showNotification(ok ? qsTr("Task saved") : page.backend.tasks.lastError)
    }

    Timer {
        id: notificationTimer
        interval: 2800
        onTriggered: page.notificationText = ""
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: page.colors.contentMargin
        spacing: page.hostWindow.dp(14)

        RowLayout {
            Layout.fillWidth: true
            spacing: page.hostWindow.dp(12)

            ColumnLayout {
                Layout.fillWidth: true
                spacing: page.hostWindow.dp(3)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Projects")
                    color: page.colors.textPrimary
                    font.pixelSize: page.hostWindow.dp(28)
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Select a project, review its activity and continue working with AI agents.")
                    color: page.colors.textSecondary
                    font.pixelSize: page.hostWindow.dp(14)
                    elide: Text.ElideRight
                }
            }

            ProjectActionButton {
                text: qsTr("New project")
                iconText: "+"
                primary: true
                hostWindow: page.hostWindow
                colors: page.colors
                onClicked: newProjectDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: page.hostWindow.dp(10)

            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.preferredHeight: page.hostWindow.dp(38)
                placeholderText: qsTr("Search projects")
                color: page.colors.textPrimary
                placeholderTextColor: page.colors.textMuted
                font.pixelSize: page.hostWindow.dp(13)
                selectByMouse: true
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: page.hostWindow.dp(12)
                rightPadding: page.hostWindow.dp(12)

                background: Rectangle {
                    radius: page.colors.cornerRadiusSmall
                    color: page.colors.inputBackground
                    border.width: searchField.activeFocus ? 1 : 0
                    border.color: page.colors.inputBorderFocused
                }
            }

            ProjectComboBox {
                id: statusFilter
                Layout.preferredWidth: page.hostWindow.dp(150)
                Layout.preferredHeight: page.hostWindow.dp(38)
                hostWindow: page.hostWindow
                colors: page.colors

                model: [
                    qsTr("All statuses"),
                    qsTr("Active"),
                    qsTr("Archived"),
                    qsTr("Unavailable")
                ]

                readonly property string filterValue: currentIndex === 1
                                                      ? "active"
                                                      : currentIndex === 2
                                                        ? "archived"
                                                        : currentIndex === 3
                                                          ? "unavailable"
                                                          : "all"
            }

        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: page.hostWindow.dp(14)

            ProjectListPanel {
                Layout.fillHeight: true
                Layout.preferredWidth: page.hostWindow.dp(500)
                Layout.minimumWidth: page.hostWindow.dp(340)
                hostWindow: page.hostWindow
                colors: page.colors
                projectsModel: page.projectsSnapshot
                selectedProjectId: page.selectedProjectId
                searchText: searchField.text
                statusFilter: statusFilter.filterValue
                onSelectionRequested: projectId => page.selectedProjectId = projectId
            }

            ProjectDetailsPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: page.hostWindow.dp(330)
                hostWindow: page.hostWindow
                colors: page.colors
                backend: page.backend
                projectData: page.selectedProject
                tasksModel: page.selectedProjectTasks

                onAddTaskRequested: taskDialog.openFor(page.selectedProjectId, null)
                onEditTaskRequested: task => taskDialog.openFor(page.selectedProjectId, task)
                onCancelTaskRequested: taskId => {
                    const ok = page.backend.tasks.cancelTask(taskId)
                    page.showNotification(ok ? qsTr("Task cancelled") : page.backend.tasks.lastError)
                }
                onEditContextRequested: {
                    if (page.selectedProject)
                        contextPopup.openFor(page.selectedProject.projectId,
                                             page.selectedProject.description)
                }

                onContinueRequested: projectId => {
                    page.continueProject(projectId)
                }

                onOrchestrationRequested: projectId => page.startOrchestration(projectId)

                onRemoveRequested: removeDialog.open()

                onClearMemoryRequested: projectId => {
                    page.clearMemoryProjectId = projectId
                    clearMemoryDialog.open()
                }

                onRestoreRequested: {
                    if (!page.selectedProject)
                        return
                    if (page.backend.projects.restoreProject(page.selectedProject.projectId))
                        page.showNotification(qsTr("Project restored"))
                    else
                        page.showNotification(page.backend.projects.lastError)
                }

                onPathChangeRequested: (projectId, path) => {
                    if (page.backend.projects.updateProjectPath(projectId, path))
                        page.showNotification(qsTr("Project path updated"))
                    else
                        page.showNotification(page.backend.projects.lastError)
                }
            }
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: page.hostWindow.dp(18)
        z: 20
        visible: page.notificationText.length > 0
        width: notificationLabel.implicitWidth + page.hostWindow.dp(28)
        height: page.hostWindow.dp(38)
        radius: page.colors.cornerRadiusSmall
        color: page.colors.tooltipBackground

        Text {
            id: notificationLabel
            anchors.centerIn: parent
            text: page.notificationText
            color: page.colors.tooltipText
            font.pixelSize: page.hostWindow.dp(12)
        }
    }

    NewProjectDialog {
        id: newProjectDialog
        hostWindow: page.hostWindow
        colors: page.colors
        backend: page.backend
        onProjectAccepted: (name, path, description, verificationSetup) => {
            page.createProject(name, path, description, verificationSetup)
        }
    }

    ProjectContextPopup {
        id: contextPopup
        hostWindow: page.hostWindow
        colors: page.colors
        onSaveRequested: (projectId, description) => {
            const ok = page.backend.projects.updateProjectDescription(projectId, description)
            if (ok) {
                page.showNotification(qsTr("Context saved"))
            } else {
                page.showNotification(page.backend.projects.lastError)
            }
        }
    }

    TaskDialog {
        id: taskDialog
        hostWindow: page.hostWindow
        colors: page.colors
        onAcceptedTask: (taskId, projectId, title, description, status, priority,
                         requiresAttention, dueAt, plannedRunAt) =>
            page.saveTask(taskId, projectId, title, description, status, priority,
                          requiresAttention, dueAt, plannedRunAt)
    }

    Dialog {
        id: removeDialog
        anchors.centerIn: parent
        width: page.hostWindow.dp(430)
        modal: true
        title: qsTr("Archive project?")
        standardButtons: Dialog.NoButton

        background: Rectangle {
            radius: page.colors.cornerRadiusMedium
            color: page.colors.popupBackground
            border.width: 1
            border.color: page.colors.border
        }

        contentItem: ColumnLayout {
            spacing: page.hostWindow.dp(16)

            Text {
                Layout.fillWidth: true
                text: page.selectedProject
                      ? qsTr("%1 will be archived. Archiving is non-destructive: project files and run history are kept.")
                        .arg(page.selectedProject.name)
                      : ""
                color: page.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: page.hostWindow.dp(13)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: page.hostWindow.dp(8)

                Item { Layout.fillWidth: true }

                ProjectActionButton {
                    text: qsTr("Cancel")
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: removeDialog.close()
                }

                ProjectActionButton {
                    text: qsTr("Archive")
                    danger: true
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: {
                        removeDialog.close()
                        page.archiveSelectedProject()
                    }
                }
            }
        }
    }

    Dialog {
        id: clearMemoryDialog
        objectName: "clearProjectMemoryDialog"
        anchors.centerIn: parent
        width: page.hostWindow.dp(430)
        modal: true
        title: qsTr("Clear project memory?")
        standardButtons: Dialog.NoButton

        background: Rectangle {
            radius: page.colors.cornerRadiusMedium
            color: page.colors.popupBackground
            border.width: 1
            border.color: page.colors.border
        }

        contentItem: ColumnLayout {
            spacing: page.hostWindow.dp(16)

            Text {
                Layout.fillWidth: true
                text: qsTr("This permanently removes every memory segment for this project. Compaction history is kept as an audit trail. This cannot be undone.")
                color: page.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: page.hostWindow.dp(13)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: page.hostWindow.dp(8)

                Item { Layout.fillWidth: true }

                ProjectActionButton {
                    text: qsTr("Cancel")
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: clearMemoryDialog.close()
                }

                ProjectActionButton {
                    objectName: "confirmClearProjectMemoryButton"
                    text: qsTr("Clear memory")
                    danger: true
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: {
                        clearMemoryDialog.close()
                        page.backend.projectMemory.clearMemory(page.clearMemoryProjectId)
                    }
                }
            }
        }
    }
}
