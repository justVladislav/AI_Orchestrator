pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

// PC-2 (gap MW-P9): the Run author's interface contracts and constraints. Each entry is an exact piece of
// text (property, signal and field names, a rule) that the Backend hands, word for word, to every worker it
// applies to - the Planner is not the carrier, so it cannot paraphrase or drop it. This editor only
// collects text. It never validates, trims or canonicalizes anything: the Backend does that when the Run or
// the draft is saved and answers with its own error. The limits shown here are display-only mirrors.
ColumnLayout {
    id: editor
    objectName: "interfaceContractsSection"

    required property var hostWindow
    required property var colors

    readonly property int maxContracts: 16
    readonly property int maxTextBytes: 4096
    readonly property int count: contractsModel.count

    spacing: hostWindow.dp(6)

    ListModel { id: contractsModel; objectName: "interfaceContractsModel" }

    function utf8ByteLength(text) {
        // TextEncoder is a browser Web API and is not available in Qt QML.
        let bytes = 0
        for (let index = 0; index < text.length; ++index) {
            const codeUnit = text.charCodeAt(index)
            if (codeUnit < 0x80) {
                bytes += 1
            } else if (codeUnit < 0x800) {
                bytes += 2
            } else if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF && index + 1 < text.length) {
                const low = text.charCodeAt(index + 1)
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    bytes += 4
                    ++index
                } else {
                    bytes += 3
                }
            } else {
                bytes += 3
            }
        }
        return bytes
    }

    // The next free "C<n>" id, so a new entry never starts as a duplicate.
    function nextFreeId() {
        const used = {}
        for (let index = 0; index < contractsModel.count; ++index)
            used[String(contractsModel.get(index).contractId).toLowerCase()] = true
        let candidate = 1
        while (used["c" + candidate] === true)
            ++candidate
        return "C" + candidate
    }

    function addContract() {
        contractsModel.append({
            "contractId": editor.nextFreeId(),
            "kind": "interface_contract",
            "title": "",
            "text": "",
            "scopes": ""
        })
    }

    function clear() {
        contractsModel.clear()
    }

    // The list of maps the Backend expects (RunContractVariant.hpp). Scopes travel as the free text the
    // author typed; the Backend splits and validates them.
    function payload() {
        const result = []
        for (let index = 0; index < contractsModel.count; ++index) {
            const row = contractsModel.get(index)
            result.push({
                "id": String(row.contractId),
                "kind": String(row.kind),
                "title": String(row.title),
                "text": String(row.text),
                "appliesToPaths": String(row.scopes)
            })
        }
        return result
    }

    // The inverse, for Edit & rerun and a reopened draft: exactly the list the Backend froze.
    function apply(contracts) {
        contractsModel.clear()
        const list = contracts || []
        for (let index = 0; index < list.length; ++index) {
            const entry = list[index]
            contractsModel.append({
                "contractId": String(entry.id || ""),
                "kind": String(entry.kind || "interface_contract"),
                "title": String(entry.title || ""),
                "text": String(entry.text || ""),
                "scopes": (entry.appliesToPaths || []).join(", ")
            })
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: editor.hostWindow.dp(8)

        Text {
            Layout.fillWidth: true
            text: qsTr("Interface contracts and constraints")
            color: editor.colors.textPrimary
            font.pixelSize: editor.hostWindow.dp(11)
            font.weight: Font.Medium
            elide: Text.ElideRight
        }

        Text {
            objectName: "interfaceContractsCount"
            text: qsTr("%1 of %2").arg(editor.count).arg(editor.maxContracts)
            color: editor.colors.textMuted
            font.pixelSize: editor.hostWindow.dp(9)
        }

        RunActionButton {
            objectName: "interfaceContractsAddButton"
            hostWindow: editor.hostWindow
            colors: editor.colors
            text: qsTr("Add")
            enabled: editor.count < editor.maxContracts
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Adds an exact piece of text the Backend gives to every worker it applies to, word for word.")
            onClicked: editor.addContract()
        }
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("Exact property, signal and field names shared by tasks, or a rule every affected worker must keep. The Planner does not carry this text: the Backend delivers it word for word to each worker whose files fall under the paths you list (empty paths = every worker).")
        color: editor.colors.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: editor.hostWindow.dp(9)
    }

    Repeater {
        objectName: "interfaceContractsRepeater"
        model: contractsModel

        delegate: Rectangle {
            id: row

            required property int index
            required property string contractId
            required property string kind
            required property string title
            required property string text
            required property string scopes

            objectName: "interfaceContractRow"
            Layout.fillWidth: true
            implicitHeight: rowColumn.implicitHeight + editor.hostWindow.dp(16)
            radius: editor.colors.cornerRadiusSmall
            color: "transparent"
            border.width: 1
            border.color: editor.colors.border

            ColumnLayout {
                id: rowColumn
                anchors.fill: parent
                anchors.margins: editor.hostWindow.dp(8)
                spacing: editor.hostWindow.dp(6)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: editor.hostWindow.dp(6)

                    Basic.TextField {
                        objectName: "interfaceContractIdField"
                        Layout.preferredWidth: editor.hostWindow.dp(84)
                        text: row.contractId
                        placeholderText: qsTr("Id")
                        color: editor.colors.textPrimary
                        placeholderTextColor: editor.colors.textMuted
                        font.pixelSize: editor.hostWindow.dp(10)
                        selectByMouse: true
                        background: Rectangle {
                            radius: editor.colors.cornerRadiusSmall
                            color: editor.colors.inputBackground
                        }
                        onTextEdited: contractsModel.setProperty(row.index, "contractId", text)
                    }

                    RunComboBox {
                        objectName: "interfaceContractKindBox"
                        Layout.preferredWidth: editor.hostWindow.dp(160)
                        Layout.preferredHeight: editor.hostWindow.dp(30)
                        hostWindow: editor.hostWindow
                        colors: editor.colors
                        options: [
                            { "label": qsTr("Interface contract"), "value": "interface_contract" },
                            { "label": qsTr("Constraint"), "value": "constraint" }
                        ]
                        selectedValue: row.kind
                        accessibleLabel: qsTr("Kind of contract")
                        font.pixelSize: editor.hostWindow.dp(10)
                        onValueSelected: value => contractsModel.setProperty(row.index, "kind", value)
                    }

                    Basic.TextField {
                        objectName: "interfaceContractTitleField"
                        Layout.fillWidth: true
                        text: row.title
                        placeholderText: qsTr("Short title")
                        color: editor.colors.textPrimary
                        placeholderTextColor: editor.colors.textMuted
                        font.pixelSize: editor.hostWindow.dp(10)
                        selectByMouse: true
                        background: Rectangle {
                            radius: editor.colors.cornerRadiusSmall
                            color: editor.colors.inputBackground
                        }
                        onTextEdited: contractsModel.setProperty(row.index, "title", text)
                    }

                    RunActionButton {
                        objectName: "interfaceContractRemoveButton"
                        hostWindow: editor.hostWindow
                        colors: editor.colors
                        text: qsTr("Remove")
                        danger: true
                        onClicked: contractsModel.remove(row.index)
                    }
                }

                Basic.TextField {
                    objectName: "interfaceContractScopesField"
                    Layout.fillWidth: true
                    text: row.scopes
                    placeholderText: qsTr("Applies to paths, comma separated (e.g. qml/models, src/) - empty = every worker")
                    color: editor.colors.textPrimary
                    placeholderTextColor: editor.colors.textMuted
                    font.pixelSize: editor.hostWindow.dp(10)
                    selectByMouse: true
                    background: Rectangle {
                        radius: editor.colors.cornerRadiusSmall
                        color: editor.colors.inputBackground
                    }
                    onTextEdited: contractsModel.setProperty(row.index, "scopes", text)
                }

                Basic.ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: editor.hostWindow.dp(88)
                    clip: true
                    background: Rectangle {
                        radius: editor.colors.cornerRadiusSmall
                        color: editor.colors.inputBackground
                    }

                    Basic.TextArea {
                        objectName: "interfaceContractTextField"
                        text: row.text
                        placeholderText: qsTr("The exact text, e.g. property string metricId")
                        color: editor.colors.textPrimary
                        placeholderTextColor: editor.colors.textMuted
                        font.pixelSize: editor.hostWindow.dp(10)
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        background: null
                        onTextChanged: contractsModel.setProperty(row.index, "text", text)
                    }
                }

                Text {
                    objectName: "interfaceContractSize"
                    readonly property int bytes: editor.utf8ByteLength(row.text)
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("%1 / %2 bytes").arg(bytes).arg(editor.maxTextBytes)
                    color: bytes > editor.maxTextBytes ? editor.colors.error : editor.colors.textMuted
                    font.pixelSize: editor.hostWindow.dp(9)
                }
            }
        }
    }
}
