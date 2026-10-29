import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// The first-visit card of a page that has a "Try it" (a few sentences and the button). Closing it is remembered in the daemon's settings (demoCardsDismissed): it is
// never shown again.
Kirigami.AbstractCard {
    id: card

    required property string kind
    property string body: ""
    property bool closedNow: false
    // A card of a part some daemons do not have (the screen triggers) says so here.
    property bool available: true

    signal tried()

    function dismiss() {
        closedNow = true;
        Daemon.dismissDemoCard(kind);
    }

    Layout.fillWidth: true
    visible: available && Daemon.automationSupported && Daemon.demosSupported && !closedNow && !(Daemon.settings.demoCardsDismissed ?? []).includes(kind)

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Icon {
            source: "help-hint"
            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: card.body
        }
        QQC2.Button {
            icon.name: "media-playback-start"
            text: qsTr("Try it")
            onClicked: card.tried()
        }
        QQC2.ToolButton {
            icon.name: "dialog-close"
            text: qsTr("Close")
            display: QQC2.AbstractButton.IconOnly
            onClicked: card.dismiss()
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: qsTr("Do not show this again")
        }
    }
}
