import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Timelines (docs/pets/automation.md): tracks of actions on pets (show, hide, place, move, sound, wait) that a trigger or this page plays. What a run changes is not
// stored: the pets go back when it ends (unless the timeline says otherwise), `pets rescue` and "Put the pets back" undo it at once.
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Timelines")

    property string deleting: ""

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).slice(0, 120);
    }

    function summary(t) {
        const tracks = (t.tracks ?? []).length;
        let actions = 0;
        for (const track of (t.tracks ?? []))
            actions += (track.actions ?? []).length;
        return qsTr("%n track(s)", "", tracks) + " · " + qsTr("%n action(s)", "", actions) + " · " + qsTr("speed %1").arg(t.speed ?? 1)
            + ((t.origin ?? "") !== "" ? " · " + qsTr("from %1").arg(plain(t.origin)) : "");
    }

    // For the smoke test: opens and closes the dialogs once, and presses "Try it".
    function smoke() {
        demo.run();
        demoRemoveDialog.open();
        demoRemoveDialog.close();
        card.dismiss();
        viewer.openExample({id: "x", name: "n", speed: 1, restore: true, tracks: [{pet: {slot: 0}, startVisible: true, actions: [{kind: "track", mode: "follow"}, {kind: "wait", ms: 5}]}]});
        viewer.close();
        editor.openFor({id: "x", name: "n", speed: 1, restore: true, tracks: [{pet: {slot: 0}, startVisible: true, actions: [{kind: "move", x: 1, y: 2, mode: "smooth", durationMs: 100}, {kind: "wait", ms: 5}]}]});
        editor.addTrack();
        editor.addAction(0, "show");
        editor.close();
        deleteDialog.open();
        deleteDialog.close();
    }

    actions: [
        Kirigami.Action {
            id: newAction
            icon.name: "list-add"
            text: qsTr("New timeline…")
            enabled: Daemon.automationSupported
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: editor.openFor(null)
        },
        Kirigami.Action {
            icon.name: "media-playback-start"
            text: qsTr("Try it")
            tooltip: qsTr("Make an example out of your pets and show what it does")
            enabled: Daemon.automationSupported && Daemon.demosSupported
            onTriggered: demo.run()
        },
        Kirigami.Action {
            icon.name: "edit-clear-all"
            text: qsTr("Remove examples")
            visible: Daemon.hasDemos
            enabled: Daemon.automationSupported
            onTriggered: demoRemoveDialog.open()
        },
        Kirigami.Action {
            icon.name: "edit-undo"
            text: qsTr("Put the pets back")
            enabled: Daemon.automationSupported
            onTriggered: Daemon.clearPetRuntime("")
        }
    ]

    TimelineDialog { id: editor }
    TimelineDialog { id: viewer }

    DemoSupport {
        id: demo
        kind: "timelines"
        onDone: result => viewer.openExample(result.timeline)
        onNeedsPet: message.missingPet()
    }

    DemoRemoveDialog { id: demoRemoveDialog }

    KirigamiDialogs.PromptDialog {
        id: deleteDialog
        title: qsTr("Delete the timeline?")
        standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel
        onAccepted: Daemon.deleteTimeline(page.deleting)
    }

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        DemoMessage { id: message }

        DemoCard {
            id: card
            kind: "timelines"
            body: qsTr("A timeline makes your pets do things in order: appear, cross the screen, wait, hide. Try it to see an example made from your own pets; they go back to where they were when it ends.")
            onTried: demo.run()
        }

        Repeater {
            model: Daemon.timelines

            delegate: QQC2.ItemDelegate {
                id: row

                required property var modelData
                readonly property bool running: Daemon.runningTimelines.includes(modelData.id)

                Layout.fillWidth: true

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Icon {
                        source: "view-time-schedule"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            opacity: 0.7
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            text: page.summary(row.modelData)
                        }
                    }
                    QQC2.Label {
                        visible: row.running
                        color: Kirigami.Theme.highlightColor
                        text: qsTr("Running")
                    }
                    QQC2.Button {
                        text: row.running ? qsTr("Stop") : qsTr("Play")
                        icon.name: row.running ? "media-playback-stop" : "media-playback-start"
                        enabled: Daemon.automationSupported
                        onClicked: row.running ? Daemon.stopTimeline(row.modelData.id) : Daemon.runTimeline(row.modelData.id)
                    }
                    QQC2.ToolButton {
                        icon.name: "document-edit"
                        text: qsTr("Change")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: editor.openFor(row.modelData)
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: text
                    }
                    QQC2.ToolButton {
                        icon.name: "edit-delete"
                        text: qsTr("Delete")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: {
                            page.deleting = row.modelData.id;
                            deleteDialog.open();
                        }
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: text
                    }
                }
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.gridUnit * 3
            visible: (Daemon.timelines ?? []).length === 0
            icon.name: Daemon.automationSupported ? "view-time-schedule" : "dialog-warning"
            text: Daemon.automationSupported ? qsTr("No timelines yet") : qsTr("Timelines need a newer Murasaki service")
            explanation: Daemon.automationSupported
                ? qsTr("A timeline shows, hides and moves pets, and plays their sound, in order. Make one here, or from a scene of an imported package (on the pet's page).")
                : qsTr("Update and restart the Murasaki service.")
            helpfulAction: Daemon.automationSupported ? newAction : null
        }
    }
}
