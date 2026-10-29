import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// "Try it" for screen triggers (src/core/demos.h demoScreenTrigger, docs/pets/automation.md "Examples"): a guided walk through the whole thing with the user's own screen.
// 1. an example trigger that hides the pets (off, no picture), 2. the setting and the trigger are turned on, 3. Start (the desktop asks which screens to share), 4. the user
// draws the region, 5. takes the reference picture (and sees it before it is kept), 6. sees the live score. Nothing is real until the end: leaving at any step, by the
// buttons or by closing the window, stops the watch, drops a held picture and puts back what this walk changed (the setting, the example); only "Keep it" keeps.
KirigamiDialogs.Dialog {
    id: dialog

    // The picker of the page (a dim window per screen).
    property Item regionPicker: null
    // intro, preparing, start, asking, region, capture, live, failed, nopet
    property string step: "intro"
    property string triggerId: ""
    // What this walk changed, to put back when it is not kept. Each change is written to the app's journal first (Daemon.noteScreenDemo), so that a GUI that dies in the
    // middle does not leave it on: the next start puts it back (DaemonClient::undoScreenDemo).
    property bool turnedOnSetting: false
    property bool startedWatch: false
    property bool picking: false
    property bool finished: true
    property bool leaving: false // the walk's window was closed while the picture dialog was open
    // Why the walk failed, when the daemon said it in an answer (plain text).
    property string problem: ""

    readonly property var watch: Daemon.screenWatch ?? ({})
    readonly property string watchState: watch.state ?? "off"
    readonly property var triggerState: (Daemon.triggerStates.states ?? []).find(s => s.id === triggerId) ?? ({})
    readonly property int references: (triggerState.refs ?? [])[0] ?? 0
    readonly property int score: (triggerState.screen?.score ?? [])[0] ?? -1
    readonly property bool matching: triggerState.screen?.active ?? false

    title: qsTr("Try a screen trigger")
    preferredWidth: Kirigami.Units.gridUnit * 34
    padding: Kirigami.Units.largeSpacing
    standardButtons: KirigamiDialogs.Dialog.NoButton

    // The example (and its own trigger) is made with the daemon's help; `needsPet`: there is nothing to hide.
    DemoSupport {
        id: demo
        kind: "screen"
        onDone: result => dialog.prepare(result)
        onNeedsPet: dialog.step = "nopet"
    }

    function begin() {
        step = "intro";
        triggerId = "";
        problem = "";
        leaving = false;
        turnedOnSetting = false;
        startedWatch = false;
        picking = false;
        finished = false;
        open();
    }

    // The button of the first step: make the example, and turn on what it needs.
    function go() {
        step = "preparing";
        demo.run();
    }

    // The example is always a new trigger (the daemon client never hands back an earlier one), so undoing is deleting it.
    function note() {
        Daemon.noteScreenDemo({trigger: triggerId, settingTurnedOn: turnedOnSetting, watchStarted: startedWatch});
    }

    function prepare(result) {
        triggerId = result.trigger;
        turnedOnSetting = !(Daemon.settings.screenTriggersEnabled ?? false);
        note();
        if (turnedOnSetting)
            Daemon.setSettings({screenTriggersEnabled: true});
        // (the daemon starts a watch only when a screen trigger is on)
        Daemon.setTriggerEnabled(triggerId, true);
        step = "start";
    }

    function startWatching() {
        if (watchState === "watching") { // already watched (the user started it): used as it is, and left running at the end
            step = "region";
            return;
        }
        startedWatch = true;
        note();
        step = "asking";
        Daemon.startScreenWatchLeased(); // (the daemon stops it by itself if this program dies)
    }

    onWatchStateChanged: {
        if (finished)
            return;
        if (step === "asking" || step === "start") {
            if (watchState === "watching")
                step = "region";
            else if (watchState === "refused" || watchState === "failed" || watchState === "stopped" || (step === "asking" && watchState === "off"))
                step = "failed";
        } else if (step !== "intro" && step !== "preparing" && step !== "failed" && step !== "nopet" && watchState !== "watching" && watchState !== "asking") {
            step = "failed"; // the desktop ended the share, the session was locked for good, the helper failed...
        }
    }

    // The daemon turned the start down (no helper, the session is locked...): it says so in its answer, and the walk ends here instead of waiting for a desktop that was never asked.
    Connections {
        target: Daemon
        function onError(message) {
            if (!dialog.finished && dialog.step === "asking") {
                dialog.problem = String(message);
                dialog.step = "failed";
            }
        }
    }

    function chooseRegion() {
        picking = true;
        close();
        regionPicker.pick();
    }

    Connections {
        target: dialog.regionPicker
        enabled: dialog.picking
        function onPicked(x, y, w, h) {
            dialog.picking = false;
            const trigger = (Daemon.triggers ?? []).find(t => t.id === dialog.triggerId);
            if (trigger) {
                const data = Object.assign({}, trigger.source?.data ?? ({}), {regions: [{kind: "rect", x: x, y: y, w: w, h: h}]});
                Daemon.setTrigger(dialog.triggerId, {source: {kind: "screen", data: data}});
                dialog.step = "capture";
            }
            dialog.open();
        }
        function onCancelled() {
            dialog.picking = false;
            dialog.open();
        }
    }

    ReferenceCaptureDialog {
        id: capture
        onClosed: {
            if (dialog.finished)
                return;
            // The walk's window went first (its close button, the page left) and closed this one with it: the walk is over.
            if (dialog.leaving) {
                dialog.settle(false);
                return;
            }
            if (dialog.step === "capture")
                Daemon.refreshTriggerStates();
        }
    }

    // A kept picture: the walk goes on to the live score.
    Connections {
        target: Daemon
        function onScreenReferenceCommitted() {
            if (!dialog.finished && dialog.step === "capture") {
                Daemon.refreshTriggerStates();
                dialog.step = "live";
            }
        }
    }

    Timer {
        interval: 500
        repeat: true
        running: dialog.visible && dialog.step === "live"
        onTriggered: Daemon.refreshTriggerStates()
    }

    function takePicture() {
        capture.openFor(triggerId, 0);
    }

    // Leaves with what this walk changed put back. `keep`: the example (with its region and picture) and the setting stay, the watch stops (Start is pressed each session),
    // and the example is the user's from now on (no demo marker: "Remove examples" never takes it).
    function settle(keep) {
        if (finished)
            return;
        finished = true;
        if (capture.visible)
            capture.close(); // (drops a held picture)
        if (keep) {
            if (startedWatch)
                Daemon.stopScreenWatch();
            if (triggerId !== "")
                Daemon.setTrigger(triggerId, {origin: ""});
            Daemon.endScreenDemo();
        } else {
            Daemon.undoScreenDemo();
        }
        close();
    }

    onClosed: {
        if (picking)
            return;
        if (capture.visible) {
            leaving = true;
            capture.close(); // (drops a held picture; its close ends the walk)
        } else {
            settle(false);
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: {
                switch (dialog.step) {
                case "intro":
                    return qsTr("This makes an example that hides your pets while a picture you choose is on the screen, and shows them again when it goes. You will need to press Start watching and answer the desktop's question about which screens Murasaki may look at; nothing is read before that, and nothing is kept unless you say so at the end.");
                case "preparing":
                    return qsTr("Making the example…");
                case "start":
                    return qsTr("Ready. Start watching: the desktop will ask which screens to share. Choose the one with the picture.");
                case "asking":
                    return qsTr("The desktop is asking which screens Murasaki may look at. Answer its question.");
                case "region":
                    return qsTr("Watching. Now choose where on the screen the picture appears: you drag a rectangle over it.");
                case "capture":
                    return qsTr("Show on the screen what should hide your pets, then take its picture. You will see the picture before it is kept.");
                case "live":
                    return dialog.matching ? qsTr("The picture is there now: your pets are hidden. Make it go away and they come back.") : qsTr("Watching for the picture. Show it, and your pets hide; hide it, and they come back.");
                case "failed":
                    return dialog.watchState === "refused" ? qsTr("You did not allow it, so nothing was watched.") : dialog.problem !== "" ? qsTr("Watching could not start, so the walk cannot go on.") : qsTr("Watching stopped, so the walk cannot go on.");
                case "nopet":
                    return qsTr("This example needs a pet on the desktop. Add one from the library, then try again.");
                default:
                    return "";
                }
            }
        }

        // The daemon's reason is its own words: plain text.
        QQC2.Label {
            Layout.fillWidth: true
            visible: dialog.step === "failed" && text !== ""
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            opacity: 0.7
            text: dialog.problem !== "" ? dialog.problem.slice(0, 200) : String(dialog.watch.reason ?? "").slice(0, 200)
        }

        QQC2.BusyIndicator {
            visible: dialog.step === "asking" || dialog.step === "preparing"
            running: visible
        }

        RowLayout {
            visible: dialog.step === "live"
            Layout.fillWidth: true
            QQC2.ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1000
                value: Math.max(0, dialog.score)
                Accessible.name: qsTr("How much the picture matches: %1%").arg(Math.max(0, Math.round(dialog.score / 10)))
            }
            QQC2.Label {
                text: dialog.score >= 0 ? qsTr("%1%").arg(Math.round(dialog.score / 10)) : qsTr("…")
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Kirigami.Units.smallSpacing

            QQC2.Button {
                visible: dialog.step === "intro"
                icon.name: "media-playback-start"
                text: qsTr("Begin")
                onClicked: dialog.go()
            }
            QQC2.Button {
                visible: dialog.step === "start"
                icon.name: "view-preview"
                text: qsTr("Start watching")
                onClicked: dialog.startWatching()
            }
            QQC2.Button {
                visible: dialog.step === "region"
                icon.name: "transform-crop"
                text: qsTr("Choose the region")
                onClicked: dialog.chooseRegion()
            }
            QQC2.Button {
                visible: dialog.step === "capture"
                icon.name: "camera-photo"
                text: qsTr("Take the picture…")
                onClicked: dialog.takePicture()
            }
            QQC2.Button {
                visible: dialog.step === "live"
                icon.name: "dialog-ok"
                text: qsTr("Keep this trigger")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: qsTr("Keeps the trigger, its region and its picture, turned on. Watching stops; you start it again another time.")
                onClicked: dialog.settle(true)
            }
            QQC2.Button {
                visible: dialog.step === "nopet"
                icon.name: "view-list-icons"
                text: qsTr("Open the library")
                onClicked: {
                    dialog.settle(false);
                    applicationWindow().showPage("library");
                }
            }
            QQC2.Button {
                icon.name: "dialog-cancel"
                text: dialog.step === "live" ? qsTr("Undo all of this") : dialog.step === "failed" || dialog.step === "nopet" ? qsTr("Close") : qsTr("Cancel")
                QQC2.ToolTip.visible: hovered && dialog.step !== "intro" && dialog.step !== "failed" && dialog.step !== "nopet"
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: qsTr("Stops watching and puts everything back as it was: the example is removed and the setting goes back.")
                onClicked: dialog.settle(false)
            }
        }
    }
}
