import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// What makes things happen by themselves (docs/pets/automation.md): a trigger watches for something and answers with one action on the pets. A trigger acts on pets
// only: it never runs a program or opens a file or a web address. Everything here comes from files or imports: texts are shown as plain text.
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Triggers")

    property string deleting: ""

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).slice(0, 120);
    }

    function sourceText(trigger) {
        const s = trigger.source ?? ({});
        switch (s.kind) {
        case "app-focus": return s.on === "leave" ? qsTr("%1 loses the focus").arg(plain(s.app)) : qsTr("%1 gets the focus").arg(plain(s.app));
        case "app-open": return s.on === "close" ? qsTr("%1 closes").arg(plain(s.app)) : qsTr("%1 opens").arg(plain(s.app));
        case "fullscreen": return s.on === "leave" ? qsTr("a fullscreen window ends") : qsTr("a window goes fullscreen");
        case "time": return qsTr("at %1").arg(plain(s.at));
        case "interval": return qsTr("every %n minute(s)", "", s.everyMinutes ?? 1);
        case "idle": return qsTr("idle for %n minute(s)", "", s.minutes ?? 1);
        case "power": return s.state === "battery" ? qsTr("on battery") : qsTr("plugged in");
        case "session": return s.state === "locked" ? qsTr("the session is locked") : qsTr("the session is unlocked");
        case "game": return s.state === "started" ? qsTr("a game starts") : qsTr("the game ends");
        case "output": return s.state === "added" ? qsTr("a screen is plugged") : qsTr("a screen is unplugged");
        case "shortcut": return qsTr("the key %1").arg(plain(s.id));
        case "signal-file": return qsTr("the file %1").arg(plain(s.name));
        case "fire": return qsTr("called %1").arg(plain(s.name));
        case "dbus": return qsTr("a D-Bus signal (%1)").arg(plain(s.member));
        case "screen": {
            const regions = (s.data?.regions ?? []).length;
            return qsTr("a picture is on the screen (%n region(s))", "", regions);
        }
        default: return plain(s.kind);
        }
    }

    function actionText(trigger) {
        const a = trigger.action ?? ({});
        switch (a.type) {
        case "timeline": return qsTr("play %1").arg(plain((Daemon.timelines ?? []).find(t => t.id === a.id)?.name ?? "?"));
        case "profile": return qsTr("switch to %1").arg(plain((Daemon.profiles ?? []).find(p => p.id === a.id)?.name ?? "?"));
        case "hide-all": return qsTr("hide the pets");
        case "show-all": return qsTr("show the pets");
        case "toggle-hidden": return qsTr("hide or show the pets");
        case "stop-sounds": return qsTr("silence the pets");
        case "mute": return a.mode === "on" ? qsTr("mute the pets") : a.mode === "off" ? qsTr("unmute the pets") : qsTr("mute or unmute the pets");
        case "play-sound": return qsTr("play a sound");
        case "scene": return qsTr("start scene %1").arg((a.index ?? 0) + 1);
        default: return plain(a.type);
        }
    }

    function stateOf(id) {
        return (Daemon.triggerStates.states ?? []).find(s => s.id === id) ?? ({});
    }

    function outcomeText(state) {
        if (!state.available && state.note === "automation: screen triggers are not active")
            return qsTr("never fires: screen triggers are not active yet");
        if (state.screen !== undefined && state.screen.active)
            return qsTr("the picture is there now");
        if (state.available === false)
            return qsTr("this session does not give the facts it needs");
        switch (state.outcome) {
        case "ran": return qsTr("last: ran %1").arg(page.ago(state.agoMs));
        case "cooldown": return qsTr("last: waited (cooldown)");
        case "rate limited": return qsTr("last: dropped (too many)");
        case "chain too deep": return qsTr("last: cut (a loop of triggers)");
        case "disabled": return qsTr("last: it was off");
        case "shortcut unbound": return qsTr("no key is bound to it yet");
        default: return "";
        }
    }

    function ago(ms) {
        if (ms === undefined)
            return "";
        const s = Math.round(ms / 1000);
        return s < 90 ? qsTr("%n second(s) ago", "", s) : qsTr("%n minute(s) ago", "", Math.round(s / 60));
    }

    function editable(trigger) {
        const kind = trigger.source?.kind;
        return kind === "screen" ? Daemon.screenSupported : kind !== "dbus";
    }

    // For the smoke test: opens and closes the dialogs once, and presses "Try it".
    function smoke() {
        demo.run();
        demoRemoveDialog.open();
        demoRemoveDialog.close();
        card.dismiss();
        dialog.openFor({id: "x", name: "n", enabled: true, cooldownMs: 500, source: {kind: "time", at: "22:00", days: [1]}, action: {type: "mute", mode: "on"}});
        dialog.buildChanges();
        dialog.close();
        dialog.openFor(null);
        dialog.close();
        screenDialog.openFor({id: "t3", name: "s", enabled: true, cooldownMs: 500, mode: "hold", source: {kind: "screen", data: {regions: [{kind: "rect", x: 1, y: 2, w: 30, h: 40}, {kind: "circle", x: 5, y: 6, w: 7, h: 8}], match: {mode: "ORB_HASH", threshold: 0.75}}}, action: {type: "timeline", id: "l1"}});
        screenDialog.buildChanges();
        screenDialog.close();
        screenDialog.openFor(null);
        screenDialog.close();
        screenDialog.openCapture(0);
        screenDemo.begin();
        screenDemo.close();
        screenCard.dismiss();
        deleteDialog.open();
        deleteDialog.close();
    }

    actions: [
        Kirigami.Action {
            id: newAction
            icon.name: "list-add"
            text: qsTr("New trigger…")
            enabled: Daemon.automationSupported
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: dialog.openFor(null)
        },
        Kirigami.Action {
            icon.name: "view-preview"
            text: qsTr("New screen trigger…")
            visible: Daemon.screenSupported
            enabled: Daemon.automationSupported
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: screenDialog.openFor(null)
        },
        Kirigami.Action {
            icon.name: "media-playback-start"
            text: qsTr("Try it")
            tooltip: qsTr("Make an example trigger that plays an example timeline")
            enabled: Daemon.automationSupported && Daemon.demosSupported
            onTriggered: demo.run()
        },
        Kirigami.Action {
            icon.name: "view-preview"
            text: qsTr("Try a screen trigger")
            tooltip: qsTr("A guided walk: hide your pets while a picture you choose is on the screen")
            visible: Daemon.screenSupported && Daemon.demosSupported
            enabled: Daemon.automationSupported
            onTriggered: screenDemo.begin()
        },
        Kirigami.Action {
            icon.name: "edit-clear-all"
            text: qsTr("Remove examples")
            visible: Daemon.hasDemos
            enabled: Daemon.automationSupported
            onTriggered: demoRemoveDialog.open()
        }
    ]

    TriggerDialog { id: dialog }
    ScreenTriggerDialog { id: screenDialog; regionPicker: picker }
    RegionPicker { id: picker }
    ScreenDemoDialog { id: screenDemo; regionPicker: picker }

    // The live scores of a screen trigger come with TriggerState(); while the screen is watched and this page is open they are read twice a second.
    Timer {
        interval: 500
        repeat: true
        running: page.visible && (Daemon.screenWatch?.state ?? "off") === "watching"
        onTriggered: Daemon.refreshTriggerStates()
    }

    DemoSupport {
        id: demo
        kind: "triggers"
        onDone: result => {
            message.testId = result.trigger;
            message.say(qsTr("Made a trigger and the timeline it plays. A trigger fires when you press its key, and the desktop asks you which key: open System Settings, Shortcuts, Murasaki and give the example shortcut a key (the first time, the desktop may ask by itself). Then press it. You can also run it now."), qsTr("Run it now"));
        }
        onNeedsPet: message.missingPet()
    }

    DemoRemoveDialog { id: demoRemoveDialog }

    Connections {
        target: Daemon
        function onSettingsChanged() { masterSwitch.checked = Daemon.settings.automationEnabled ?? true; }
    }

    KirigamiDialogs.PromptDialog {
        id: deleteDialog
        title: qsTr("Delete the trigger?")
        standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel
        onAccepted: Daemon.deleteTrigger(page.deleting)
    }

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        DemoMessage {
            id: message
            property string testId: ""
            onExtraTriggered: Daemon.testTrigger(testId)
        }

        ScreenWatchBanner {
            Layout.fillWidth: true
        }

        DemoCard {
            id: screenCard
            kind: "screen"
            available: Daemon.screenSupported
            body: qsTr("A screen trigger can make your pets react to what is on your screen: for example, hide while a game or a video is showing. You choose a region and take its picture yourself; nothing is looked at until you press Start and the desktop agrees. Try it for a guided walk.")
            onTried: screenDemo.begin()
        }

        DemoCard {
            id: card
            kind: "triggers"
            body: qsTr("A trigger makes your pets do something by themselves: when you press a key, at a time of day, when an application opens. Try it to make an example key that plays an example timeline with your own pets.")
            onTried: demo.run()
        }

        QQC2.Switch {
            id: masterSwitch
            Layout.fillWidth: true
            enabled: Daemon.automationSupported
            text: qsTr("Triggers are on")
            checked: Daemon.settings.automationEnabled ?? true
            onToggled: Daemon.setSettings({automationEnabled: checked})
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: qsTr("Off: nothing fires, every timeline stops and nothing is watched.")
        }

        // The rescue pauses the triggers (a loop of them must not win against it).
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: (Daemon.triggerStates.suspendedMs ?? 0) > 0
            type: Kirigami.MessageType.Warning
            text: qsTr("The triggers are paused for a little while because the pets were rescued.")
            actions: Kirigami.Action {
                text: qsTr("Resume now")
                icon.name: "media-playback-start"
                onTriggered: Daemon.resumeAutomation()
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("A trigger only acts on the pets: it never runs a program or opens a file or a web address. It reads the open windows' classes (not their titles), the power and lock state, the clock and the keys you give it. It looks at the screen only for a screen trigger, and only after you press Watch the screen and answer the desktop's question.")
        }

        Repeater {
            model: Daemon.triggers

            delegate: QQC2.ItemDelegate {
                id: row

                required property var modelData
                readonly property var lastState: page.stateOf(modelData.id)
                readonly property bool imported: (modelData.origin ?? "") !== ""

                Layout.fillWidth: true

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    QQC2.Switch {
                        checked: row.modelData.enabled
                        onToggled: Daemon.setTriggerEnabled(row.modelData.id, checked)
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: checked ? qsTr("On") : qsTr("Off")
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
                            text: qsTr("When %1: %2").arg(page.sourceText(row.modelData)).arg(page.actionText(row.modelData))
                        }
                        // Screen triggers while the screen is watched: how well the best region matches now, and the level at which it counts.
                        QQC2.ProgressBar {
                            id: scoreBar
                            readonly property var scores: row.lastState.screen?.score ?? []
                            readonly property real best: Math.max(0, ...scores)
                            Layout.fillWidth: true
                            visible: row.modelData.source?.kind === "screen" && scores.length > 0
                            from: 0
                            to: 1000
                            value: best
                            QQC2.ToolTip.visible: scoreHover.containsMouse
                            QQC2.ToolTip.text: qsTr("Best match now: %1%").arg(Math.round(best / 10))
                            Accessible.name: QQC2.ToolTip.text
                            MouseArea { id: scoreHover; anchors.fill: parent; hoverEnabled: true }
                            Rectangle {
                                // where the trigger's threshold falls
                                visible: parent.visible
                                x: scoreBar.leftPadding + scoreBar.availableWidth * Math.min(1, (row.modelData.source?.data?.match?.threshold ?? 0.65))
                                width: 2
                                height: parent.height
                                color: Kirigami.Theme.negativeTextColor
                            }
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            opacity: 0.6
                            font: Kirigami.Theme.smallFont
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            visible: text !== ""
                            text: {
                                const parts = [];
                                if (row.imported && !row.modelData.enabled)
                                    parts.push(qsTr("imported, disabled (from %1)").arg(page.plain(row.modelData.origin)));
                                else if (row.imported)
                                    parts.push(qsTr("from %1").arg(page.plain(row.modelData.origin)));
                                const outcome = page.outcomeText(row.lastState);
                                if (outcome !== "")
                                    parts.push(outcome);
                                return parts.join(" · ");
                            }
                        }
                    }

                    QQC2.Button {
                        text: qsTr("Test")
                        icon.name: "media-playback-start"
                        enabled: row.modelData.enabled && Daemon.automationSupported
                        onClicked: Daemon.testTrigger(row.modelData.id)
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                        QQC2.ToolTip.text: qsTr("Do what it does now, as if it had just happened")
                    }
                    QQC2.ToolButton {
                        icon.name: "document-edit"
                        text: qsTr("Change")
                        display: QQC2.AbstractButton.IconOnly
                        enabled: page.editable(row.modelData)
                        onClicked: row.modelData.source?.kind === "screen" ? screenDialog.openFor(row.modelData) : dialog.openFor(row.modelData)
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: page.editable(row.modelData) ? text : qsTr("This one is made with murasakictl or by an import")
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
            visible: (Daemon.triggers ?? []).length === 0
            icon.name: Daemon.automationSupported ? "preferences-system-time" : "dialog-warning"
            text: Daemon.automationSupported ? qsTr("No triggers yet") : qsTr("Triggers need a newer Murasaki service")
            explanation: Daemon.automationSupported
                ? qsTr("A trigger makes the pets do something when an application gets the focus, at a time of day, when the laptop goes on battery, or when you press a key.")
                : qsTr("Update and restart the Murasaki service.")
            helpfulAction: Daemon.automationSupported ? newAction : null
        }
    }
}
