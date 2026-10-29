import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Makes or changes an automation as one sentence with two slots, "When [condition] then [action]" (spec 2026-10-25, editor A).
// It edits the daemon's triggers (docs/pets/automation.md) without changing them: the same closed lists, nothing runs a program or
// opens a file or a web address. The technical fields (a window class typed by hand, the cooldown, hold, signal files, commands)
// wait under "More options", closed by default.
KirigamiDialogs.Dialog {
    id: dialog

    // The trigger being changed (its JSON), or null for a new one.
    property var trigger: null
    readonly property bool editing: trigger !== null && (trigger.id ?? "") !== ""

    property string kind: "app-open"
    property string actionKey: "hide-all"
    property string appClass: ""
    property string appOn: "open"
    property string outputName: ""
    property var days: []
    property string stateValue: ""
    property string keyId: ""
    property string timelineId: ""
    property string profileId: ""
    property string petId: ""
    // A trigger may name its pets by item instead of petId (docs/pets/automation.md): kept as it is while no pet is chosen.
    property string petItem: ""

    AutomationText { id: words }

    readonly property bool appKind: kind === "app-open" || kind === "app-focus" || kind === "fullscreen"
    readonly property bool technicalKind: kind === "signal-file" || kind === "fire"
    readonly property bool canHold: kind === "shortcut" && ["hide-all", "show-all", "toggle-hidden", "mute-on", "mute-off", "mute-toggle", "timeline"].includes(actionKey)
    readonly property var stateChoices: kind === "power" ? [{key: "battery", text: qsTr("I unplug it")}, {key: "ac", text: qsTr("I plug it in")}]
        : kind === "session" ? [{key: "locked", text: qsTr("I lock it")}, {key: "unlocked", text: qsTr("I unlock it")}]
        : kind === "game" ? [{key: "started", text: qsTr("it starts")}, {key: "ended", text: qsTr("it ends")}]
        : kind === "output" ? [{key: "added", text: qsTr("I plug it in")}, {key: "removed", text: qsTr("I unplug it")}]
        : kind === "idle" && Daemon.idleBackSupported ? [{key: "away", text: qsTr("I leave")}, {key: "back", text: qsTr("I come back")}] : []
    readonly property var appOnChoices: kind === "app-open" ? [{key: "open", text: qsTr("it opens")}, {key: "close", text: qsTr("its last window closes")}]
        : kind === "app-focus" ? [{key: "enter", text: qsTr("I start using it")}, {key: "leave", text: qsTr("I stop using it")}]
        : [{key: "enter", text: qsTr("it starts")}, {key: "leave", text: qsTr("it ends")}]

    // What is missing before it can be saved, or "" (the Save button says so instead of the daemon refusing it later).
    readonly property string missing: {
        if ((kind === "app-open" || kind === "app-focus") && appClass.trim() === "")
            return qsTr("Choose an application.");
        if (kind === "time" && !/^([01][0-9]|2[0-3]):[0-5][0-9]$/.test(atField.text))
            return qsTr("Write a time such as 22:00.");
        if (kind === "shortcut" && keyField.text.trim() === "")
            return qsTr("Name the key.");
        if (technicalKind && technicalField.text.trim() === "")
            return qsTr("Give it a name under More options.");
        if (actionKey === "timeline" && !(Daemon.timelines ?? []).some(t => t.id === timelineId))
            return (Daemon.timelines ?? []).length === 0 ? qsTr("Make a timeline first (Timelines page).") : qsTr("Choose a timeline.");
        if (actionKey === "profile" && !(Daemon.profiles ?? []).some(p => p.id === profileId))
            return (Daemon.profiles ?? []).length === 0 ? qsTr("Save a profile first (Profiles page).") : qsTr("Choose a profile.");
        if ((actionKey === "play-sound" || actionKey === "scene") && petId === "" && petItem === "")
            return qsTr("Choose a pet.");
        return "";
    }
    onMissingChanged: syncSave()
    onOpened: syncSave()
    function syncSave() {
        const save = standardButton(KirigamiDialogs.Dialog.Save);
        if (save)
            save.enabled = missing === "";
    }

    title: editing ? qsTr("Change the automation") : qsTr("New automation")
    standardButtons: KirigamiDialogs.Dialog.Save | KirigamiDialogs.Dialog.Cancel
    preferredWidth: Kirigami.Units.gridUnit * 36
    padding: Kirigami.Units.largeSpacing

    // Opens the editor on a trigger (to change it), on a preset without an id (an idea of the gallery), or empty.
    function openFor(existing) {
        trigger = existing ?? null;
        const t = existing ?? ({});
        const s = t.source ?? ({});
        const a = t.action ?? ({});
        kind = s.kind ?? "app-open";
        appClass = s.app ?? "";
        appOn = s.on ?? (kind === "app-open" ? "open" : "enter");
        outputName = s.output ?? s.name ?? "";
        atField.text = s.at ?? "22:00";
        days = (s.days ?? []).slice();
        minutesBox.value = s.everyMinutes ?? s.minutes ?? 15;
        stateValue = s.state ?? "";
        keyId = s.kind === "shortcut" ? (s.id ?? "") : "";
        keyField.text = s.kind === "shortcut" ? (s.description ?? "") : "";
        technicalField.text = s.kind === "signal-file" || s.kind === "fire" ? (s.name ?? "") : "";
        actionKey = a.type ? words.actionKey(a) : "hide-all";
        timelineId = a.type === "timeline" ? (a.id ?? "") : "";
        profileId = a.type === "profile" ? (a.id ?? "") : "";
        petId = a.petId ?? "";
        petItem = a.item ?? "";
        sceneBox.value = (a.index ?? 0) + 1;
        nameField.text = t.name ?? "";
        cooldownBox.value = t.cooldownMs ?? (Daemon.settings.triggerCooldownMs ?? 500);
        holdBox.checked = (t.mode ?? "press") === "hold";
        enabledBox.checked = t.enabled ?? true;
        moreOptions.checked = false;
        open();
    }

    function chooseKind(newKind) {
        if (newKind === kind)
            return;
        kind = newKind;
        appOn = kind === "app-open" ? "open" : "enter";
        stateValue = stateChoices.length > 0 ? stateChoices[0].key : "";
    }

    function buildSource() {
        const source = {kind: kind};
        if (appKind) {
            if (appClass.trim() !== "")
                source.app = appClass.trim();
            source.on = appOnChoices.some(c => c.key === appOn) ? appOn : appOnChoices[0].key;
            if (kind === "fullscreen" && outputName.trim() !== "")
                source.output = outputName.trim();
        } else if (kind === "time") {
            source.at = atField.text.trim();
            source.days = days.slice().sort();
        } else if (kind === "interval") {
            source.everyMinutes = minutesBox.value;
        } else if (kind === "idle") {
            source.minutes = minutesBox.value;
            if (stateValue === "back" && Daemon.idleBackSupported)
                source.state = "back"; // (away is the default)
        } else if (stateChoices.length > 0) {
            source.state = stateChoices.some(c => c.key === stateValue) ? stateValue : stateChoices[0].key;
            if (kind === "output" && outputName.trim() !== "")
                source.name = outputName.trim();
        } else if (kind === "shortcut") {
            // the key is named once, from what the user calls it; the desktop's shortcut settings bind it
            source.id = keyId !== "" ? keyId : "key-" + Date.now().toString(36);
            if (keyField.text.trim() !== "")
                source.description = keyField.text.trim().slice(0, 200);
        } else if (technicalKind) {
            source.name = technicalField.text.trim();
        }
        return source;
    }

    function buildAction() {
        const key = actionKey;
        if (key.startsWith("mute-"))
            return {type: "mute", mode: key.slice(5)};
        const action = {type: key};
        if (key === "timeline")
            action.id = timelineId;
        else if (key === "profile")
            action.id = profileId;
        else if (key === "play-sound" || key === "scene") {
            if (petId !== "")
                action.petId = petId;
            else
                action.item = petItem;
            if (key === "scene")
                action.index = sceneBox.value - 1;
        }
        return action;
    }

    // The sentence as it reads now: the slots' text and the default name.
    readonly property string sentence: words.sentence({source: buildSource(), action: buildAction()})

    function buildChanges() {
        const source = buildSource();
        if (source.kind === "shortcut")
            keyId = source.id; // the same id on a second build
        const name = nameField.text.trim() !== "" ? nameField.text.trim() : words.sentence({source: source, action: buildAction()});
        return {name: name.slice(0, 100), enabled: enabledBox.checked, cooldownMs: cooldownBox.value,
                mode: canHold && holdBox.checked ? "hold" : "press", source: source, action: buildAction()};
    }

    onAccepted: {
        const changes = buildChanges();
        if (editing && trigger.origin === "demo")
            changes.origin = ""; // changed by the user: it is theirs now, "Remove examples" leaves it
        Daemon.setTrigger(editing ? trigger.id : "", changes);
    }

    // A coloured slot of the sentence: what is chosen, with its icon; a click opens the list to choose from.
    component Slot: QQC2.Button {
        id: slot
        property string iconName: ""
        property string label: ""
        Accessible.name: label
        padding: Kirigami.Units.smallSpacing
        leftPadding: Kirigami.Units.largeSpacing
        rightPadding: Kirigami.Units.largeSpacing
        // Drawn here: the desktop style does not fill a highlighted button, and the slot must read as one coloured piece.
        background: Rectangle {
            radius: Kirigami.Units.cornerRadius
            color: slot.down ? Qt.darker(Kirigami.Theme.highlightColor, 1.2)
                 : slot.hovered ? Qt.lighter(Kirigami.Theme.highlightColor, 1.1) : Kirigami.Theme.highlightColor
            border.width: slot.visualFocus ? 2 : 0
            border.color: Kirigami.Theme.focusColor
        }
        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                source: slot.iconName
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                selected: true
            }
            QQC2.Label {
                text: slot.label
                textFormat: Text.PlainText
                elide: Text.ElideRight
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                color: Kirigami.Theme.highlightedTextColor
            }
            Kirigami.Icon {
                source: "arrow-down"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                selected: true
            }
        }
    }

    // Which of a list of {id, label} is selected, by id.
    component IdBox: QQC2.ComboBox {
        property string selected: ""
        property var entries: []
        signal chosen(string id)
        id: idBox
        model: entries
        textRole: "label"
        valueRole: "id"
        // Names come from files: the labels lose their angle brackets (words.plain) and the list shows plain text.
        delegate: QQC2.ItemDelegate {
            required property var modelData
            width: ListView.view ? ListView.view.width : implicitWidth
            highlighted: idBox.highlightedIndex === index
            required property int index
            contentItem: QQC2.Label {
                text: modelData.label
                textFormat: Text.PlainText
                elide: Text.ElideRight
            }
        }
        currentIndex: entries.findIndex(e => e.id === selected)
        displayText: currentIndex < 0 ? qsTr("Choose…") : currentText
        onActivated: chosen(currentValue ?? "")
    }

    QQC2.Menu {
        id: conditionMenu
        Instantiator {
            model: words.conditions
            delegate: QQC2.MenuItem {
                required property var modelData
                text: modelData.text
                icon.name: modelData.icon
                checkable: true
                checked: dialog.kind === modelData.kind
                onTriggered: dialog.chooseKind(modelData.kind)
            }
            onObjectAdded: (index, object) => conditionMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => conditionMenu.removeItem(object)
        }
    }

    QQC2.Menu {
        id: actionMenu
        Instantiator {
            model: words.actions
            delegate: QQC2.MenuItem {
                required property var modelData
                text: modelData.text
                icon.name: modelData.icon
                checkable: true
                checked: dialog.actionKey === modelData.key
                onTriggered: dialog.actionKey = modelData.key
            }
            onObjectAdded: (index, object) => actionMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => actionMenu.removeItem(object)
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // The sentence. A fixed width: a Flow that takes the dialog's width makes the dialog's height and place loop.
        Flow {
            Layout.preferredWidth: Kirigami.Units.gridUnit * 33
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                height: whenSlot.height
                verticalAlignment: Text.AlignVCenter
                text: qsTr("When")
                font.bold: true
            }
            Slot {
                id: whenSlot
                iconName: words.conditionOf(dialog.kind).icon
                label: words.whenText(dialog.buildSource())
                onClicked: conditionMenu.popup(whenSlot, 0, whenSlot.height)
            }
            QQC2.Label {
                height: whenSlot.height
                verticalAlignment: Text.AlignVCenter
                text: qsTr("then")
                font.bold: true
            }
            Slot {
                id: thenSlot
                iconName: words.actionOf(dialog.buildAction()).icon
                label: words.thenText(dialog.buildAction())
                onClicked: actionMenu.popup(thenSlot, 0, thenSlot.height)
            }
        }

        // The details of both slots: only what the chosen condition and action need.
        Kirigami.FormLayout {
            Layout.fillWidth: true

            RowLayout {
                visible: dialog.appKind
                Kirigami.FormData.label: qsTr("Application:")
                AppPicker {
                    id: appPicker
                    appClass: dialog.appClass
                    placeholder: dialog.kind === "fullscreen" ? qsTr("Any application") : qsTr("Choose an application…")
                    onPicked: cls => dialog.appClass = cls
                }
                // Fullscreen works without an application: back to any.
                QQC2.ToolButton {
                    visible: dialog.kind === "fullscreen" && dialog.appClass !== ""
                    icon.name: "edit-clear"
                    text: qsTr("Any application")
                    display: QQC2.AbstractButton.IconOnly
                    onClicked: dialog.appClass = ""
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: text
                }
            }
            QQC2.ComboBox {
                visible: dialog.appKind
                Kirigami.FormData.label: qsTr("When:")
                model: dialog.appOnChoices
                textRole: "text"
                valueRole: "key"
                currentIndex: Math.max(0, dialog.appOnChoices.findIndex(c => c.key === dialog.appOn))
                onActivated: dialog.appOn = currentValue
            }

            QQC2.TextField {
                id: atField
                visible: dialog.kind === "time"
                Kirigami.FormData.label: qsTr("Time:")
                inputMask: "99:99"
                text: "22:00"
            }
            RowLayout {
                visible: dialog.kind === "time"
                Kirigami.FormData.label: qsTr("Days:")
                Repeater {
                    model: 7
                    QQC2.CheckBox {
                        required property int index
                        text: words.dayNames[index]
                        checked: dialog.days.includes(index)
                        onToggled: dialog.days = checked ? dialog.days.concat([index]) : dialog.days.filter(d => d !== index)
                    }
                }
            }
            Hint {
                visible: dialog.kind === "time"
                text: qsTr("No day marked means every day.")
            }

            QQC2.SpinBox {
                id: minutesBox
                visible: dialog.kind === "interval" || dialog.kind === "idle"
                Kirigami.FormData.label: dialog.kind !== "idle" ? qsTr("Every (minutes):")
                    : dialog.stateValue === "back" ? qsTr("Away for at least (minutes):") : qsTr("Away for (minutes):")
                from: 1
                to: 1440
                value: 15
                editable: true
            }

            QQC2.ComboBox {
                visible: dialog.stateChoices.length > 0
                Kirigami.FormData.label: qsTr("When:")
                model: dialog.stateChoices
                textRole: "text"
                valueRole: "key"
                currentIndex: Math.max(0, dialog.stateChoices.findIndex(c => c.key === dialog.stateValue))
                onActivated: dialog.stateValue = currentValue
            }

            QQC2.TextField {
                id: keyField
                visible: dialog.kind === "shortcut"
                Kirigami.FormData.label: qsTr("Name of the key:")
                Layout.fillWidth: true
                maximumLength: 200
                placeholderText: qsTr("For example: Hide my pets")
            }
            Hint {
                visible: dialog.kind === "shortcut"
                text: qsTr("You choose the key in your desktop's own shortcut settings (System Settings, Shortcuts, Murasaki); Murasaki never sees other keys. Mouse buttons and gamepads are not possible.")
            }

            IdBox {
                visible: dialog.actionKey === "timeline"
                Kirigami.FormData.label: qsTr("Timeline:")
                Layout.fillWidth: true
                entries: (Daemon.timelines ?? []).map(t => ({id: t.id, label: words.plain(t.name)}))
                selected: dialog.timelineId
                onChosen: id => dialog.timelineId = id
            }
            IdBox {
                visible: dialog.actionKey === "profile"
                Kirigami.FormData.label: qsTr("Profile:")
                Layout.fillWidth: true
                entries: (Daemon.profiles ?? []).map(p => ({id: p.id, label: words.plain(p.name)}))
                selected: dialog.profileId
                onChosen: id => dialog.profileId = id
            }
            IdBox {
                visible: dialog.actionKey === "play-sound" || dialog.actionKey === "scene"
                Kirigami.FormData.label: qsTr("Pet:")
                Layout.fillWidth: true
                entries: (Daemon.pets ?? []).map(p => ({id: p.petId, label: words.plain(p.variant).slice(0, 40) + " · " + words.plain(p.itemId).slice(0, 40)}))
                selected: dialog.petId
                onChosen: id => dialog.petId = id
                displayText: currentIndex >= 0 ? currentText : (dialog.petItem !== "" ? words.plain(dialog.petItem) : qsTr("Choose…"))
            }
            QQC2.SpinBox {
                id: sceneBox
                visible: dialog.actionKey === "scene"
                Kirigami.FormData.label: qsTr("Scene number:")
                from: 1
                to: 1000
            }

            QQC2.TextField {
                id: nameField
                Kirigami.FormData.label: qsTr("Name:")
                Layout.fillWidth: true
                maximumLength: 100
                placeholderText: dialog.sentence
            }
        }

        // Why Save is not available yet.
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.missing !== ""
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                source: "dialog-information"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: dialog.missing
                wrapMode: Text.Wrap
            }
        }

        QQC2.Button {
            id: moreOptions
            flat: true
            checkable: true
            checked: false
            icon.name: checked ? "arrow-down" : "arrow-right"
            text: qsTr("More options")
        }

        // The technical part, for those who look for it.
        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: moreOptions.checked

            QQC2.TextField {
                visible: dialog.appKind
                Kirigami.FormData.label: qsTr("Window class:")
                Layout.fillWidth: true
                maximumLength: 128
                text: dialog.appClass
                placeholderText: qsTr("A window class such as firefox (* and ? match)")
                onTextEdited: dialog.appClass = text
            }
            QQC2.TextField {
                visible: dialog.kind === "fullscreen" || dialog.kind === "output"
                Kirigami.FormData.label: qsTr("Screen:")
                Layout.fillWidth: true
                maximumLength: 128
                text: dialog.outputName
                placeholderText: qsTr("Any, or a name such as DP-1")
                onTextEdited: dialog.outputName = text
            }
            RowLayout {
                Kirigami.FormData.label: qsTr("For scripts:")
                Repeater {
                    model: words.technicalConditions
                    QQC2.Button {
                        required property var modelData
                        text: modelData.text
                        icon.name: modelData.icon
                        checkable: true
                        checked: dialog.kind === modelData.kind
                        onClicked: dialog.chooseKind(modelData.kind)
                    }
                }
            }
            QQC2.TextField {
                id: technicalField
                visible: dialog.technicalKind
                Kirigami.FormData.label: qsTr("Name:")
                Layout.fillWidth: true
                maximumLength: 64
                validator: RegularExpressionValidator { regularExpression: /[A-Za-z0-9._-]*/ }
                placeholderText: qsTr("Letters, digits, dot, dash and underscore")
            }
            Hint {
                visible: dialog.kind === "signal-file"
                text: qsTr("Touching the file %1 in the runtime folder fires it; the contents are never read.").arg("murasaki/triggers/" + (technicalField.text || "NAME"))
            }
            Hint {
                visible: dialog.kind === "fire"
                text: qsTr("Fires when something you run calls: murasakictl triggers fire %1").arg(technicalField.text || "NAME")
            }
            QQC2.SpinBox {
                id: cooldownBox
                Kirigami.FormData.label: qsTr("At most once every (ms):")
                from: 100
                to: 86400000
                stepSize: 100
                value: 500
                editable: true
            }
            QQC2.CheckBox {
                id: holdBox
                visible: dialog.canHold
                text: qsTr("Hold: undo it when the key is released")
            }
            QQC2.Switch {
                id: enabledBox
                Kirigami.FormData.label: qsTr("Enabled:")
                checked: true
            }
        }
    }
}
