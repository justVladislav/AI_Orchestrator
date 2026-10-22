import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "Providers_Objects"
import "../Runs/Runs_Objects"

Item {
    id: page

    required property var hostWindow
    required property var colors
    // Qt adapter over the Backend-owned provider/model catalog.
    required property var backend

    signal providerCreateRequested(string name, string providerType, string baseUrl, string credential)
    signal providerUpdateRequested(string providerId, string name, string providerType, string baseUrl, string credential)
    signal providerRemoveRequested(string providerId)
    signal providerTestRequested(string providerId)
    signal providerEnabledChanged(string providerId, bool enabled)
    signal modelCreateRequested(string providerId, string modelId, string name, string contextLabel, string inputPrice, string outputPrice)
    signal modelRemoveRequested(string modelId)
    signal defaultModelChanged(string providerId, string modelId)
    signal modelEnabledChanged(string modelId, bool enabled)

    property int selectedProviderIndex: providersSnapshot.length > 0 ? 0 : -1
    property string notificationText: ""
    property var providersSnapshot: []
    property var modelsSnapshot: []
    property var profilesSnapshot: []
    property string pendingProviderSelectionId: ""

    readonly property real logicalWidth: width / Math.max(hostWindow.scaleFactor, 0.01)
    readonly property var selectedProvider: selectedProviderIndex >= 0
                                            && selectedProviderIndex < providersSnapshot.length
                                            ? providersSnapshot[selectedProviderIndex]
                                            : null

    function providerTypeLabel(kind) {
        switch (kind) {
        case "openai": return "OpenAI"
        case "anthropic": return "Anthropic"
        case "openai_compatible": return qsTr("OpenAI-compatible")
        case "local": return qsTr("Local runtime")
        default: return kind
        }
    }

    function providerKind(providerType) {
        switch (providerType) {
        case "OpenAI": return "openai"
        case "Anthropic": return "anthropic"
        case "Local runtime": return "local"
        default: return "openai_compatible"
        }
    }

    function compactTokens(value) {
        if (value >= 1000000 && value % 1000000 === 0)
            return (value / 1000000) + "M"
        if (value >= 1000 && value % 1000 === 0)
            return (value / 1000) + "K"
        return String(value)
    }

    function parseTokenCount(label) {
        const normalized = label.trim().toUpperCase()
        const multiplier = normalized.endsWith("M") ? 1000000
                         : normalized.endsWith("K") ? 1000 : 1
        const numericPart = multiplier === 1 ? normalized : normalized.slice(0, -1)
        const value = Number(numericPart)
        return Number.isFinite(value) && value > 0 ? Math.round(value * multiplier) : 0
    }

    function priceLabel(micros) {
        let value = (Number(micros) / 1000000).toFixed(6)
        value = value.replace(/0+$/, "").replace(/\.$/, "")
        if (value.indexOf(".") < 0)
            value += ".00"
        else if (value.length - value.indexOf(".") - 1 < 2)
            value += "0"
        return "$" + value
    }

    function providerIndex(providerId) {
        for (let i = 0; i < providersSnapshot.length; ++i) {
            if (providersSnapshot[i].providerId === providerId)
                return i
        }
        return -1
    }

    function modelIndex(modelId) {
        for (let i = 0; i < modelsSnapshot.length; ++i) {
            if (modelsSnapshot[i].modelId === modelId)
                return i
        }
        return -1
    }

    function hasDefaultModel(providerId) {
        return modelsSnapshot.some(function (model) {
            return model.providerId === providerId && model.isDefault
        })
    }

    function rebuildProvidersSnapshot() {
        const previousId = pendingProviderSelectionId.length > 0
                ? pendingProviderSelectionId
                : (selectedProvider ? selectedProvider.providerId : "")
        const items = []
        for (let i = 0; i < providersMirror.count; ++i) {
            const provider = providersMirror.itemAt(i)
            if (!provider)
                continue
            let modelCount = 0
            for (let j = 0; j < modelsSnapshot.length; ++j) {
                if (modelsSnapshot[j].providerId === provider.providerId)
                    ++modelCount
            }
            items.push({
                providerId: provider.providerId,
                name: provider.name,
                kind: provider.kind,
                type: providerTypeLabel(provider.kind),
                status: provider.providerEnabled ? "active" : "inactive",
                isEnabled: provider.providerEnabled,
                baseUrl: provider.baseUrl,
                credentialState: provider.credentialConfigured ? "configured" : "missing",
                lastCheck: qsTr("Not checked"),
                modelsCount: modelCount,
                monthlyCost: qsTr("No data"),
                tokens: qsTr("No data"),
                description: qsTr("Persisted provider metadata. Connection execution is not enabled yet.")
            })
        }
        providersSnapshot = items
        const preserved = providerIndex(previousId)
        selectedProviderIndex = preserved >= 0 ? preserved : (items.length > 0 ? 0 : -1)
        if (preserved >= 0)
            pendingProviderSelectionId = ""
    }

    function mirrorField(item, key) {
        return item[key]
    }

    function rebuildModelsSnapshot() {
        const items = []
        for (let i = 0; i < modelsMirror.count; ++i) {
            const model = modelsMirror.itemAt(i)
            if (!model)
                continue
            items.push({
                modelId: model.modelId,
                providerId: model.providerId,
                apiModelId: model.apiModelId,
                name: model.displayName,
                contextLabel: compactTokens(Number(model.contextWindowTokens)),
                contextWindowTokens: Number(model.contextWindowTokens),
                maxOutputTokens: Number(model.maxOutputTokens),
                inputPrice: priceLabel(model.inputPriceMicros),
                outputPrice: priceLabel(model.outputPriceMicros),
                inputPriceMicros: Number(model.inputPriceMicros),
                outputPriceMicros: Number(model.outputPriceMicros),
                // The cache prices and the long-context tier are part of the pricing form's
                // prefill: a dialog that did not receive them would show zeros and a save would
                // overwrite the stored values with those zeros. (Read through mirrorField(): the
                // mirror delegate's properties are only known at runtime, and a direct member
                // access would add a qmllint "member not found" warning per field.)
                cacheWritePriceMicros: Number(mirrorField(model, "cacheWritePriceMicros")),
                cacheReadPriceMicros: Number(mirrorField(model, "cacheReadPriceMicros")),
                cacheWriteOneHourPriceMicros: Number(mirrorField(model, "cacheWriteOneHourPriceMicros")),
                longContextEnabled: Boolean(mirrorField(model, "longContextEnabled")),
                longContextThresholdInputTokens: Number(mirrorField(model, "longContextThresholdInputTokens")),
                longContextInputPriceMicros: Number(mirrorField(model, "longContextInputPriceMicros")),
                longContextOutputPriceMicros: Number(mirrorField(model, "longContextOutputPriceMicros")),
                longContextCacheWritePriceMicros: Number(mirrorField(model, "longContextCacheWritePriceMicros")),
                longContextCacheReadPriceMicros: Number(mirrorField(model, "longContextCacheReadPriceMicros")),
                longContextCacheWriteOneHourPriceMicros: Number(mirrorField(model, "longContextCacheWriteOneHourPriceMicros")),
                isEnabled: model.modelEnabled,
                isDefault: model.isDefault
            })
        }
        modelsSnapshot = items
        rebuildProvidersSnapshot()
    }

    function rebuildProfilesSnapshot() {
        const items = []
        for (let i = 0; i < profilesMirror.count; ++i) {
            const profile = profilesMirror.itemAt(i)
            if (!profile)
                continue
            items.push({
                profileId: profile.profileId,
                name: profile.name,
                workflowId: profile.workflowId,
                profileEnabled: profile.profileEnabled,
                isDefault: profile.isDefault,
                assignments: profile.assignments,
                providerBudget: profile.providerBudget
            })
        }
        profilesSnapshot = items
    }

    function saveExecutionProfile(editMode, profileId, name, workflowId, assignments, providerBudget) {
        const success = editMode
                ? backend.executionProfiles.updateProfile(
                      profileId, name, workflowId, assignments, providerBudget)
                : backend.executionProfiles.createProfile(
                      name, workflowId, assignments, providerBudget).length > 0
        if (!success) {
            profileDialog.errorText = backend.executionProfiles.lastError
            showNotification(backend.executionProfiles.lastError)
            return
        }
        profileDialog.accept()
        showNotification(editMode ? qsTr("Execution profile updated")
                                  : qsTr("Execution profile created"))
    }

    function setDefaultExecutionProfile(profileId) {
        if (backend.executionProfiles.setDefaultProfile(profileId)) {
            showNotification(qsTr("Default execution profile updated"))
        } else {
            showNotification(backend.executionProfiles.lastError)
        }
    }

    function saveCredential(providerId, secret) {
        if (backend.credentials.setCredential(providerId, secret)) {
            credentialDialog.accept()
            showNotification(qsTr("Credential saved securely"))
        } else {
            credentialDialog.errorText = backend.credentials.lastError
            showNotification(backend.credentials.lastError)
        }
    }

    function removeCredential(providerId) {
        if (backend.credentials.removeCredential(providerId)) {
            showNotification(qsTr("Credential removed"))
        } else {
            showNotification(backend.credentials.lastError)
        }
    }

    function saveProvider(editMode, providerId, name, providerType, baseUrl, credential) {
        // Credential is deliberately ignored until a secure credential-store
        // adapter exists. Never persist a secret as credentialReference.
        const kind = providerKind(providerType)
        if (editMode) {
            if (backend.providerCatalog.updateProvider(providerId, name, kind, baseUrl)) {
                providerUpdateRequested(providerId, name, providerType, baseUrl, "")
                showNotification(qsTr("Provider settings updated"))
            } else {
                showNotification(backend.providerCatalog.lastError)
            }
            return
        }

        const newProviderId = backend.providerCatalog.createProvider(name, kind, baseUrl)
        if (newProviderId.length > 0) {
            pendingProviderSelectionId = newProviderId
            rebuildProvidersSnapshot()
            providerCreateRequested(name, providerType, baseUrl, "")
            showNotification(qsTr("Provider added"))
        } else {
            showNotification(backend.providerCatalog.lastError)
        }
    }

    function removeSelectedProvider() {
        // ProviderCatalogService intentionally has no delete command yet.
        showNotification(qsTr("Provider removal is not available yet"))
    }

    function setProviderEnabled(providerId, enabled) {
        if (backend.providerCatalog.setProviderEnabled(providerId, enabled)) {
            providerEnabledChanged(providerId, enabled)
            showNotification(enabled ? qsTr("Provider enabled") : qsTr("Provider disabled"))
        } else {
            showNotification(backend.providerCatalog.lastError)
        }
    }

    function createModelRecord(providerId, modelId, name, contextTokens,
                               maxOutputTokens, inputMicros, outputMicros,
                               makeDefault) {
        const newModelId = backend.providerCatalog.createModel(
            providerId, modelId, name, contextTokens, maxOutputTokens,
            inputMicros, outputMicros)
        return finishModelCreation(providerId, newModelId, modelId, name, contextTokens,
                                   inputMicros, outputMicros, makeDefault)
    }

    function finishModelCreation(providerId, newModelId, modelId, name, contextTokens,
                                 inputMicros, outputMicros, makeDefault) {
        if (newModelId.length > 0) {
            if (makeDefault
                    && !backend.providerCatalog.setDefaultModel(providerId, newModelId)) {
                showNotification(backend.providerCatalog.lastError)
                return false
            }
            modelCreateRequested(providerId, modelId, name,
                                 compactTokens(contextTokens),
                                 priceLabel(inputMicros), priceLabel(outputMicros))
            return true
        }
        showNotification(backend.providerCatalog.lastError)
        return false
    }

    function addCustomModel(providerId, modelId, name, contextLabel, inputPrice, outputPrice) {
        const created = createModelRecord(
            providerId, modelId, name, parseTokenCount(contextLabel), 4096,
            Math.round(Number(inputPrice) * 1000000),
            Math.round(Number(outputPrice) * 1000000),
            !hasDefaultModel(providerId))
        if (created)
            showNotification(qsTr("Custom model added"))
    }

    // A price field is a decimal number of dollars; an empty field is "not entered" (0).
    function parsePriceField(text) {
        return Number(String(text).replace(",", "."))
    }

    // The long-context threshold is a whole number of input tokens; empty means 0 (no tier). This
    // only reads the FORMAT: whether a threshold is acceptable for the model is the Backend's rule.
    function parseThresholdField(text) {
        const normalized = String(text).replace(/[\s,_]/g, "")
        if (normalized.length === 0)
            return 0
        if (!/^\d+$/.test(normalized))
            return NaN
        const value = Number(normalized)
        return Number.isSafeInteger(value) ? value : NaN
    }

    // Saves the whole pricing form in ONE atomic Backend update. A call WITHOUT the long-context
    // arguments (the older flat-only form) leaves the stored tier untouched; an explicit
    // longContextEnabled == false removes it. Every value is exactly what the user typed - nothing
    // here guesses a price or a threshold - and every business rule is the Backend's: its typed
    // error text is shown verbatim.
    // Keep the pre-ACD-2 public method at its exact QML meta-object arity. Besides preserving
    // direct QML callers, this matters to C++ callers of QMetaObject::invokeMethod: unlike a
    // JavaScript call, that boundary does not fill omitted arguments with undefined.
    function saveModelPricing(modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,
                              longContextEnabled, longContextThreshold, longContextInputPrice,
                              longContextOutputPrice, longContextCacheWritePrice,
                              longContextCacheReadPrice) {
        saveModelPricingImpl(modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,
                             undefined, longContextEnabled, longContextThreshold,
                             longContextInputPrice, longContextOutputPrice,
                             longContextCacheWritePrice, longContextCacheReadPrice, undefined)
    }

    function saveModelPricingWithCacheDuration(
            modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,
            cacheWriteOneHourPrice,
            longContextEnabled, longContextThreshold, longContextInputPrice,
            longContextOutputPrice, longContextCacheWritePrice,
            longContextCacheReadPrice, longContextCacheWriteOneHourPrice) {
        saveModelPricingImpl(modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,
                             cacheWriteOneHourPrice, longContextEnabled, longContextThreshold,
                             longContextInputPrice, longContextOutputPrice,
                             longContextCacheWritePrice, longContextCacheReadPrice,
                             longContextCacheWriteOneHourPrice)
    }

    function saveModelPricingImpl(
            modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,
            cacheWriteOneHourPrice,
            longContextEnabled, longContextThreshold, longContextInputPrice,
            longContextOutputPrice, longContextCacheWritePrice,
            longContextCacheReadPrice, longContextCacheWriteOneHourPrice) {
        const inputValue = parsePriceField(inputPrice)
        const outputValue = parsePriceField(outputPrice)
        const cacheWriteValue = parsePriceField(cacheWritePrice)
        const cacheReadValue = parsePriceField(cacheReadPrice)
        const hasCacheDurationPrice = cacheWriteOneHourPrice !== undefined
        const cacheWriteOneHourValue = hasCacheDurationPrice
            ? parsePriceField(cacheWriteOneHourPrice) : 0
        if (!Number.isFinite(inputValue) || inputValue < 0
                || !Number.isFinite(outputValue) || outputValue < 0
                || !Number.isFinite(cacheWriteValue) || cacheWriteValue < 0
                || !Number.isFinite(cacheReadValue) || cacheReadValue < 0
                || (hasCacheDurationPrice
                    && (!Number.isFinite(cacheWriteOneHourValue) || cacheWriteOneHourValue < 0))) {
            pricingDialog.errorText = qsTr("Enter valid non-negative prices.")
            return
        }

        let saved = false
        if (longContextEnabled === undefined) {
            saved = backend.providerCatalog.updateModelPricing(
                        modelId, Math.round(inputValue * 1000000),
                        Math.round(outputValue * 1000000),
                        Math.round(cacheWriteValue * 1000000),
                        Math.round(cacheReadValue * 1000000))
        } else {
            let threshold = 0
            let longInput = 0
            let longOutput = 0
            let longCacheWrite = 0
            let longCacheRead = 0
            let longCacheWriteOneHour = 0
            if (longContextEnabled) {
                threshold = parseThresholdField(longContextThreshold)
                longInput = parsePriceField(longContextInputPrice)
                longOutput = parsePriceField(longContextOutputPrice)
                longCacheWrite = parsePriceField(longContextCacheWritePrice)
                longCacheRead = parsePriceField(longContextCacheReadPrice)
                longCacheWriteOneHour = hasCacheDurationPrice
                    ? parsePriceField(longContextCacheWriteOneHourPrice) : 0
                if (!Number.isFinite(threshold)) {
                    pricingDialog.errorText = qsTr("Enter a whole number of input tokens for the threshold.")
                    return
                }
                if (!Number.isFinite(longInput) || longInput < 0
                        || !Number.isFinite(longOutput) || longOutput < 0
                        || !Number.isFinite(longCacheWrite) || longCacheWrite < 0
                        || !Number.isFinite(longCacheRead) || longCacheRead < 0
                        || !Number.isFinite(longCacheWriteOneHour) || longCacheWriteOneHour < 0) {
                    pricingDialog.errorText = qsTr("Enter valid non-negative prices.")
                    return
                }
            }
            if (!hasCacheDurationPrice) {
                saved = backend.providerCatalog.saveModelPricing(
                            modelId, Math.round(inputValue * 1000000),
                            Math.round(outputValue * 1000000),
                            Math.round(cacheWriteValue * 1000000),
                            Math.round(cacheReadValue * 1000000),
                            threshold,
                            Math.round(longInput * 1000000),
                            Math.round(longOutput * 1000000),
                            Math.round(longCacheWrite * 1000000),
                            Math.round(longCacheRead * 1000000))
            } else {
                saved = backend.providerCatalog.saveModelPricingWithCacheDuration(
                            modelId, Math.round(inputValue * 1000000),
                            Math.round(outputValue * 1000000),
                            Math.round(cacheWriteValue * 1000000),
                            Math.round(cacheReadValue * 1000000),
                            Math.round(cacheWriteOneHourValue * 1000000),
                            threshold,
                            Math.round(longInput * 1000000),
                            Math.round(longOutput * 1000000),
                            Math.round(longCacheWrite * 1000000),
                            Math.round(longCacheRead * 1000000),
                            Math.round(longCacheWriteOneHour * 1000000))
            }
        }
        if (saved) {
            pricingDialog.accept()
            showNotification(qsTr("Model pricing updated"))
        } else {
            pricingDialog.errorText = backend.providerCatalog.lastError
        }
    }

    function saveModelEdit(modelId, apiModelId, name, contextLabel, maxOutputTokens, inputPrice, outputPrice) {
        const inputValue = Number(String(inputPrice).replace(",", "."))
        const outputValue = Number(String(outputPrice).replace(",", "."))
        const maxOutputValue = Math.round(Number(maxOutputTokens))
        if (apiModelId.trim().length === 0 || name.trim().length === 0) {
            showNotification(qsTr("Model ID and display name cannot be empty."))
            return
        }
        if (!Number.isFinite(inputValue) || inputValue < 0
                || !Number.isFinite(outputValue) || outputValue < 0) {
            showNotification(qsTr("Enter valid non-negative prices."))
            return
        }
        if (!Number.isFinite(maxOutputValue) || maxOutputValue <= 0) {
            showNotification(qsTr("Enter a valid max output token count."))
            return
        }
        if (backend.providerCatalog.updateModel(
                    modelId, apiModelId.trim(), name.trim(),
                    parseTokenCount(contextLabel), maxOutputValue,
                    Math.round(inputValue * 1000000), Math.round(outputValue * 1000000))) {
            showNotification(qsTr("Model updated"))
        } else {
            showNotification(backend.providerCatalog.lastError)
        }
    }

    // Stable page command retained for tests and external QML callers. The
    // visible UI now names this path explicitly as a custom model.
    function addModel(providerId, modelId, name, contextLabel, inputPrice, outputPrice) {
        addCustomModel(providerId, modelId, name, contextLabel, inputPrice, outputPrice)
    }

    function catalogCandidates(providerId, providerKind) {
        const configuredIds = modelsSnapshot.filter(function(model) {
            return model.providerId === providerId
        }).map(function(model) {
            return model.apiModelId
        })
        return backend.providerCatalog.catalogModelsForProvider(providerKind)
            .filter(function(model) {
                return configuredIds.indexOf(String(model.apiModelId)) < 0
            })
    }

    function openModelCatalog(providerId, providerKind, providerName) {
        modelCatalogDialog.openForProvider(
            providerId, providerName,
            catalogCandidates(providerId, providerKind))
    }

    function addCatalogModels(providerId, entries) {
        let addedCount = 0
        let shouldSetDefault = !hasDefaultModel(providerId)
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            // The Backend creates the row from its own catalog, so the cache prices and the
            // long-context tier are carried too; the entry only identifies the model.
            const created = finishModelCreation(
                providerId,
                backend.providerCatalog.createCatalogModel(providerId, String(entry.apiModelId)),
                String(entry.apiModelId),
                String(entry.displayName),
                Number(entry.contextWindowTokens),
                Number(entry.inputPriceMicros),
                Number(entry.outputPriceMicros),
                shouldSetDefault)
            if (created) {
                ++addedCount
                shouldSetDefault = false
            }
        }
        if (addedCount > 0)
            showNotification(qsTr("%1 catalog model(s) added").arg(addedCount))
    }

    function removeModel(modelId) {
        if (backend.providerCatalog.deleteModel(modelId)) {
            showNotification(qsTr("Model removed"))
        } else {
            showNotification(backend.providerCatalog.lastError)
        }
    }

    function setDefaultModel(modelId) {
        const targetIndex = modelIndex(modelId)
        if (targetIndex < 0)
            return
        const providerId = modelsSnapshot[targetIndex].providerId
        if (backend.providerCatalog.setDefaultModel(providerId, modelId)) {
            defaultModelChanged(providerId, modelId)
            showNotification(qsTr("Default model updated"))
        } else {
            showNotification(backend.providerCatalog.lastError)
        }
    }

    function setModelEnabled(modelId, enabled) {
        if (backend.providerCatalog.setModelEnabled(modelId, enabled)) {
            modelEnabledChanged(modelId, enabled)
        } else {
            showNotification(backend.providerCatalog.lastError)
        }
    }

    function showNotification(message) {
        notificationText = message
        notificationTimer.restart()
    }

    // QAbstractItemModel roles are mirrored into immutable JS snapshots for
    // existing presentation components. Commands always go back to Backend.
    Repeater {
        id: providersMirror
        model: page.backend.providerCatalog.providers
        onCountChanged: page.rebuildProvidersSnapshot()
        delegate: Item {
            required property var model
            required property string providerId
            required property string name
            required property string kind
            required property string baseUrl
            readonly property bool providerEnabled: Boolean(model.enabled)
            required property bool credentialConfigured
            Component.onCompleted: page.rebuildProvidersSnapshot()
        }
    }

    Repeater {
        id: profilesMirror
        model: page.backend.executionProfiles.profiles
        onCountChanged: page.rebuildProfilesSnapshot()
        delegate: Item {
            required property string profileId
            required property string name
            required property string workflowId
            required property bool profileEnabled
            required property bool isDefault
            required property var assignments
            required property var providerBudget
            Component.onCompleted: page.rebuildProfilesSnapshot()
        }
    }

    Repeater {
        id: modelsMirror
        model: page.backend.providerCatalog.models
        onCountChanged: page.rebuildModelsSnapshot()
        delegate: Item {
            required property var model
            required property string modelId
            required property string providerId
            required property string apiModelId
            required property string displayName
            required property var contextWindowTokens
            required property var maxOutputTokens
            required property var inputPriceMicros
            required property var outputPriceMicros
            required property var cacheWritePriceMicros
            required property var cacheReadPriceMicros
            required property var cacheWriteOneHourPriceMicros
            required property bool longContextEnabled
            required property var longContextThresholdInputTokens
            required property var longContextInputPriceMicros
            required property var longContextOutputPriceMicros
            required property var longContextCacheWritePriceMicros
            required property var longContextCacheReadPriceMicros
            required property var longContextCacheWriteOneHourPriceMicros
            readonly property bool modelEnabled: Boolean(model.enabled)
            required property bool isDefault
            Component.onCompleted: page.rebuildModelsSnapshot()
        }
    }

    Timer {
        id: notificationTimer
        interval: 2800
        onTriggered: page.notificationText = ""
    }

    Flickable {
        id: scrollView
        anchors.fill: parent
        contentWidth: width
        contentHeight: contentLayout.implicitHeight + page.colors.contentMargin * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: RunScrollBar {
            id: providersPageScrollBar
            hostWindow: page.hostWindow
            colors: page.colors
        }

        ColumnLayout {
            id: contentLayout
            x: page.colors.contentMargin
            y: page.colors.contentMargin
            width: Math.max(0, scrollView.width - page.colors.contentMargin * 2
                            - providersPageScrollBar.width)
            spacing: page.hostWindow.dp(14)

            RowLayout {
                Layout.fillWidth: true
                spacing: page.hostWindow.dp(12)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: page.hostWindow.dp(3)
                    Text { Layout.fillWidth: true; text: qsTr("Providers and models"); color: page.colors.textPrimary; font.pixelSize: page.hostWindow.dp(28); font.weight: Font.DemiBold; elide: Text.ElideRight }
                    Text { Layout.fillWidth: true; text: qsTr("Manage connections, credentials and the model catalog."); color: page.colors.textSecondary; font.pixelSize: page.hostWindow.dp(13); wrapMode: Text.WordWrap }
                }

                Rectangle {
                    Layout.preferredWidth: demoLabel.implicitWidth + page.hostWindow.dp(18)
                    Layout.preferredHeight: page.hostWindow.dp(28)
                    radius: height / 2
                    color: page.colors.accentSoft
                    Text { id: demoLabel; anchors.centerIn: parent; text: qsTr("Stored catalog"); color: page.colors.accent; font.pixelSize: page.hostWindow.dp(10); font.weight: Font.DemiBold }
                }

                ProviderActionButton {
                    text: qsTr("Add provider")
                    iconText: "+"
                    primary: true
                    hostWindow: page.hostWindow
                    colors: page.colors
                    onClicked: providerDialog.openForCreate()
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: page.logicalWidth >= 900 ? 2 : 1
                columnSpacing: page.hostWindow.dp(14)
                rowSpacing: page.hostWindow.dp(14)

                ProviderListPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: page.hostWindow.dp(340)
                    Layout.preferredHeight: page.hostWindow.dp(620)
                    hostWindow: page.hostWindow
                    colors: page.colors
                    providersModel: page.providersSnapshot
                    selectedIndex: page.selectedProviderIndex
                    onSelectionRequested: index => page.selectedProviderIndex = index
                }

                ProviderDetailsPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: page.hostWindow.dp(700)
                    Layout.preferredHeight: page.hostWindow.dp(620)
                    hostWindow: page.hostWindow
                    colors: page.colors
                    providerData: page.selectedProvider
                    modelsModel: page.modelsSnapshot
                    onEditRequested: providerDialog.openForEdit(page.selectedProvider)
                    onTestRequested: providerId => {
                        page.providerTestRequested(providerId)
                        page.showNotification(qsTr("Connection test requested"))
                    }
                    onRemoveRequested: removeProviderDialog.open()
                    onProviderEnabledChangeRequested: (providerId, enabled) => page.setProviderEnabled(providerId, enabled)
                    onAddCatalogModelsRequested: (providerId, providerKind, providerName) =>
                        page.openModelCatalog(providerId, providerKind, providerName)
                    onAddCustomModelRequested: providerId => modelDialog.openForProvider(providerId)
                    onRemoveModelRequested: modelId => page.removeModel(modelId)
                    onDefaultModelRequested: modelId => page.setDefaultModel(modelId)
                    onModelEnabledChanged: (modelId, enabled) => page.setModelEnabled(modelId, enabled)
                    onModelPricingRequested: modelData => pricingDialog.openForModel(modelData)
                    onModelEditRequested: modelData => modelDialog.openForEdit(modelData)
                    onConfigureCredentialRequested: (providerId, providerName) =>
                        credentialDialog.openForProvider(providerId, providerName)
                    onRemoveCredentialRequested: providerId => page.removeCredential(providerId)
                }
            }

            ExecutionProfilesPanel {
                Layout.fillWidth: true
                hostWindow: page.hostWindow
                colors: page.colors
                profilesModel: page.profilesSnapshot
                onCreateRequested: profileDialog.openForCreate()
                onEditRequested: profileData => profileDialog.openForEdit(profileData)
                onDefaultRequested: profileId => page.setDefaultExecutionProfile(profileId)
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
        Text { id: notificationLabel; anchors.centerIn: parent; text: page.notificationText; color: page.colors.tooltipText; font.pixelSize: page.hostWindow.dp(11) }
    }

    ProviderFormDialog {
        id: providerDialog
        hostWindow: page.hostWindow
        colors: page.colors
        onProviderAccepted: (editMode, providerId, name, providerType, baseUrl, credential) => page.saveProvider(editMode, providerId, name, providerType, baseUrl, credential)
    }

    ModelFormDialog {
        id: modelDialog
        hostWindow: page.hostWindow
        colors: page.colors
        onModelAccepted: (providerId, modelId, name, contextLabel, inputPrice, outputPrice) => page.addCustomModel(providerId, modelId, name, contextLabel, inputPrice, outputPrice)
        onModelEditAccepted: (modelId, apiModelId, name, contextLabel, maxOutputTokens, inputPrice, outputPrice) =>
            page.saveModelEdit(modelId, apiModelId, name, contextLabel, maxOutputTokens, inputPrice, outputPrice)
    }

    ModelPricingDialog {
        id: pricingDialog
        objectName: "pricingDialog"
        hostWindow: page.hostWindow
        colors: page.colors
        onPricingAccepted: (modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,
                            cacheWriteOneHourPrice,
                            longContextEnabled, longContextThreshold, longContextInputPrice,
                            longContextOutputPrice, longContextCacheWritePrice,
                            longContextCacheReadPrice, longContextCacheWriteOneHourPrice) =>
            page.saveModelPricingWithCacheDuration(
                    modelId, inputPrice, outputPrice, cacheWritePrice, cacheReadPrice,
                    cacheWriteOneHourPrice,
                    longContextEnabled, longContextThreshold, longContextInputPrice,
                    longContextOutputPrice, longContextCacheWritePrice,
                    longContextCacheReadPrice, longContextCacheWriteOneHourPrice)
    }

    ModelCatalogDialog {
        id: modelCatalogDialog
        hostWindow: page.hostWindow
        colors: page.colors
        onModelsAccepted: (providerId, models) => page.addCatalogModels(providerId, models)
    }

    ExecutionProfileDialog {
        id: profileDialog
        hostWindow: page.hostWindow
        colors: page.colors
        providersModel: page.providersSnapshot
        modelsModel: page.modelsSnapshot
        onProfileAccepted: (editMode, profileId, name, workflowId, assignments, providerBudget) =>
            page.saveExecutionProfile(editMode, profileId, name, workflowId, assignments, providerBudget)
    }

    CredentialDialog {
        id: credentialDialog
        hostWindow: page.hostWindow
        colors: page.colors
        onCredentialAccepted: (providerId, secret) => page.saveCredential(providerId, secret)
    }

    Dialog {
        id: removeProviderDialog
        anchors.centerIn: parent
        width: Math.min(parent ? parent.width - page.hostWindow.dp(32) : page.hostWindow.dp(430), page.hostWindow.dp(430))
        modal: true
        title: qsTr("Remove provider?")
        standardButtons: Dialog.NoButton
        background: Rectangle { radius: page.colors.cornerRadiusMedium; color: page.colors.popupBackground; border.width: 1; border.color: page.colors.border }
        contentItem: ColumnLayout {
            spacing: page.hostWindow.dp(14)
            Text { Layout.fillWidth: true; text: page.selectedProvider ? qsTr("%1 and its local model entries will be removed from this UI.").arg(page.selectedProvider.name) : ""; color: page.colors.textSecondary; wrapMode: Text.WordWrap; font.pixelSize: page.hostWindow.dp(11) }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ProviderActionButton { text: qsTr("Cancel"); hostWindow: page.hostWindow; colors: page.colors; onClicked: removeProviderDialog.reject() }
                ProviderActionButton { text: qsTr("Remove"); danger: true; hostWindow: page.hostWindow; colors: page.colors; onClicked: { removeProviderDialog.accept(); page.removeSelectedProvider() } }
            }
        }
    }
}
