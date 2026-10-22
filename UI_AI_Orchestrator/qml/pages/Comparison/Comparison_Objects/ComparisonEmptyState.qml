import QtQuick
import QtQuick.Layouts

// Centered title/description block reused for the page's non-content
// states: no projects, nothing selected, loading, empty results, and so on.
// Kept deliberately plain (no spinner/animation) to match the project's
// existing convention of immediate, non-animated state changes. An optional
// action control (e.g. a "New comparison" button) can be placed as a child
// and renders centered below the description.
Item {
    id: state

    required property var hostWindow
    required property var colors
    required property string title
    property string description: ""

    default property alias actionItem: actionHolder.data

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.max(0, Math.min(parent.width * 0.86,
                                    state.hostWindow.dp(440)))
        spacing: state.hostWindow.dp(4)

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: state.title
            color: state.colors.textPrimary
            font.pixelSize: state.hostWindow.dp(14)
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            visible: state.description.length > 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: state.description
            color: state.colors.textMuted
            font.pixelSize: state.hostWindow.dp(11.5)
        }

        Item {
            id: actionHolder
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: state.hostWindow.dp(6)
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }
}
