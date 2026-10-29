import QtQuick
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Asks before "Remove examples" deletes what the "Try it" buttons made (only what is marked as an example: your own profiles, triggers and timelines stay).
KirigamiDialogs.PromptDialog {
    title: qsTr("Remove the examples?")
    subtitle: qsTr("This deletes the timelines, triggers and profiles that the Try it buttons made. Yours stay, and so do your pets.")
    standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel
    onAccepted: Daemon.removeDemos()
}
