import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Makes or changes a trigger (docs/pets/automation.md): something that happens, and what the pets do about it. The sources and the actions are closed lists: none of
// them can run a program or open a file or a web address. A trigger made by an import is disabled until the user switches it on.
KirigamiDialogs.Dialog {
    id: dialog

    // The trigger being changed (its JSON), or null for a new one.
    property var trigger: null
    readonly property bool editing: trigger !== null

    readonly property var kinds: [
        {key: "app-focus", text: qsTr("An application gets the focus")},
        {key: "app-open", text: qsTr("An application opens")},
        {key: "fullscreen", text: qsTr("A window goes fullscreen")},
        {key: "time", text: qsTr("At a time of day")},
        {key: "interval", text: qsTr("Every few minutes")},
        {key: "idle", text: qsTr("The session is idle")},
        {key: "power", text: qsTr("The power source changes")},
        {key: "session", text: qsTr("The session is locked or unlocked")},
        {key: "game", text: qsTr("A game starts or ends")},
        {key: "output", text: qsTr("A screen is plugged or unplugged")},
        {key: "shortcut", text: qsTr("A key is pressed")},
        {key: "signal-file", text: qsTr("A signal file is touched")},
        {key: "fire", text: qsTr("Called with murasakictl triggers fire")}
    ]
    readonly property var actionTypes: [
        {key: "timeline", text: qsTr("Play a timeline")},
        {key: "profile", text: qsTr("Switch to a profile")},
        {key: "hide-all", text: qsTr("Hide the pets")},
        {key: "show-all", text: qsTr("Show the pets")},
        {key: "toggle-hidden", text: qsTr("Hide or show the pets")},
        {key: "stop-sounds", text: qsTr("Silence the pets")},
        {key: "mute", text: qsTr("Mute or unmute the pets")},
        {key: "play-sound", text: qsTr("Play a pet's sound")},
        {key: "scene", text: qsTr("Start a scene of a package")}
    ]
    readonly property string kind: kindBox.currentValue ?? "fire"
    readonly property string actionType: actionBox.currentValue ?? "hide-all"
    readonly property bool windowKind: kind === "app-focus" || kind === "app-open" || kind === "fullscreen"
    readonly property bool canHold: kind === "shortcut" && ["hide-all", "show-all", "toggle-hidden", "mute", "timeline"].includes(actionType)

    title: editing ? qsTr("Change the trigger") : qsTr("New trigger")
    standardButtons: KirigamiDialogs.Dialog.Save | KirigamiDialogs.Dialog.Cancel
    preferredWidth: Kirigami.Units.gridUnit * 32
    padding: Kirigami.Units.largeSpacing

    function indexOfKey(list, key) {
        const i = list.findIndex(e => e.key === key);
        return i < 0 ? 0 : i;
    }

    function openFor(existing) {
        trigger = existing ?? null;
        const t = existing ?? ({});
        const s = t.source ?? ({});
        const a = t.action ?? ({});
        nameField.text = t.name ?? "";
        kindBox.currentIndex = indexOfKey(kinds, s.kind ?? "fire");
        appField.text = s.app ?? "";
        leaveBox.currentIndex = (s.on === "leave" || s.on === "close") ? 1 : 0;
        outputField.text = s.output ?? s.name ?? "";
        atField.text = s.at ?? "22:00";
        for (let d = 0; d < 7; ++d)
            dayBoxes.itemAt(d).checked = (s.days ?? []).includes(d);
        minutesBox.value = s.everyMinutes ?? s.minutes ?? 15;
        stateBox.stateValue = s.state ?? "";
        nameKeyField.text = s.kind === "shortcut" ? (s.id ?? "") : (s.kind === "signal-file" || s.kind === "fire" ? (s.name ?? "") : "");
        descriptionField.text = s.description ?? "";
        actionBox.currentIndex = indexOfKey(actionTypes, a.type ?? "hide-all");
        timelineBox.selected = a.id ?? "";
        profileBox.selected = a.id ?? "";
        petBox.selected = a.petId ?? "";
        sceneBox.value = (a.index ?? 0) + 1;
        muteBox.currentIndex = ["on", "off", "toggle"].indexOf(a.mode ?? "toggle");
        cooldownBox.value = t.cooldownMs ?? (Daemon.settings.triggerCooldownMs ?? 500);
        holdBox.checked = (t.mode ?? "press") === "hold";
        enabledBox.checked = t.enabled ?? true;
        open();
    }

    function buildChanges() {
        const source = {kind: kind};
        if (kind === "app-focus" || kind === "app-open" || kind === "fullscreen") {
            if (appField.text.trim() !== "")
                source.app = appField.text.trim();
            source.on = leaveBox.currentIndex === 1 ? (kind === "app-open" ? "close" : "leave") : (kind === "app-open" ? "open" : "enter");
            if (kind === "fullscreen" && outputField.text.trim() !== "")
                source.output = outputField.text.trim();
        } else if (kind === "time") {
            source.at = atField.text.trim();
            const days = [];
            for (let d = 0; d < 7; ++d)
                if (dayBoxes.itemAt(d).checked)
                    days.push(d);
            source.days = days;
        } else if (kind === "interval") {
            source.everyMinutes = minutesBox.value;
        } else if (kind === "idle") {
            source.minutes = minutesBox.value;
        } else if (kind === "power" || kind === "session" || kind === "game" || kind === "output") {
            source.state = stateBox.currentValue;
            if (kind === "output" && outputField.text.trim() !== "")
                source.name = outputField.text.trim();
        } else if (kind === "shortcut") {
            source.id = nameKeyField.text.trim();
            if (descriptionField.text.trim() !== "")
                source.description = descriptionField.text.trim();
        } else {
            source.name = nameKeyField.text.trim();
        }
        const action = {type: actionType};
        if (actionType === "timeline")
            action.id = timelineBox.currentValue ?? "";
        else if (actionType === "profile")
            action.id = profileBox.currentValue ?? "";
        else if (actionType === "mute")
            action.mode = ["on", "off", "toggle"][Math.max(0, muteBox.currentIndex)];
        else if (actionType === "play-sound")
            action.petId = petBox.currentValue ?? "";
        else if (actionType === "scene") {
            action.petId = petBox.currentValue ?? "";
            action.index = sceneBox.value - 1;
        }
        return {name: nameField.text.trim(), enabled: enabledBox.checked, cooldownMs: cooldownBox.value, mode: canHold && holdBox.checked ? "hold" : "press", source: source, action: action};
    }

    onAccepted: {
        const changes = buildChanges();
        if (editing && trigger.origin === "demo")
            changes.origin = ""; // changed by the user: it is theirs now, "Remove examples" leaves it
        Daemon.setTrigger(editing ? trigger.id : "", changes);
    }

    // Which of a list of {id, name} is selected, by id (kept while the list changes under it).
    component IdBox: QQC2.ComboBox {
        property string selected: ""
        property var entries: []
        model: entries
        textRole: "label"
        valueRole: "id"
        onEntriesChanged: currentIndex = Math.max(0, entries.findIndex(e => e.id === selected))
        onSelectedChanged: currentIndex = Math.max(0, entries.findIndex(e => e.id === selected))
        onActivated: selected = currentValue ?? ""
    }

    Kirigami.FormLayout {
        QQC2.TextField {
            id: nameField
            Kirigami.FormData.label: qsTr("Name:")
            Layout.fillWidth: true
            maximumLength: 100
            placeholderText: qsTr("For example: Hide when I play")
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: qsTr("When") }

        QQC2.ComboBox {
            id: kindBox
            Kirigami.FormData.label: qsTr("This happens:")
            Layout.fillWidth: true
            model: dialog.kinds
            textRole: "text"
            valueRole: "key"
        }

        QQC2.TextField {
            id: appField
            visible: dialog.windowKind
            Kirigami.FormData.label: qsTr("Application:")
            Layout.fillWidth: true
            maximumLength: 128
            placeholderText: dialog.kind === "fullscreen" ? qsTr("Any, or a window class such as mpv") : qsTr("A window class such as firefox (* and ? match)")
        }
        QQC2.ComboBox {
            id: leaveBox
            visible: dialog.windowKind
            Kirigami.FormData.label: qsTr("Fire when:")
            model: dialog.kind === "app-open" ? [qsTr("it opens"), qsTr("the last window closes")] : [qsTr("it starts"), qsTr("it stops")]
        }
        QQC2.TextField {
            id: outputField
            visible: dialog.kind === "fullscreen" || dialog.kind === "output"
            Kirigami.FormData.label: qsTr("Screen:")
            Layout.fillWidth: true
            maximumLength: 128
            placeholderText: qsTr("Any, or a name such as DP-1")
        }

        QQC2.TextField {
            id: atField
            visible: dialog.kind === "time"
            Kirigami.FormData.label: qsTr("Time:")
            inputMask: "99:99"
            placeholderText: "22:00"
        }
        RowLayout {
            visible: dialog.kind === "time"
            Kirigami.FormData.label: qsTr("Days:")
            Repeater {
                id: dayBoxes
                model: [qsTr("Sun"), qsTr("Mon"), qsTr("Tue"), qsTr("Wed"), qsTr("Thu"), qsTr("Fri"), qsTr("Sat")]
                QQC2.CheckBox { required property string modelData; text: modelData }
            }
        }
        QQC2.Label {
            visible: dialog.kind === "time"
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("No day marked means every day. A time that passed while the computer slept by more than two minutes is skipped.")
        }

        QQC2.SpinBox {
            id: minutesBox
            visible: dialog.kind === "interval" || dialog.kind === "idle"
            Kirigami.FormData.label: dialog.kind === "idle" ? qsTr("Idle for (minutes):") : qsTr("Every (minutes):")
            from: 1
            to: 1440
            value: 15
        }

        QQC2.ComboBox {
            id: stateBox
            visible: dialog.kind === "power" || dialog.kind === "session" || dialog.kind === "game" || dialog.kind === "output"
            property string stateValue: ""
            readonly property var choices: dialog.kind === "power" ? [{key: "battery", text: qsTr("it runs on battery")}, {key: "ac", text: qsTr("it is plugged in")}]
                : dialog.kind === "session" ? [{key: "locked", text: qsTr("it is locked")}, {key: "unlocked", text: qsTr("it is unlocked")}]
                : dialog.kind === "game" ? [{key: "started", text: qsTr("a game starts")}, {key: "ended", text: qsTr("the game ends")}]
                : [{key: "added", text: qsTr("a screen is plugged")}, {key: "removed", text: qsTr("a screen is unplugged")}]
            Kirigami.FormData.label: qsTr("Fire when:")
            model: choices
            textRole: "text"
            valueRole: "key"
            onStateValueChanged: currentIndex = Math.max(0, choices.findIndex(c => c.key === stateValue))
            onChoicesChanged: currentIndex = Math.max(0, choices.findIndex(c => c.key === stateValue))
        }

        QQC2.TextField {
            id: nameKeyField
            visible: dialog.kind === "shortcut" || dialog.kind === "signal-file" || dialog.kind === "fire"
            Kirigami.FormData.label: dialog.kind === "shortcut" ? qsTr("Shortcut name:") : qsTr("Name:")
            Layout.fillWidth: true
            maximumLength: 64
            validator: RegularExpressionValidator { regularExpression: /[A-Za-z0-9._-]*/ }
            placeholderText: qsTr("Letters, digits, dot, dash and underscore")
        }
        QQC2.TextField {
            id: descriptionField
            visible: dialog.kind === "shortcut"
            Kirigami.FormData.label: qsTr("Description:")
            Layout.fillWidth: true
            maximumLength: 200
        }
        QQC2.Label {
            visible: dialog.kind === "shortcut"
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("You choose the key in your desktop's own shortcut settings (System Settings, Shortcuts, Murasaki); Murasaki never sees other keys. Mouse buttons and gamepads are not possible.")
        }
        QQC2.Label {
            visible: dialog.kind === "signal-file"
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.7
            textFormat: Text.PlainText
            text: qsTr("Touching the file %1 in the runtime folder fires it; the contents are never read.").arg("murasaki/triggers/" + (nameKeyField.text || "NAME"))
        }
        QQC2.Label {
            visible: dialog.kind === "fire"
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.7
            textFormat: Text.PlainText
            text: qsTr("Fires when something you run calls: murasakictl triggers fire %1").arg(nameKeyField.text || "NAME")
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: qsTr("Then") }

        QQC2.ComboBox {
            id: actionBox
            Kirigami.FormData.label: qsTr("The pets:")
            Layout.fillWidth: true
            model: dialog.actionTypes
            textRole: "text"
            valueRole: "key"
        }
        IdBox {
            id: timelineBox
            visible: dialog.actionType === "timeline"
            Kirigami.FormData.label: qsTr("Timeline:")
            Layout.fillWidth: true
            entries: (Daemon.timelines ?? []).map(t => ({id: t.id, label: String(t.name)}))
        }
        IdBox {
            id: profileBox
            visible: dialog.actionType === "profile"
            Kirigami.FormData.label: qsTr("Profile:")
            Layout.fillWidth: true
            entries: (Daemon.profiles ?? []).map(p => ({id: p.id, label: String(p.name)}))
        }
        IdBox {
            id: petBox
            visible: dialog.actionType === "play-sound" || dialog.actionType === "scene"
            Kirigami.FormData.label: qsTr("Pet:")
            Layout.fillWidth: true
            entries: (Daemon.pets ?? []).map(p => ({id: p.petId, label: String(p.variant).slice(0, 40) + " · " + String(p.itemId).slice(0, 40)}))
        }
        QQC2.SpinBox {
            id: sceneBox
            visible: dialog.actionType === "scene"
            Kirigami.FormData.label: qsTr("Scene number:")
            from: 1
            to: 1000
        }
        QQC2.ComboBox {
            id: muteBox
            visible: dialog.actionType === "mute"
            Kirigami.FormData.label: qsTr("Sound:")
            model: [qsTr("mute"), qsTr("unmute"), qsTr("toggle")]
        }
        QQC2.CheckBox {
            id: holdBox
            visible: dialog.canHold
            text: qsTr("Hold: undo it when the key is released")
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true }

        QQC2.SpinBox {
            id: cooldownBox
            Kirigami.FormData.label: qsTr("At most once every (ms):")
            from: 100
            to: 86400000
            stepSize: 100
            value: 500
            editable: true
        }
        QQC2.Switch {
            id: enabledBox
            Kirigami.FormData.label: qsTr("Enabled:")
            checked: true
        }
    }
}
