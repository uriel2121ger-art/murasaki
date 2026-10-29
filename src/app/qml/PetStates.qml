import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// The states of one pet and its Mouse Tracking (API revision 19, docs/pets/states.md): what it shows when it is at rest, held, carried or just dropped, and whether
// it turns to the cursor or follows it. Both are off until chosen.
ColumnLayout {
    id: root

    required property string petId
    required property var pet            // the pet's entry of ListPets
    required property bool settled       // no edit of this pet is waiting to be sent
    required property var variantChoices // [{text, value}] the animations of the item
    property var overlayWindow: undefined // the overlay's Windows() entry of the pet

    readonly property bool supported: Daemon.petStatesSupported
    readonly property var petStates: pet?.states ?? ({})
    readonly property var tracking: pet?.tracking ?? ({})
    readonly property string trackingMode: tracking.mode ?? "off"
    readonly property var degraded: overlayWindow?.degraded ?? []
    readonly property bool dodgeOrFade: (pet?.smartDodge ?? false) || (pet?.hoverHide ?? false) || (pet?.focusFade ?? false)

    spacing: Kirigami.Units.smallSpacing
    enabled: supported

    function degradedText(name) {
        switch (name) {
        case "states: missing variant": return qsTr("A state names an animation this pet does not have: it shows the one before it.");
        case "states: transcode pending": return qsTr("A copy of an animation of the states is being made; the original plays meanwhile.");
        case "states: switching slow": return qsTr("Changing the animation takes a moment on this computer.");
        case "states: over budget, playing from video": return qsTr("The animations of the states do not all fit the memory kept for pet frames: the one that does not plays from the video.");
        case "tracking: needs the global pointer": return qsTr("This session cannot tell where the cursor is away from the pet, so it does not track it.");
        case "tracking: parked over a fullscreen window": return qsTr("Waiting while a window is fullscreen on its screen.");
        case "tracking: moves limited to 30 a second": return qsTr("It moves at most 30 times a second.");
        default: return "";
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 28
        wrapMode: Text.Wrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: qsTr("What the pet shows when it is at rest, held with the mouse, carried (or stepping away) and just dropped. A state left “As before” shows what the one before it shows: carried, then held, then at rest.")
    }

    Repeater {
        model: [
            {name: "idle", label: qsTr("At rest")},
            {name: "grabbed", label: qsTr("Held with the mouse")},
            {name: "moving", label: qsTr("Carried or stepping away")},
            {name: "released", label: qsTr("Just dropped")}
        ]
        delegate: PetStateRow {
            required property var modelData
            petId: root.petId
            stateName: modelData.name
            label: modelData.label
            entry: root.petStates[modelData.name]
            variantChoices: root.variantChoices
            overlayWindow: root.overlayWindow
        }
    }

    Kirigami.Separator { Layout.fillWidth: true }

    QQC2.Label { text: qsTr("Mouse tracking"); font.bold: true }

    QQC2.ComboBox {
        id: modeBox
        textRole: "text"
        valueRole: "value"
        model: [
            {text: qsTr("Off"), value: "off"},
            {text: qsTr("Turn to face the cursor"), value: "look"},
            {text: qsTr("Follow the cursor"), value: "follow"}
        ]
        onActivated: Daemon.updatePet(root.petId, {tracking: {mode: currentValue}})
        Binding { target: modeBox; property: "currentIndex"; value: modeBox.indexOfValue(root.trackingMode); restoreMode: Binding.RestoreNone }
        Connections { target: Daemon; function onError() { modeBox.currentIndex = modeBox.indexOfValue(root.trackingMode); } }
    }

    RowLayout {
        visible: root.trackingMode === "follow"
        spacing: Kirigami.Units.smallSpacing
        QQC2.Label { text: qsTr("Stay this far away:") }
        QQC2.SpinBox {
            id: distanceBox
            from: 0
            to: 4000
            stepSize: 10
            editable: true
            textFromValue: (value, locale) => qsTr("%1 px").arg(value)
            valueFromText: (text, locale) => parseInt(text) || 0
            onValueModified: Daemon.queuePetUpdate(root.petId, {tracking: {distance: value}})
            Binding { target: distanceBox; property: "value"; value: Math.round(root.tracking.distance ?? 160); when: root.settled && !distanceBox.activeFocus; restoreMode: Binding.RestoreNone }
        }
    }

    RowLayout {
        visible: root.trackingMode === "follow"
        spacing: Kirigami.Units.smallSpacing
        QQC2.Label { text: qsTr("Speed:") }
        QQC2.SpinBox {
            id: speedBox
            from: 1      // tenths of the base speed (about 200 px a second): 0.1 to 4
            to: 40
            stepSize: 1
            editable: true
            textFromValue: (value, locale) => qsTr("%1×").arg(Number(value / 10).toLocaleString(locale, "f", 1))
            valueFromText: (text, locale) => {
                // "1,5" in a locale with a decimal comma, "1.5" in the others
                const parsed = Number.fromLocaleString(locale, text.replace(/[^0-9.,]/g, ""));
                return Math.max(1, Math.min(40, Math.round((isNaN(parsed) ? 1 : parsed) * 10)));
            }
            onValueModified: Daemon.queuePetUpdate(root.petId, {tracking: {speed: value / 10}})
            Binding { target: speedBox; property: "value"; value: Math.round((root.tracking.speed ?? 1) * 10); when: root.settled && !speedBox.activeFocus; restoreMode: Binding.RestoreNone }
        }
    }

    QQC2.Switch {
        id: returnSwitch
        visible: root.trackingMode === "follow"
        text: qsTr("Go back to its place when it stops following")
        onToggled: Daemon.updatePet(root.petId, {tracking: {returnHome: checked}})
        Binding { target: returnSwitch; property: "checked"; value: root.tracking.returnHome ?? true; restoreMode: Binding.RestoreNone }
        Connections { target: Daemon; function onError() { returnSwitch.checked = root.tracking.returnHome ?? true; } }
    }

    QQC2.Label {
        visible: root.trackingMode === "follow" && root.dodgeOrFade
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 28
        wrapMode: Text.Wrap
        color: Kirigami.Theme.negativeTextColor
        text: qsTr("A pet that follows the cursor cannot also run from it or hide under it: turn those off first.")
    }

    RowLayout {
        spacing: Kirigami.Units.smallSpacing
        visible: Daemon.demosSupported && Daemon.automationSupported

        QQC2.Button {
            icon.name: "media-playback-start"
            text: qsTr("Try it")
            onClicked: demo.run()
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: qsTr("This pet follows the cursor for 20 seconds and then goes back. Nothing is saved.")
        }
        QQC2.Button {
            icon.name: "edit-clear-all"
            text: qsTr("Remove examples")
            visible: Daemon.hasDemos
            onClicked: demoRemoveDialog.open()
        }
    }

    DemoMessage {
        id: demoMessage
        Layout.maximumWidth: Kirigami.Units.gridUnit * 28
    }

    DemoSupport {
        id: demo
        kind: "tracking"
        petId: root.petId
        onDone: result => demoMessage.say(result.mode === "look"
            ? qsTr("For %n second(s) this pet turns to face the cursor (it is locked or fills the screen, so it cannot walk): move the mouse around. Nothing was saved. This needs the compositor to tell where the cursor is.", "", result.seconds ?? 20)
            : qsTr("For %n second(s) this pet follows the cursor: move the mouse around. Then it goes back to its place. Nothing was saved. This needs the compositor to tell where the cursor is.", "", result.seconds ?? 20))
        onNeedsPet: demoMessage.missingPet()
    }

    DemoRemoveDialog { id: demoRemoveDialog }

    QQC2.Label {
        visible: root.trackingMode !== "off"
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 28
        wrapMode: Text.Wrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: qsTr("Needs the compositor to tell where the cursor is. It stops when the pets are hidden, paused or being arranged, and the rescue turns it off.")
    }

    Repeater {
        model: root.degraded
        delegate: QQC2.Label {
            required property var modelData
            visible: text !== ""
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 28
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            text: root.degradedText(String(modelData))
        }
    }

    QQC2.Label {
        visible: !root.supported
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: qsTr("The running daemon is too old for this; restart it after updating.")
    }
}
