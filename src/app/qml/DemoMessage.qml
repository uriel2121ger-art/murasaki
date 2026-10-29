import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// What a "Try it" tells the user: what it made and what to do next, or that it needs a pet first (with the way to the library). One optional extra button.
Kirigami.InlineMessage {
    id: msg

    property bool needsPet: false
    property string extraText: ""

    signal extraTriggered()

    function say(message, extra) {
        needsPet = false;
        extraText = extra ?? "";
        text = message;
        type = Kirigami.MessageType.Positive;
        visible = true;
    }

    function missingPet() {
        needsPet = true;
        extraText = "";
        text = qsTr("This example needs a pet on the desktop. Add one from the library, then try again.");
        type = Kirigami.MessageType.Warning;
        visible = true;
    }

    Layout.fillWidth: true
    visible: false
    showCloseButton: true
    actions: [
        Kirigami.Action {
            visible: msg.needsPet
            icon.name: "view-list-icons"
            text: qsTr("Open the library")
            onTriggered: {
                msg.visible = false;
                applicationWindow().showPage("library");
            }
        },
        Kirigami.Action {
            visible: msg.extraText !== ""
            icon.name: "media-playback-start"
            text: msg.extraText
            onTriggered: msg.extraTriggered()
        }
    ]
}
