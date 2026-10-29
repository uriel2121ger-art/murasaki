import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// A small editor of a timeline (docs/pets/automation.md): tracks, each driving one pet, each a list of actions. The working copy is plain data (`draft`); Save sends it
// to the daemon, which checks every field and says which one is wrong. "Save and play" shows what it does.
KirigamiDialogs.Dialog {
    id: dialog

    property var timeline: null
    property var draft: ({name: "", speed: 1, restore: true, tracks: []})
    property bool playAfter: false
    property bool readOnly: false // an example shown before it plays: the steps are listed, nothing is edited
    property string saveError: ""

    readonly property var actionKinds: [
        {key: "show", text: qsTr("Show")}, {key: "hide", text: qsTr("Hide")}, {key: "place", text: qsTr("Place at")}, {key: "move", text: qsTr("Move to")},
        {key: "sound", text: qsTr("Play its sound")}, {key: "wait", text: qsTr("Wait")}, {key: "stopSounds", text: qsTr("Silence")}
    ].concat(Daemon.demosSupported ? [{key: "track", text: qsTr("Mouse tracking")}] : []) // (an older daemon does not know the action)

    title: readOnly ? qsTr("Example timeline") : timeline ? qsTr("Change the timeline") : qsTr("New timeline")
    standardButtons: KirigamiDialogs.Dialog.NoButton
    preferredWidth: Kirigami.Units.gridUnit * 38
    padding: Kirigami.Units.largeSpacing

    function copy(o) {
        return JSON.parse(JSON.stringify(o));
    }

    function openFor(existing) {
        load(existing, false);
    }

    // An example shown before it plays (read-only: the steps are listed, and Play runs it).
    function openExample(existing) {
        load(existing, true);
    }

    function load(existing, viewOnly) {
        readOnly = viewOnly;
        timeline = existing ?? null;
        draft = existing ? copy({name: existing.name, speed: existing.speed ?? 1, restore: existing.restore ?? true, tracks: existing.tracks ?? []})
                         : {name: "", speed: 1, restore: true, tracks: []};
        saveError = "";
        open();
    }

    function playExample() {
        if (timeline && timeline.id !== undefined)
            Daemon.runTimeline(timeline.id);
        close();
    }

    // The ways a track may name its pet.
    function refKind(ref) {
        return ref.petId !== undefined ? "pet" : (ref.slot !== undefined ? "slot" : "item");
    }

    function touch() {
        draft = copy(draft); // (a change inside the working copy: a new object, so that what shows it is built again)
    }

    function addTrack() {
        const pets = Daemon.pets ?? [];
        draft.tracks.push({pet: pets.length > 0 ? {petId: pets[0].petId} : {slot: 0}, startVisible: true, actions: []});
        touch();
    }

    function addAction(track, kind) {
        const action = {kind: kind};
        if (kind === "place" || kind === "move") {
            action.x = 0;
            action.y = 0;
        }
        if (kind === "move") {
            action.mode = "smooth";
            action.durationMs = 1000;
        }
        if (kind === "wait")
            action.ms = 1000;
        if (kind === "track")
            action.mode = "follow";
        draft.tracks[track].actions.push(action);
        touch();
    }

    function save(play) {
        const changes = {name: nameField.text.trim(), speed: speedBox.value / 100, restore: restoreSwitch.checked, tracks: draft.tracks};
        if (timeline && timeline.origin === "demo")
            changes.origin = ""; // changed by the user: it is theirs now, "Remove examples" leaves it
        playAfter = play;
        saveError = "";
        Daemon.setTimeline(timeline ? timeline.id : "", changes);
    }

    Connections {
        target: Daemon
        function onTimelineSaved(id) {
            if (!dialog.visible)
                return;
            if (dialog.playAfter)
                Daemon.runTimeline(id);
            dialog.close();
        }
        function onError(message) {
            if (dialog.visible)
                dialog.saveError = message;
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: dialog.saveError !== ""
            type: Kirigami.MessageType.Error
            text: String(dialog.saveError).replace(/&/g, "＆").replace(/</g, "‹").replace(/>/g, "›") // (the daemon's message can hold a name from a file)
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: dialog.readOnly
            type: Kirigami.MessageType.Information
            text: qsTr("This example is made from your pets. While it plays it moves and hides them; when it ends they are back where they were, and nothing you have stored changes. Change it from the list if you want to keep it: a changed example is yours and stays when the examples are removed.")
        }

        RowLayout {
            Layout.fillWidth: true
            enabled: !dialog.readOnly
            QQC2.TextField {
                id: nameField
                Layout.fillWidth: true
                maximumLength: 100
                placeholderText: qsTr("Name")
                text: dialog.draft.name
            }
            QQC2.Label { text: qsTr("Speed") }
            QQC2.SpinBox {
                id: speedBox
                from: 25
                to: 400
                stepSize: 25
                value: Math.round((dialog.draft.speed ?? 1) * 100)
                textFromValue: v => (v / 100).toFixed(2) + "×"
                valueFromText: t => Math.round(parseFloat(t) * 100)
            }
            QQC2.Switch {
                id: restoreSwitch
                text: qsTr("Put things back at the end")
                checked: dialog.draft.restore ?? true
            }
        }

        Repeater {
            model: dialog.draft.tracks

            delegate: Kirigami.AbstractCard {
                id: trackCard

                required property var modelData
                required property int index

                Layout.fillWidth: true
                enabled: !dialog.readOnly

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label { text: qsTr("Track %1: the pet").arg(trackCard.index + 1) }
                        QQC2.ComboBox {
                            id: refType
                            model: [qsTr("on the desktop"), qsTr("of an item"), qsTr("of the profile (slot)")]
                            currentIndex: ["pet", "item", "slot"].indexOf(dialog.refKind(trackCard.modelData.pet))
                            onActivated: {
                                const pets = Daemon.pets ?? [];
                                trackCard.modelData.pet = currentIndex === 0 ? {petId: pets.length > 0 ? pets[0].petId : ""} : currentIndex === 1 ? {item: ""} : {slot: 0};
                                dialog.touch();
                            }
                        }
                        QQC2.ComboBox {
                            visible: refType.currentIndex === 0
                            Layout.fillWidth: true
                            model: (Daemon.pets ?? []).map(p => ({id: p.petId, label: String(p.variant).slice(0, 40) + " · " + String(p.itemId).slice(0, 40)}))
                            textRole: "label"
                            valueRole: "id"
                            currentIndex: Math.max(0, (Daemon.pets ?? []).findIndex(p => p.petId === trackCard.modelData.pet.petId))
                            onActivated: trackCard.modelData.pet = {petId: currentValue}
                        }
                        QQC2.TextField {
                            visible: refType.currentIndex === 1
                            Layout.fillWidth: true
                            placeholderText: qsTr("Item id")
                            maximumLength: 200
                            text: trackCard.modelData.pet.item ?? ""
                            onEditingFinished: trackCard.modelData.pet = {item: text, variant: trackCard.modelData.pet.variant}
                        }
                        QQC2.TextField {
                            visible: refType.currentIndex === 1
                            placeholderText: qsTr("Animation")
                            maximumLength: 200
                            text: trackCard.modelData.pet.variant ?? ""
                            onEditingFinished: trackCard.modelData.pet = {item: trackCard.modelData.pet.item, variant: text}
                        }
                        QQC2.SpinBox {
                            visible: refType.currentIndex === 2
                            from: 0
                            to: 1000
                            value: trackCard.modelData.pet.slot ?? 0
                            onValueModified: trackCard.modelData.pet = {slot: value}
                        }
                        QQC2.CheckBox {
                            text: qsTr("starts visible")
                            checked: trackCard.modelData.startVisible ?? true
                            onToggled: trackCard.modelData.startVisible = checked
                        }
                        QQC2.ToolButton {
                            icon.name: "edit-delete"
                            text: qsTr("Remove the track")
                            display: QQC2.AbstractButton.IconOnly
                            onClicked: {
                                dialog.draft.tracks.splice(trackCard.index, 1);
                                dialog.touch();
                            }
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.text: text
                        }
                    }

                    Repeater {
                        model: trackCard.modelData.actions

                        delegate: RowLayout {
                            id: actionRow

                            required property var modelData
                            required property int index

                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            QQC2.ComboBox {
                                id: kindBox
                                model: dialog.actionKinds
                                textRole: "text"
                                valueRole: "key"
                                currentIndex: Math.max(0, dialog.actionKinds.findIndex(k => k.key === actionRow.modelData.kind))
                                onActivated: {
                                    const kind = currentValue;
                                    trackCard.modelData.actions[actionRow.index] = {kind: kind};
                                    dialog.addDefaults(trackCard.modelData.actions[actionRow.index]);
                                    dialog.touch();
                                }
                            }
                            QQC2.SpinBox {
                                visible: ["place", "move"].includes(actionRow.modelData.kind)
                                from: -100000
                                to: 100000
                                editable: true
                                value: actionRow.modelData.x ?? 0
                                onValueModified: actionRow.modelData.x = value
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.text: qsTr("x, pixels from the left of the screen")
                            }
                            QQC2.SpinBox {
                                visible: ["place", "move"].includes(actionRow.modelData.kind)
                                from: -100000
                                to: 100000
                                editable: true
                                value: actionRow.modelData.y ?? 0
                                onValueModified: actionRow.modelData.y = value
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.text: qsTr("y, pixels from the top of the screen")
                            }
                            QQC2.ComboBox {
                                visible: actionRow.modelData.kind === "move"
                                model: [qsTr("jump"), qsTr("glide")]
                                currentIndex: actionRow.modelData.mode === "teleport" ? 0 : 1
                                onActivated: actionRow.modelData.mode = currentIndex === 0 ? "teleport" : "smooth"
                            }
                            QQC2.SpinBox {
                                visible: actionRow.modelData.kind === "move" && actionRow.modelData.mode !== "teleport"
                                from: 0
                                to: 60000
                                stepSize: 100
                                editable: true
                                value: actionRow.modelData.durationMs ?? 0
                                onValueModified: actionRow.modelData.durationMs = value
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.text: qsTr("milliseconds")
                            }
                            QQC2.TextField {
                                visible: actionRow.modelData.kind === "show"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Animation (optional)")
                                maximumLength: 200
                                text: actionRow.modelData.variant ?? ""
                                onEditingFinished: actionRow.modelData.variant = text
                            }
                            QQC2.ComboBox {
                                visible: actionRow.modelData.kind === "track"
                                textRole: "text"
                                valueRole: "value"
                                model: [{text: qsTr("off"), value: "off"}, {text: qsTr("turn to face the cursor"), value: "look"}, {text: qsTr("follow the cursor"), value: "follow"}]
                                currentIndex: Math.max(0, ["off", "look", "follow"].indexOf(actionRow.modelData.mode ?? "follow"))
                                onActivated: actionRow.modelData.mode = currentValue
                            }
                            QQC2.SpinBox {
                                visible: actionRow.modelData.kind === "wait"
                                from: 0
                                to: 3600000
                                stepSize: 100
                                editable: true
                                value: actionRow.modelData.ms ?? 0
                                onValueModified: actionRow.modelData.ms = value
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.text: qsTr("milliseconds")
                            }
                            Item { Layout.fillWidth: true }
                            QQC2.ToolButton {
                                icon.name: "list-remove"
                                text: qsTr("Remove the action")
                                display: QQC2.AbstractButton.IconOnly
                                onClicked: {
                                    trackCard.modelData.actions.splice(actionRow.index, 1);
                                    dialog.touch();
                                }
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.text: text
                            }
                        }
                    }

                    RowLayout {
                        visible: !dialog.readOnly
                        QQC2.Label { text: qsTr("Add:") }
                        Repeater {
                            model: dialog.actionKinds
                            QQC2.Button {
                                required property var modelData
                                flat: true
                                text: modelData.text
                                onClicked: dialog.addAction(trackCard.index, modelData.key)
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Button {
                visible: !dialog.readOnly
                icon.name: "list-add"
                text: qsTr("Add a track")
                enabled: dialog.draft.tracks.length < 16
                onClicked: dialog.addTrack()
            }
            Item { Layout.fillWidth: true }
            QQC2.Button {
                visible: dialog.readOnly
                text: qsTr("Close")
                onClicked: dialog.close()
            }
            QQC2.Button {
                visible: dialog.readOnly
                icon.name: "media-playback-start"
                text: qsTr("Play")
                highlighted: true
                enabled: dialog.timeline !== null && dialog.timeline.id !== undefined
                onClicked: dialog.playExample()
            }
            QQC2.Button {
                visible: !dialog.readOnly
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            QQC2.Button {
                visible: !dialog.readOnly
                icon.name: "media-playback-start"
                text: qsTr("Save and play")
                enabled: nameField.text.trim() !== "" && dialog.draft.tracks.length > 0
                onClicked: dialog.save(true)
            }
            QQC2.Button {
                visible: !dialog.readOnly
                icon.name: "document-save"
                text: qsTr("Save")
                highlighted: true
                enabled: nameField.text.trim() !== ""
                onClicked: dialog.save(false)
            }
        }
    }

    function addDefaults(action) {
        if (action.kind === "place" || action.kind === "move") {
            action.x = 0;
            action.y = 0;
        }
        if (action.kind === "move") {
            action.mode = "smooth";
            action.durationMs = 1000;
        }
        if (action.kind === "wait")
            action.ms = 1000;
        if (action.kind === "track")
            action.mode = "follow";
    }
}
