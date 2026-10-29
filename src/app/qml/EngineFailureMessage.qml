import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Why one screen does not show its wallpaper, with the way out: try again.
// `failure` is the "failure" of an entry of Surface.outputs ({kind, reason, engine, canTryOther}); without a kind there is
// nothing to say. Reasons come from engine and page messages: they are untrusted text.
Kirigami.InlineMessage {
    id: message

    required property string output
    property var failure: null

    readonly property bool failed: failure !== null && failure !== undefined && (failure.kind ?? "") !== ""

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    Layout.fillWidth: true
    visible: failed
    type: Kirigami.MessageType.Warning
    text: qsTr("Could not show this wallpaper: %1").arg(plain(failure ? failure.reason : ""))
    actions: [
        Kirigami.Action {
            icon.name: "view-refresh"
            text: qsTr("Retry")
            onTriggered: Surface.retryOutput(message.output)
        }
    ]
}
