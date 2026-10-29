import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One automation in "My automations": its icon, name and sentence, a switch, and what can be done with it. Names and sentences may
// carry text from files: they are shown as plain text.
Kirigami.AbstractCard {
    id: row

    property string iconName: ""
    property string title: ""
    property string sentence: ""
    property string note: ""
    // What it does now or did last ("Acting now: ...", "Last ran ..."); positive while it acts.
    property string status: ""
    property bool statusPositive: false
    property bool active: false
    property bool canEdit: false
    property bool canTest: false
    property bool canDelete: false

    signal switched(bool on)
    signal editRequested()
    signal testRequested()
    signal removeRequested()

    Layout.fillWidth: true

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Icon {
            source: row.iconName
            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
            Layout.alignment: Qt.AlignTop
            opacity: row.active ? 1 : 0.5
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            QQC2.Label {
                Layout.fillWidth: true
                text: row.title
                textFormat: Text.PlainText
                font.bold: true
                elide: Text.ElideRight
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: row.sentence
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                opacity: 0.8
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: row.note !== ""
                text: row.note
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.neutralTextColor
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: row.status !== "" && row.active
                text: row.status
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: row.statusPositive ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
            }
        }

        QQC2.ToolButton {
            visible: row.canTest
            enabled: row.active
            icon.name: "media-playback-start"
            text: qsTr("Try it")
            display: QQC2.AbstractButton.IconOnly
            onClicked: row.testRequested()
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: qsTr("Run it now, as if it had happened")
        }
        QQC2.ToolButton {
            visible: row.canEdit
            icon.name: "document-edit"
            text: qsTr("Change")
            display: QQC2.AbstractButton.IconOnly
            onClicked: row.editRequested()
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: text
        }
        QQC2.ToolButton {
            visible: row.canDelete
            icon.name: "edit-delete"
            text: qsTr("Delete")
            display: QQC2.AbstractButton.IconOnly
            onClicked: row.removeRequested()
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: text
        }
        QQC2.Switch {
            id: toggle
            checked: row.active
            Accessible.name: row.title
            onToggled: {
                row.switched(checked);
                resync.restart();
            }
            // The real state comes back from the daemon; a change it refused puts the switch back after a moment.
            Timer {
                id: resync
                interval: 1500
                onTriggered: toggle.checked = Qt.binding(() => row.active)
            }
        }
    }
}
