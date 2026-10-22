import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Chat_Objects"

Item {
    id: page

    required property var hostWindow
    required property var colors
    required property var backend

    // ---- Backend-facing UI contract -----------------------------------
    // Persistent state and provider calls are owned by backend.chats.
    signal newConversationRequested()
    signal conversationSelected(string conversationId)
    signal sendMessageRequested(string conversationId, string providerId, string modelId, string message)
    signal stopGenerationRequested(string conversationId)
    signal retryMessageRequested(string messageId)
    signal editMessageRequested(string messageId, string content)
    signal deleteConversationRequested(string conversationId)
    signal clearContextRequested(string conversationId)
    signal providerSelected(string providerId, string modelId)
    // PC-3D: the user asked to see the drafts a plan just produced (the host switches to Runs and opens Drafts).
    signal runDraftsRequested()

    // ---- Responsive breakpoint -----------------------------------------
    // Computed from the page's own logical (design-space) width, so it
    // reacts to the actual space ChatsPage has rather than only the raw
    // ApplicationWindow size.
    readonly property real logicalWidth: hostWindow.scaleFactor > 0
                                         ? width / hostWindow.scaleFactor
                                         : width
    readonly property string layoutMode: logicalWidth >= 960
                                         ? "wide"
                                         : logicalWidth >= 640
                                           ? "medium"
                                           : "narrow"

    property bool narrowShowingConversation: false

    // ---- Selection / active-conversation state --------------------------
    property string selectedConversationId: ""
    property string selectedConversationTitle: ""
    property string selectedConversationProviderId: ""
    property string selectedConversationModelId: ""
    property bool selectedProviderAvailable: true
    property string selectedProviderDisplayName: ""
    property string selectedModelDisplayName: ""
    property string selectedProviderStatusText: ""
    property string selectedProjectId: ""
    property string selectedProjectName: ""
    property var currentModelMetrics: []

    property bool isLoadingMessages: false
    property string generatingConversationId: ""
    property string generatingMessageId: ""

    property string editingMessageId: ""
    property string editingPreviewText: ""
    property var pendingContextQuote: null
    property var contextQuoteCandidate: null

    // Bumped whenever messagesStore/sessionStore (plain JS, not QML
    // properties) change, so currentSessionData below re-evaluates.
    property int sessionRevision: 0

    // QML mirrors of persistent Backend read models. They exist only to keep
    // the established child-component contracts; no row is fabricated here.
    property var messagesStore: ({})
    property var sessionStore: ({})

    ListModel { id: conversationsModel }
    ListModel { id: providersModel }
    ListModel { id: projectsModel }
    ListModel { id: messagesModel }

    readonly property var currentSessionData: {
        sessionRevision

        if (selectedConversationId.length === 0)
            return null

        const stats = sessionStore[selectedConversationId] || {
            inputTokens: 0, outputTokens: 0, totalTokens: 0,
            estimatedCost: "$0.00", contextSummary: "", lastResponseTime: ""
        }

        return {
            providerName: selectedProviderDisplayName,
            modelName: selectedModelDisplayName,
            connectionState: selectedProviderAvailable ? "connected" : "unavailable",
            connectionLabel: selectedProviderStatusText,
            messageCount: (messagesStore[selectedConversationId] || []).length,
            inputTokens: stats.inputTokens,
            outputTokens: stats.outputTokens,
            totalTokens: stats.totalTokens,
            estimatedCost: stats.estimatedCost,
            contextSummary: stats.contextSummary,
            lastResponseTime: stats.lastResponseTime,
            projectName: selectedProjectName,
            modelMetrics: currentModelMetrics
        }
    }

    // ---- Lookup helpers ---------------------------------------------------

    function findConversationIndex(conversationId) {
        for (let i = 0; i < conversationsModel.count; ++i) {
            if (conversationsModel.get(i).conversationId === conversationId)
                return i
        }
        return -1
    }

    function providerRowFor(providerId, modelId) {
        for (let i = 0; i < providersModel.count; ++i) {
            const row = providersModel.get(i)

            if (row.providerId === providerId && row.modelId === modelId)
                return row
        }
        return null
    }

    function defaultProviderRow() {
        const preferredProviderId = backend.settings.defaultChatProviderId
        const preferredModelId = backend.settings.defaultChatModelId
        if (preferredProviderId.length > 0 && preferredModelId.length > 0) {
            const preferred = providerRowFor(preferredProviderId, preferredModelId)
            if (preferred && preferred.available)
                return preferred
        }
        for (let i = 0; i < providersModel.count; ++i) {
            if (providersModel.get(i).available)
                return providersModel.get(i)
        }
        return providersModel.count > 0 ? providersModel.get(0) : null
    }

    function projectNameFor(projectId) {
        for (let i = 0; i < projectsModel.count; ++i) {
            if (projectsModel.get(i).projectId === projectId)
                return projectsModel.get(i).projectName
        }
        return qsTr("No project context")
    }

    function refreshProviderAvailability() {
        const row = providerRowFor(selectedConversationProviderId, selectedConversationModelId)

        selectedProviderAvailable = row ? row.available : false
        selectedProviderDisplayName = row ? row.providerName : qsTr("Unknown provider")
        selectedModelDisplayName = row ? row.modelName : qsTr("Unknown model")
        selectedProviderStatusText = row ? row.statusText : qsTr("No provider selected")
    }

    // ---- Conversation activation -------------------------------------

    function activateConversation(conversationId) {
        clearPendingContextAttachment()
        selectedConversationId = conversationId

        if (conversationId.length === 0) {
            selectedConversationTitle = ""
            selectedConversationProviderId = ""
            selectedConversationModelId = ""
            selectedProviderAvailable = true
            selectedProviderDisplayName = ""
            selectedModelDisplayName = ""
            selectedProviderStatusText = ""
            selectedProjectId = ""
            selectedProjectName = qsTr("No project context")
            currentModelMetrics = []
            messagesModel.clear()
            isLoadingMessages = false
            sessionRevision += 1
            return
        }

        const idx = findConversationIndex(conversationId)

        if (idx < 0)
            return

        const row = conversationsModel.get(idx)
        selectedConversationTitle = row.title
        selectedConversationProviderId = row.providerId
        selectedConversationModelId = row.modelId
        selectedProjectId = row.projectId || ""
        selectedProjectName = projectNameFor(selectedProjectId)
        refreshProviderAvailability()

        if (row.unreadCount > 0)
            conversationsModel.setProperty(idx, "unreadCount", 0)

        refreshMessages(conversationId)

        sessionRevision += 1
    }

    function selectConversation(conversationId) {
        if (conversationId === selectedConversationId)
            return

        editingMessageId = ""
        editingPreviewText = ""
        activateConversation(conversationId)

        if (layoutMode === "narrow")
            narrowShowingConversation = true

        page.conversationSelected(conversationId)
    }

    function createConversation() {
        const provider = defaultProviderRow()
        if (!provider) {
            backendErrorText = qsTr("Configure and enable a provider model first.")
            return ""
        }
        const id = backend.chats.createConversation(provider.providerId, provider.modelId)
        if (id.length === 0)
            return ""
        if (backend.settings.chatContextPolicy !== "active_project"
                && !backend.chats.setProject(id, "")) {
            backendErrorText = qsTr("Could not apply the default chat context policy.")
        }
        refreshConversations()

        editingMessageId = ""
        editingPreviewText = ""
        activateConversation(id)

        if (layoutMode === "narrow")
            narrowShowingConversation = true

        page.newConversationRequested()
        page.conversationSelected(id)

        return id
    }

    function deleteSelectedConversation() {
        if (selectedConversationId.length === 0)
            return

        const idx = findConversationIndex(selectedConversationId)

        if (idx < 0)
            return

        const removedId = selectedConversationId
        if (!backend.chats.removeConversation(removedId))
            return
        refreshConversations()
        editingMessageId = ""
        editingPreviewText = ""

        const nextId = conversationsModel.count > 0
                       ? conversationsModel.get(Math.min(idx, conversationsModel.count - 1)).conversationId
                       : ""
        activateConversation(nextId)

        page.deleteConversationRequested(removedId)
    }

    // ---- Messaging ------------------------------------------------------

    function handleSend(text) {
        if (editingMessageId.length > 0) {
            applyMessageEdit(editingMessageId, text)
            return
        }

        let conversationId = selectedConversationId

        if (conversationId.length === 0)
            conversationId = createConversation()

        page.sendMessageRequested(conversationId, selectedConversationProviderId,
                                  selectedConversationModelId, text)

        const providerRow = providerRowFor(selectedConversationProviderId, selectedConversationModelId)

        if (!providerRow || !providerRow.available) {
            backendErrorText = qsTr("Message not sent: the selected provider is unavailable.")
            return
        }

        const sent = pendingContextQuote !== null
                   ? backend.chats.sendMessageWithProjectContext(
                         conversationId, text, pendingContextQuote.fingerprint, pendingContextQuote.quoteToken)
                   : backend.chats.sendMessage(conversationId, text)
        if (sent)
            clearPendingContextAttachment()
    }

    function clearPendingContextAttachment() {
        pendingContextQuote = null
        contextQuoteCandidate = null
        if (contextAttachmentDialog.visible)
            contextAttachmentDialog.close()
    }

    function requestProjectContextAttachment() {
        if (selectedConversationId.length === 0 || selectedProjectId.length === 0) {
            backendErrorText = qsTr("Select a conversation with a project first.")
            return
        }
        if (!backend.projectContext.hasSnapshot
                || backend.projectContext.projectId !== selectedProjectId) {
            backendErrorText = qsTr("Preview the Project Context Snapshot first.")
            return
        }
        const quote = backend.chats.projectContextQuote(
                          selectedConversationId, backend.projectContext.fingerprint)
        if (!quote.ok) {
            backendErrorText = qsTr("Could not quote project context: %1 (%2)")
                               .arg(quote.errorMessage).arg(quote.errorCode)
            return
        }
        contextQuoteCandidate = quote
        contextAttachmentDialog.open()
    }

    function stopGeneration() {
        if (generatingConversationId.length === 0)
            return

        page.stopGenerationRequested(generatingConversationId)
        backend.chats.stopGeneration()
    }

    function retryMessage(messageId) {
        page.retryMessageRequested(messageId)
        backend.chats.retryMessage(messageId)
    }

    function beginEditMessage(messageId, content) {
        editingMessageId = messageId
        editingPreviewText = content.length > 40 ? content.slice(0, 40) + "\u2026" : content
        composer.setDraft(content)
    }

    function cancelEditMessage() {
        editingMessageId = ""
        editingPreviewText = ""
        composer.setDraft("")
    }

    function applyMessageEdit(messageId, newText) {
        editingMessageId = ""
        editingPreviewText = ""
        composer.setDraft("")

        page.editMessageRequested(messageId, newText)
        backend.chats.editMessage(messageId, newText)
    }

    function handleProviderSelected(providerId, modelId) {
        clearPendingContextAttachment()
        if (selectedConversationId.length === 0) {
            page.providerSelected(providerId, modelId)
            return
        }

        const idx = findConversationIndex(selectedConversationId)

        if (idx >= 0) {
            conversationsModel.setProperty(idx, "providerId", providerId)
            conversationsModel.setProperty(idx, "modelId", modelId)
        }

        selectedConversationProviderId = providerId
        selectedConversationModelId = modelId
        refreshProviderAvailability()
        sessionRevision += 1

        page.providerSelected(providerId, modelId)
        backend.chats.setSelection(selectedConversationId, providerId, modelId)
    }

    function handleProjectSelected(projectId) {
        clearPendingContextAttachment()
        if (selectedConversationId.length === 0)
            return
        if (backend.chats.setProject(selectedConversationId, projectId)) {
            selectedProjectId = projectId
            selectedProjectName = projectNameFor(projectId)
            sessionRevision += 1
        }
    }

    function clearContext() {
        if (selectedConversationId.length === 0 || !sessionStore[selectedConversationId])
            return

        page.clearContextRequested(selectedConversationId)
        backend.chats.clearUsage(selectedConversationId)
    }

    property string backendErrorText: ""
    onBackendErrorTextChanged: {
        if (backendErrorText.length > 0)
            errorBannerTimer.restart()
    }

    Timer {
        id: errorBannerTimer
        interval: 8000
        onTriggered: page.backendErrorText = ""
    }

    // ---- PC-3D: Prompt Workshop (next-run plan -> Run Drafts) ---------------------------------------------------------
    // The Backend parses, validates, previews and saves; this page only relays the user's choices to
    // backend.promptWorkshop and hands the results to the dialog. QML never reads a plan.
    // What the dialog was made from, so "Preview again" repeats exactly that.
    property var planSource: ({ "kind": "", "messageId": "", "pastedText": "" })
    // A preview that is no longer good (changed reply, expired or dropped token, moved conversation): the only way
    // forward is a new preview of the same source.
    readonly property var stalePlanCodes: [
        "run_draft_plan.stale_preview", "run_draft_plan.preview_expired",
        "run_draft_plan.preview_not_found", "run_draft_plan.project_changed"
    ]

    function planErrorText(result) {
        return qsTr("%1 (%2)").arg(result.errorMessage).arg(result.errorCode)
    }

    // Shows a preview result (or its failure) in the dialog. Returns true for a usable preview.
    function applyPlanPreview(result) {
        planDialog.creation = null
        planDialog.offerPreviewAgain = false
        if (!result.ok) {
            planDialog.preview = null
            planDialog.errorText = page.planErrorText(result)
            return false
        }
        planDialog.errorText = ""
        planDialog.selectedRunIds = []
        planDialog.preview = result
        page.loadPlanChoices()
        return true
    }

    // The two pickers of the dialog. Each answer is a typed result: a failure is shown (and can be retried), never
    // taken for "there is nothing to choose".
    function loadPlanChoices() {
        const templates = backend.promptWorkshop.rosterTemplates(page.selectedConversationId)
        const runs = backend.promptWorkshop.sourceRunChoices(page.selectedConversationId)
        const problems = []
        if (!templates.ok)
            problems.push(qsTr("The roster templates could not be loaded: %1").arg(page.planErrorText(templates)))
        if (!runs.ok)
            problems.push(qsTr("The source Runs could not be loaded: %1").arg(page.planErrorText(runs)))
        planDialog.templates = templates.ok ? templates.templates : []
        planDialog.sourceRuns = runs.ok ? runs.runs : []
        planDialog.choiceErrors = problems
        // A selection never outlives its Run.
        planDialog.selectedRunIds = planDialog.selectedRunIds.filter(
                    id => planDialog.sourceRuns.some(run => run.runId === id))
    }

    function openPlanFromMessage(messageId) {
        if (page.selectedConversationId.length === 0)
            return
        page.planSource = { "kind": "message", "messageId": messageId, "pastedText": "" }
        planDialog.reset()
        planDialog.mode = "message"
        page.applyPlanPreview(backend.promptWorkshop.previewFromMessage(page.selectedConversationId, messageId))
        planDialog.open()
    }

    function openPlanImport() {
        if (page.selectedConversationId.length === 0 || page.selectedProjectId.length === 0)
            return
        page.planSource = { "kind": "paste", "messageId": "", "pastedText": "" }
        planDialog.reset()
        planDialog.mode = "import"
        planDialog.open()
    }

    function previewPastedPlan(text) {
        page.planSource = { "kind": "paste", "messageId": "", "pastedText": text }
        page.applyPlanPreview(backend.promptWorkshop.previewFromPaste(page.selectedConversationId, text))
    }

    function previewPlanAgain() {
        if (page.planSource.kind === "message") {
            page.applyPlanPreview(backend.promptWorkshop.previewFromMessage(
                                      page.selectedConversationId, page.planSource.messageId))
        } else if (page.planSource.kind === "paste") {
            page.applyPlanPreview(backend.promptWorkshop.previewFromPaste(
                                      page.selectedConversationId, page.planSource.pastedText))
        }
    }

    // The one write: only the preview's own token, an optional template draft and optional source Runs.
    function confirmPlan(previewToken, templateDraftId, sourceRunIds) {
        const result = backend.promptWorkshop.createFromPreview(previewToken, templateDraftId, sourceRunIds)
        if (!result.ok) {
            planDialog.errorText = page.planErrorText(result)
            planDialog.offerPreviewAgain = page.stalePlanCodes.indexOf(result.errorCode) >= 0
            return
        }
        planDialog.errorText = ""
        planDialog.offerPreviewAgain = false
        planDialog.creation = result
    }

    // PC-1D: the Project Context preview is local, explicit and built by the Backend off the UI thread. It is
    // dropped when the conversation's project changes, so a result is never shown for another project.
    function requestProjectContextPreview(includeProjectMemory) {
        if (selectedProjectId.length === 0)
            return
        backend.projectContext.preview(selectedProjectId, "", includeProjectMemory)
    }

    function clearProjectContextPreview() {
        backend.projectContext.clear()
    }

    onSelectedProjectIdChanged: {
        if (backend.projectContext.projectId !== selectedProjectId)
            backend.projectContext.clear()
    }

    function replaceModel(target, rows) {
        target.clear()
        for (let i = 0; i < rows.length; ++i)
            target.append(rows[i])
    }

    function refreshProviders() {
        replaceModel(providersModel, backend.chats.providerChoices())
        refreshProviderAvailability()
    }

    function refreshProjects() {
        replaceModel(projectsModel, backend.chats.projectChoices())
        selectedProjectName = projectNameFor(selectedProjectId)
    }

    function refreshConversations() {
        const previous = selectedConversationId
        const rows = backend.chats.conversations()
        replaceModel(conversationsModel, rows)
        sessionStore = ({})
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            sessionStore[row.conversationId] = {
                inputTokens: Number(row.inputTokens),
                outputTokens: Number(row.outputTokens),
                totalTokens: Number(row.inputTokens) + Number(row.outputTokens),
                estimatedCost: row.estimatedCost,
                contextSummary: row.projectId
                                ? qsTr("Project Context Snapshot is opt-in per message. Preview it, review tokens and cost, then attach it explicitly.")
                                : qsTr("No project context attached"),
                lastResponseTime: row.lastResponseTime
            }
        }
        sessionRevision += 1
        if (previous.length > 0 && findConversationIndex(previous) >= 0)
            activateConversation(previous)
        else if (conversationsModel.count > 0)
            activateConversation(conversationsModel.get(0).conversationId)
        else
            activateConversation("")
    }

    function refreshMessages(conversationId) {
        isLoadingMessages = true
        const rows = backend.chats.messages(conversationId)
        replaceModel(messagesModel, rows)
        messagesStore[conversationId] = rows
        isLoadingMessages = false
        currentModelMetrics = backend.chats.modelMetrics(conversationId)
        sessionRevision += 1
    }

    function initializeBackendData() {
        refreshProjects()
        refreshProviders()
        refreshConversations()
    }

    // Defer synchronous repository reads until the full object tree has
    // completed construction. This also keeps QQmlApplicationEngine loading
    // deterministic in the headless smoke test.
    Component.onCompleted: Qt.callLater(page.initializeBackendData)

    Connections {
        target: page.backend.chats
        function onConversationsChanged() { page.refreshConversations() }
        function onMessagesChanged(conversationId) {
            if (conversationId === page.selectedConversationId)
                page.refreshMessages(conversationId)
        }
        function onGeneratingChanged() {
            page.generatingConversationId = page.backend.chats.generatingConversationId
            page.generatingMessageId = ""
        }
        function onErrorOccurred(code, message) {
            page.backendErrorText = message
            console.warn("Chats backend:", code, message)
        }
        function onChoicesChanged() {
            page.refreshProjects()
            page.refreshProviders()
        }
    }

    Connections {
        target: page.backend.projectContext
        function onSnapshotChanged() {
            if ((page.pendingContextQuote !== null || page.contextQuoteCandidate !== null)
                    && (!page.backend.projectContext.hasSnapshot
                        || page.backend.projectContext.fingerprint
                           !== (page.pendingContextQuote !== null
                                ? page.pendingContextQuote.fingerprint
                                : page.contextQuoteCandidate.fingerprint)))
                page.clearPendingContextAttachment()
        }
    }

    // ---- Layout -----------------------------------------------------------

    RowLayout {
        anchors.fill: parent
        spacing: page.hostWindow.dp(14)

        ChatSidebar {
            Layout.preferredWidth: page.hostWindow.dp(280)
            Layout.fillWidth: page.layoutMode === "narrow"
            Layout.fillHeight: true
            visible: page.layoutMode !== "narrow" || !page.narrowShowingConversation
            hostWindow: page.hostWindow
            colors: page.colors
            conversationsModel: conversationsModel
            selectedConversationId: page.selectedConversationId

            onNewConversationRequested: page.createConversation()
            onConversationSelected: conversationId => page.selectConversation(conversationId)
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.layoutMode !== "narrow" || page.narrowShowingConversation
            spacing: page.hostWindow.dp(10)

            ChatHeader {
                Layout.fillWidth: true
                hostWindow: page.hostWindow
                colors: page.colors
                hasConversation: page.selectedConversationId.length > 0
                conversationTitle: page.selectedConversationTitle
                providerName: page.selectedProviderDisplayName
                modelName: page.selectedModelDisplayName
                providerAvailable: page.selectedProviderAvailable
                showBackButton: page.layoutMode === "narrow"
                showSessionButton: page.layoutMode === "medium"
                canImportPlan: page.selectedProjectId.length > 0

                onImportPlanRequested: page.openPlanImport()
                onBackRequested: page.narrowShowingConversation = false
                onSessionPanelRequested: sessionPopup.open()
                onDeleteConversationRequested: deleteConversationDialog.open()
            }

            ChatMessageList {
                Layout.fillWidth: true
                Layout.fillHeight: true
                hostWindow: page.hostWindow
                colors: page.colors
                messagesModel: messagesModel
                conversationSelected: page.selectedConversationId.length > 0
                isLoading: page.isLoadingMessages
                providerUnavailable: page.selectedConversationId.length > 0 && !page.selectedProviderAvailable

                onRetryRequested: messageId => page.retryMessage(messageId)
                onEditRequested: (messageId, content) => page.beginEditMessage(messageId, content)
                onPlanDraftsRequested: messageId => page.openPlanFromMessage(messageId)
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: errorBannerRow.implicitHeight + page.hostWindow.dp(14)
                visible: page.backendErrorText.length > 0
                radius: page.colors.cornerRadiusSmall
                color: page.colors.errorSoft
                border.width: 1
                border.color: page.colors.error

                RowLayout {
                    id: errorBannerRow
                    anchors.fill: parent
                    anchors.leftMargin: page.hostWindow.dp(12)
                    anchors.rightMargin: page.hostWindow.dp(6)
                    spacing: page.hostWindow.dp(8)

                    Text {
                        Layout.fillWidth: true
                        text: page.backendErrorText
                        color: page.colors.error
                        wrapMode: Text.WordWrap
                        font.pixelSize: page.hostWindow.dp(11)
                    }

                    Rectangle {
                        Layout.preferredWidth: page.hostWindow.dp(24)
                        Layout.preferredHeight: page.hostWindow.dp(24)
                        radius: page.colors.cornerRadiusSmall
                        color: errorCloseArea.containsMouse ? page.colors.buttonHover : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            color: page.colors.error
                            font.pixelSize: page.hostWindow.dp(15)
                        }

                        MouseArea {
                            id: errorCloseArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.backendErrorText = ""
                        }
                    }
                }
            }

            ChatComposer {
                id: composer
                Layout.fillWidth: true
                hostWindow: page.hostWindow
                colors: page.colors
                providersModel: providersModel
                projectsModel: projectsModel
                providerId: page.selectedConversationProviderId
                modelId: page.selectedConversationModelId
                projectId: page.selectedProjectId
                providerAvailable: page.selectedProviderAvailable
                generating: page.selectedConversationId.length > 0
                           && page.generatingConversationId === page.selectedConversationId
                editing: page.editingMessageId.length > 0
                editingPreview: page.editingPreviewText
                contextAttachmentEnabled: page.pendingContextQuote !== null
                contextAttachmentSummary: page.pendingContextQuote !== null
                                          ? qsTr("%1 · ~%2 tokens · %3")
                                            .arg(page.pendingContextQuote.fingerprint.slice(0, 12))
                                            .arg(page.pendingContextQuote.estimatedInputTokens)
                                            .arg(page.pendingContextQuote.estimatedCost)
                                          : ""

                onSendRequested: text => page.handleSend(text)
                onStopRequested: page.stopGeneration()
                onProviderSelected: (providerId, modelId) => page.handleProviderSelected(providerId, modelId)
                onProjectSelected: projectId => page.handleProjectSelected(projectId)
                onAttachRequested: page.requestProjectContextAttachment()
                onDetachRequested: page.clearPendingContextAttachment()
                onCancelEditRequested: page.cancelEditMessage()
            }
        }

        ChatSessionPanel {
            Layout.preferredWidth: page.hostWindow.dp(300)
            Layout.fillHeight: true
            visible: page.layoutMode === "wide"
            hostWindow: page.hostWindow
            colors: page.colors
            sessionData: page.currentSessionData
            projectContext: page.backend.projectContext
            projectId: page.selectedProjectId
            attachedFingerprint: page.pendingContextQuote !== null ? page.pendingContextQuote.fingerprint : ""

            onClearContextRequested: page.clearContext()
            onProjectContextPreviewRequested: includeProjectMemory => page.requestProjectContextPreview(includeProjectMemory)
            onProjectContextClearRequested: page.clearProjectContextPreview()
            onProjectContextAttachRequested: page.requestProjectContextAttachment()
        }
    }

    Popup {
        id: sessionPopup
        x: page.width - width - page.hostWindow.dp(4)
        y: page.hostWindow.dp(4)
        width: page.hostWindow.dp(320)
        height: Math.min(page.height - page.hostWindow.dp(8), page.hostWindow.dp(520))
        modal: true
        focus: true
        padding: 0

        background: Rectangle {
            radius: page.colors.cornerRadiusMedium
            color: "transparent"
        }

        ChatSessionPanel {
            anchors.fill: parent
            hostWindow: page.hostWindow
            colors: page.colors
            sessionData: page.currentSessionData
            showCloseButton: true
            projectContext: page.backend.projectContext
            projectId: page.selectedProjectId
            attachedFingerprint: page.pendingContextQuote !== null ? page.pendingContextQuote.fingerprint : ""

            onClearContextRequested: page.clearContext()
            onCloseRequested: sessionPopup.close()
            onProjectContextPreviewRequested: includeProjectMemory => page.requestProjectContextPreview(includeProjectMemory)
            onProjectContextClearRequested: page.clearProjectContextPreview()
            onProjectContextAttachRequested: page.requestProjectContextAttachment()
        }
    }

    NextRunPlanDialog {
        id: planDialog
        hostWindow: page.hostWindow
        colors: page.colors

        onPreviewRequested: pastedText => page.previewPastedPlan(pastedText)
        onPreviewAgainRequested: page.previewPlanAgain()
        onReloadChoicesRequested: page.loadPlanChoices()
        onConfirmRequested: (previewToken, templateDraftId, sourceRunIds) =>
                            page.confirmPlan(previewToken, templateDraftId, sourceRunIds)
        onOpenDraftsRequested: page.runDraftsRequested()
    }

    Dialog {
        id: contextAttachmentDialog
        objectName: "chatProjectContextAttachmentDialog"
        anchors.centerIn: parent
        width: page.hostWindow.dp(480)
        modal: true
        title: qsTr("Attach project context?")
        standardButtons: Dialog.NoButton
        onRejected: page.contextQuoteCandidate = null

        background: Rectangle {
            radius: page.colors.cornerRadiusMedium
            color: page.colors.popupBackground
            border.width: 1
            border.color: page.colors.border
        }

        contentItem: ColumnLayout {
            spacing: page.hostWindow.dp(12)

            Text {
                Layout.fillWidth: true
                text: page.contextQuoteCandidate !== null
                      ? qsTr("Snapshot %1 will add about %2 input tokens to the next message.")
                        .arg(page.contextQuoteCandidate.fingerprint.slice(0, 12))
                        .arg(page.contextQuoteCandidate.estimatedInputTokens)
                      : ""
                color: page.colors.textPrimary
                wrapMode: Text.WordWrap
                font.pixelSize: page.hostWindow.dp(12)
            }

            Text {
                objectName: "chatProjectContextCostQuote"
                Layout.fillWidth: true
                text: page.contextQuoteCandidate !== null
                      ? (page.contextQuoteCandidate.priceKnown
                         ? qsTr("Conservative additional input-cost estimate: up to %1. The final bill also depends on conversation history, output, caching and provider-reported usage.")
                           .arg(page.contextQuoteCandidate.estimatedCost)
                         : qsTr("Additional input price is unavailable for this model. Final cost will come from provider-reported usage."))
                      : ""
                color: page.contextQuoteCandidate !== null && page.contextQuoteCandidate.priceKnown
                       ? page.colors.textSecondary : page.colors.warning
                wrapMode: Text.WordWrap
                font.pixelSize: page.hostWindow.dp(11)
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("This confirms attachment only. No request is sent until you press Send. Project Memory, when included, and model-authored Work Ledger prose are untrusted reference data.")
                color: page.colors.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: page.hostWindow.dp(10.5)
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ChatActionButton {
                    text: qsTr("Cancel")
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: {
                        page.contextQuoteCandidate = null
                        contextAttachmentDialog.close()
                    }
                }
                ChatActionButton {
                    objectName: "chatConfirmProjectContextAttachment"
                    text: qsTr("Attach to next message")
                    primary: true
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: {
                        page.pendingContextQuote = page.contextQuoteCandidate
                        page.contextQuoteCandidate = null
                        contextAttachmentDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: deleteConversationDialog
        anchors.centerIn: parent
        width: page.hostWindow.dp(420)
        modal: true
        title: qsTr("Delete conversation?")
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
                text: page.selectedConversationTitle.length > 0
                      ? qsTr("\u201C%1\u201D will be permanently removed from this list.").arg(page.selectedConversationTitle)
                      : ""
                color: page.colors.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: page.hostWindow.dp(13)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: page.hostWindow.dp(8)

                Item { Layout.fillWidth: true }

                ChatActionButton {
                    text: qsTr("Cancel")
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: deleteConversationDialog.close()
                }

                ChatActionButton {
                    text: qsTr("Delete")
                    danger: true
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: {
                        deleteConversationDialog.close()
                        page.deleteSelectedConversation()
                    }
                }
            }
        }
    }
}
