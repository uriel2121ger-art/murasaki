import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// What one pet shows in one of its states (docs/pets/states.md): like the one before it, one animation of its item, or a sequence of them. The entry is the pet's
// `states.<name>` of ListPets; every change is sent as that state's whole entry (the daemon replaces a state whole and keeps the others).
ColumnLayout {
    id: root

    required property string petId
    required property string stateName   // idle, grabbed, moving or released
    required property string label
    required property var entry          // states.<name> of the pet, or undefined
    required property var variantChoices // [{text, value}] the animations of the item
    property var overlayWindow: undefined // the overlay's Windows() entry of the pet

    readonly property string kind: entry === undefined || entry === null ? "none" : entry.variant !== undefined ? "variant" : "sequence"
    readonly property var sequence: entry?.sequence ?? ({})
    readonly property var items: sequence.items ?? []
    readonly property int maxItems: 16

    spacing: Kirigami.Units.smallSpacing

    function send(value) {
        const change = {};
        change[stateName] = value;
        Daemon.updatePet(petId, {states: change});
    }
    function firstVariant() {
        return variantChoices.length > 0 ? variantChoices[0].value : "";
    }
    // The sequence with some of its fields changed, as the daemon takes it.
    function sendSequence(changes) {
        const next = {
            items: items.slice(),
            order: sequence.order ?? "linear",
            advance: sequence.advance ?? "auto",
            once: sequence.once ?? false,
            holdMs: sequence.holdMs ?? 4000
        };
        for (const key in changes)
            next[key] = changes[key];
        send({sequence: next});
    }

    RowLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Label {
            Layout.preferredWidth: Kirigami.Units.gridUnit * 11
            text: root.label
            elide: Text.ElideRight
        }
        QQC2.ComboBox {
            id: kindBox
            textRole: "text"
            valueRole: "value"
            model: [
                {text: qsTr("As before"), value: "none"},
                {text: qsTr("One animation"), value: "variant"},
                {text: qsTr("A sequence"), value: "sequence"}
            ]
            onActivated: {
                if (currentValue === "none")
                    root.send(null);
                else if (currentValue === "variant")
                    root.send({variant: root.firstVariant()});
                else
                    root.send({sequence: {items: [root.firstVariant()]}});
            }
            Binding { target: kindBox; property: "currentIndex"; value: kindBox.indexOfValue(root.kind); restoreMode: Binding.RestoreNone }
            Connections { target: Daemon; function onError() { kindBox.currentIndex = kindBox.indexOfValue(root.kind); } }
        }
        QQC2.Button {
            icon.name: "media-playback-start"
            text: qsTr("Try")
            readonly property bool shown: overlayWindow?.shown ?? false
            enabled: Overlay.available && shown
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: !Overlay.available ? qsTr("The desktop pets are not running. Show the pet on the desktop first.")
                : !shown ? qsTr("The pet is not on the desktop right now (hidden, on a screen that is not connected, or its file does not play).")
                : qsTr("Shows this state on the desktop for a few seconds.")
            onClicked: Overlay.previewPetState(root.petId, root.stateName)
        }
    }

    QQC2.ComboBox {
        id: variantBox
        Layout.leftMargin: Kirigami.Units.gridUnit * 11 + Kirigami.Units.smallSpacing
        visible: root.kind === "variant"
        textRole: "text"
        valueRole: "value"
        model: root.variantChoices
        onActivated: root.send({variant: currentValue})
        Binding { target: variantBox; property: "currentIndex"; value: variantBox.indexOfValue(root.entry?.variant ?? ""); restoreMode: Binding.RestoreNone }
        Connections { target: Daemon; function onError() { variantBox.currentIndex = variantBox.indexOfValue(root.entry?.variant ?? ""); } }
    }

    ColumnLayout {
        visible: root.kind === "sequence"
        Layout.leftMargin: Kirigami.Units.gridUnit * 11 + Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        Repeater {
            model: root.items
            delegate: RowLayout {
                id: itemRow
                required property int index
                required property string modelData
                spacing: Kirigami.Units.smallSpacing

                QQC2.ComboBox {
                    id: itemBox
                    textRole: "text"
                    valueRole: "value"
                    model: root.variantChoices
                    onActivated: {
                        const next = root.items.slice();
                        next[itemRow.index] = currentValue;
                        root.sendSequence({items: next});
                    }
                    Binding { target: itemBox; property: "currentIndex"; value: itemBox.indexOfValue(itemRow.modelData); restoreMode: Binding.RestoreNone }
                    Connections { target: Daemon; function onError() { itemBox.currentIndex = itemBox.indexOfValue(itemRow.modelData); } }
                }
                QQC2.ToolButton {
                    icon.name: "list-remove"
                    enabled: root.items.length > 1
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: qsTr("Remove from the sequence")
                    onClicked: {
                        const next = root.items.slice();
                        next.splice(itemRow.index, 1);
                        root.sendSequence({items: next});
                    }
                }
            }
        }

        QQC2.Button {
            icon.name: "list-add"
            text: qsTr("Add an animation")
            enabled: root.items.length < root.maxItems
            onClicked: root.sendSequence({items: root.items.concat([root.firstVariant()])})
        }

        QQC2.ComboBox {
            id: orderBox
            textRole: "text"
            valueRole: "value"
            model: [{text: qsTr("In order, then again"), value: "linear"}, {text: qsTr("In random order"), value: "random"}]
            onActivated: root.sendSequence({order: currentValue})
            Binding { target: orderBox; property: "currentIndex"; value: orderBox.indexOfValue(root.sequence.order ?? "linear"); restoreMode: Binding.RestoreNone }
            Connections { target: Daemon; function onError() { orderBox.currentIndex = orderBox.indexOfValue(root.sequence.order ?? "linear"); } }
        }
        QQC2.ComboBox {
            id: advanceBox
            textRole: "text"
            valueRole: "value"
            model: [{text: qsTr("Moves on by itself"), value: "auto"}, {text: qsTr("Moves on when the pet is clicked"), value: "click"}]
            onActivated: root.sendSequence({advance: currentValue})
            Binding { target: advanceBox; property: "currentIndex"; value: advanceBox.indexOfValue(root.sequence.advance ?? "auto"); restoreMode: Binding.RestoreNone }
            Connections { target: Daemon; function onError() { advanceBox.currentIndex = advanceBox.indexOfValue(root.sequence.advance ?? "auto"); } }
        }
        QQC2.Switch {
            id: onceSwitch
            text: qsTr("Play each one once and stop on its last frame")
            onToggled: root.sendSequence({once: checked})
            Binding { target: onceSwitch; property: "checked"; value: root.sequence.once ?? false; restoreMode: Binding.RestoreNone }
            Connections { target: Daemon; function onError() { onceSwitch.checked = root.sequence.once ?? false; } }
        }
        RowLayout {
            visible: (root.sequence.advance ?? "auto") === "auto"
            spacing: Kirigami.Units.smallSpacing
            QQC2.Label { text: qsTr("Time on each one:") }
            QQC2.SpinBox {
                id: holdBox
                from: 500
                to: 60000
                stepSize: 500
                editable: true
                textFromValue: (value, locale) => qsTr("%1 s").arg(Number(value / 1000).toLocaleString(locale, "f", 1))
                valueFromText: (text, locale) => {
                    // "4,5" in a locale with a decimal comma, "4.5" in the others
                    const parsed = Number.fromLocaleString(locale, text.replace(/[^0-9.,]/g, ""));
                    return Math.round((isNaN(parsed) ? 4 : parsed) * 1000);
                }
                onValueModified: root.sendSequence({holdMs: value})
                Binding { target: holdBox; property: "value"; value: root.sequence.holdMs ?? 4000; when: !holdBox.activeFocus; restoreMode: Binding.RestoreNone }
            }
        }
    }
}
