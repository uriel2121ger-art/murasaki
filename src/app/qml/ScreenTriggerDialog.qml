import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Makes or changes a screen trigger (docs/pets/automation.md "Screen triggers"): "when this picture is on my screen, do this to the pets". The regions are rectangles of the
// desktop; each has reference pictures (captured while the screen shows what should fire it) and the trigger fires while the picture is there. What it does is the same closed list
// of actions as any other trigger: it acts on the pets only. A trigger made by a pack import keeps whatever else the pack brought; only what is shown here is changed.
KirigamiDialogs.Dialog {
    id: dialog

    // The trigger being changed (its JSON), or null for a new one.
    property var trigger: null
    readonly property bool editing: trigger !== null
    // The regions as the record has them (kind, x, y, w, h and whatever else); the rows below edit x, y, w, h of these.
    property var regions: []
    property var extraData: ({})
    property var extraMatch: ({})

    readonly property var actionTypes: [
        {key: "timeline", text: qsTr("Play a timeline")},
        {key: "profile", text: qsTr("Switch to a profile")},
        {key: "hide-all", text: qsTr("Hide the pets")},
        {key: "show-all", text: qsTr("Show the pets")},
        {key: "toggle-hidden", text: qsTr("Hide or show the pets")},
        {key: "stop-sounds", text: qsTr("Silence the pets")},
        {key: "mute", text: qsTr("Mute or unmute the pets")}
    ]
    readonly property string actionType: actionBox.currentValue ?? "timeline"
    readonly property bool canHold: ["hide-all", "show-all", "toggle-hidden", "mute", "timeline"].includes(actionType)
    readonly property string triggerId: trigger?.id ?? ""
    // The score lives under "screen" (while watching); the reference counts ("refs") sit on the state itself.
    readonly property var triggerLive: (Daemon.triggerStates.states ?? []).find(s => s.id === triggerId) ?? ({})
    readonly property var liveState: Object.assign({}, triggerLive.screen ?? ({}), {"refs": triggerLive.refs ?? []})

    // A trigger needs a name and a region to be saved at all; the button says so instead of the daemon refusing after the dialog is gone.
    readonly property bool saveable: nameField.text.trim() !== "" && regions.length > 0
    onSaveableChanged: syncSave()
    onOpened: syncSave()
    function syncSave() {
        const save = standardButton(KirigamiDialogs.Dialog.Save);
        if (save)
            save.enabled = saveable;
    }

    title: editing ? qsTr("Change the screen trigger") : qsTr("New screen trigger")
    standardButtons: KirigamiDialogs.Dialog.Save | KirigamiDialogs.Dialog.Cancel
    preferredWidth: Kirigami.Units.gridUnit * 36
    padding: Kirigami.Units.largeSpacing
    // Kirigami centres on the height, which the popup changes when it shrinks the dialog to fit the window: on a small
    // window that binding loops. The implicit height stays put while the popup does that.
    y: parent ? Math.round((parent.height - implicitHeight) / 2) + Kirigami.Units.gridUnit * 2 * (1 - opacity) : 0

    function indexOfKey(list, key) {
        const i = list.findIndex(e => e.key === key);
        return i < 0 ? 0 : i;
    }

    function setRegion(index, key, value) {
        const next = regions.slice();
        next[index] = Object.assign({}, next[index], {[key]: value});
        regions = next;
    }

    function addRegion(x, y, w, h) {
        if (regions.length >= 8)
            return;
        regions = regions.concat([{kind: "rect", x: x, y: y, w: w, h: h}]);
    }

    function removeRegion(index) {
        regions = regions.filter((r, i) => i !== index);
    }

    function openFor(existing) {
        trigger = existing ?? null;
        const t = existing ?? ({});
        const a = t.action ?? ({});
        const data = t.source?.data ?? ({});
        const m = data.match ?? ({});
        extraData = Object.assign({}, data);
        extraMatch = Object.assign({}, m);
        regions = (data.regions ?? []).slice(0, 8).map(r => Object.assign({}, r));
        nameField.text = t.name ?? "";
        const mode = String(m.mode ?? "hash").toLowerCase();
        modeBox.currentIndex = mode.includes("hash") && !mode.includes("sift") ? 0 : 1;
        thresholdSlider.value = m.threshold ?? 0.65;
        aggBox.currentIndex = indexOfKey(aggs, ["all", "min"].includes(String(m.agg ?? "any")) ? "all" : String(m.agg ?? "any") === "mean" ? "mean" : "any");
        framesBox.value = m.startFramesToCheck ?? 3;
        graceBox.value = m.stopGraceMs ?? 250;
        cooldownBox.value = m.cooldownMs ?? 500;
        fromBox.currentIndex = m.cooldownFrom === "deactivate" ? 1 : 0;
        stopBox.checked = m.stopOnMismatch ?? true;
        sequenceBox.checked = m.refMode === "sequencer";
        seqLenBox.value = m.seqLen ?? 8;
        seqStepBox.value = m.seqStep ?? 1;
        actionBox.currentIndex = indexOfKey(actionTypes, a.type ?? "timeline");
        timelineBox.selected = a.id ?? "";
        profileBox.selected = a.id ?? "";
        muteBox.currentIndex = ["on", "off", "toggle"].indexOf(a.mode ?? "toggle");
        enabledBox.checked = t.enabled ?? false;
        open();
    }

    readonly property var aggs: [{key: "any", text: qsTr("any one of them")}, {key: "all", text: qsTr("all of them")}, {key: "mean", text: qsTr("the average of them")}]

    function buildChanges() {
        const match = Object.assign({}, extraMatch, {
            mode: modeBox.currentIndex === 0 ? "hash" : "structure",
            threshold: Math.round(thresholdSlider.value * 100) / 100,
            agg: aggs[Math.max(0, aggBox.currentIndex)].key,
            startFramesToCheck: framesBox.value,
            stopGraceMs: graceBox.value,
            cooldownMs: cooldownBox.value,
            cooldownFrom: fromBox.currentIndex === 1 ? "deactivate" : "activate",
            stopOnMismatch: stopBox.checked,
            refMode: sequenceBox.checked ? "sequencer" : "anchor"
        });
        if (sequenceBox.checked) {
            match.seqLen = seqLenBox.value;
            match.seqStep = seqStepBox.value;
        }
        const data = Object.assign({}, extraData, {regions: regions, match: match});
        const action = {type: actionType};
        if (actionType === "timeline")
            action.id = timelineBox.currentValue ?? "";
        else if (actionType === "profile")
            action.id = profileBox.currentValue ?? "";
        else if (actionType === "mute")
            action.mode = ["on", "off", "toggle"][Math.max(0, muteBox.currentIndex)];
        return {name: nameField.text.trim(), enabled: enabledBox.checked, cooldownMs: cooldownBox.value, mode: "press", source: {kind: "screen", data: data}, action: action};
    }

    onAccepted: {
        const changes = buildChanges();
        if (editing && trigger.origin === "demo")
            changes.origin = "";
        Daemon.setTrigger(editing ? trigger.id : "", changes);
    }

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

    // The page owns the picker (its windows must outlive this dialog while it is closed to leave the screen free): `regionPicker` is that RegionPicker.
    property var regionPicker: null
    property int pickTarget: -1 // the row being given a place, or -1 for a new region
    property bool picking: false

    function pick(target) {
        if (!regionPicker)
            return;
        pickTarget = target;
        picking = true;
        close();
        regionPicker.pick();
    }

    Connections {
        target: dialog.regionPicker
        enabled: dialog.picking
        function onPicked(x, y, w, h) {
            dialog.picking = false;
            if (dialog.pickTarget >= 0) {
                dialog.setRegion(dialog.pickTarget, "x", x);
                dialog.setRegion(dialog.pickTarget, "y", y);
                dialog.setRegion(dialog.pickTarget, "w", w);
                dialog.setRegion(dialog.pickTarget, "h", h);
            } else {
                dialog.addRegion(x, y, w, h);
            }
            dialog.open();
        }
        function onCancelled() {
            dialog.picking = false;
            dialog.open();
        }
    }

    ReferenceCaptureDialog { id: capture }

    // For the smoke test: opens and closes the capture dialog of a region.
    function openCapture(index) {
        capture.openFor(triggerId, index);
        capture.close();
    }

    Kirigami.FormLayout {
        QQC2.TextField {
            id: nameField
            Kirigami.FormData.label: qsTr("Name:")
            Layout.fillWidth: true
            maximumLength: 100
            placeholderText: qsTr("For example: Wave when the victory banner shows")
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: qsTr("When this picture is on the screen") }

        ColumnLayout {
            Kirigami.FormData.label: qsTr("Regions:")
            Kirigami.FormData.labelAlignment: Qt.AlignTop
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            // The screens and the regions on them, to scale.
            Item {
                id: map
                Accessible.ignored: true
                Layout.fillWidth: true
                implicitHeight: Kirigami.Units.gridUnit * 8
                readonly property var mapScreens: Qt.application.screens
                readonly property real mapLeft: Math.min(0, ...mapScreens.map(s => s.virtualX))
                readonly property real mapTop: Math.min(0, ...mapScreens.map(s => s.virtualY))
                readonly property real mapRight: Math.max(1, ...mapScreens.map(s => s.virtualX + s.width))
                readonly property real mapBottom: Math.max(1, ...mapScreens.map(s => s.virtualY + s.height))
                readonly property real scaleFactor: Math.min(width / Math.max(1, mapRight - mapLeft), height / Math.max(1, mapBottom - mapTop))

                Repeater {
                    model: map.mapScreens
                    delegate: Rectangle {
                        required property var modelData
                        x: (modelData.virtualX - map.mapLeft) * map.scaleFactor
                        y: (modelData.virtualY - map.mapTop) * map.scaleFactor
                        width: modelData.width * map.scaleFactor
                        height: modelData.height * map.scaleFactor
                        color: "transparent"
                        border.color: Kirigami.Theme.disabledTextColor
                        border.width: 1
                        QQC2.Label {
                            anchors.centerIn: parent
                            opacity: 0.5
                            font: Kirigami.Theme.smallFont
                            text: modelData.name
                            textFormat: Text.PlainText
                        }
                    }
                }
                Repeater {
                    model: dialog.regions.length
                    delegate: Rectangle {
                        required property int index
                        readonly property var r: dialog.regions[index] ?? ({})
                        x: ((r.x ?? 0) - map.mapLeft) * map.scaleFactor
                        y: ((r.y ?? 0) - map.mapTop) * map.scaleFactor
                        width: Math.max(2, (r.w ?? 0) * map.scaleFactor)
                        height: Math.max(2, (r.h ?? 0) * map.scaleFactor)
                        color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.35)
                        border.color: Kirigami.Theme.highlightColor
                        border.width: 1
                        QQC2.Label {
                            anchors.centerIn: parent
                            font: Kirigami.Theme.smallFont
                            text: String(parent.index + 1)
                        }
                    }
                }
            }

            Repeater {
                model: dialog.regions.length
                delegate: ColumnLayout {
                    id: row

                    required property int index
                    readonly property var r: dialog.regions[index] ?? ({})
                    readonly property bool rectangle: (r.kind ?? "rect") === "rect"
                    readonly property int references: (dialog.liveState.refs ?? [])[index] ?? -1
                    readonly property int score: (dialog.liveState.score ?? [])[index] ?? -1

                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Label {
                        font.bold: true
                        text: qsTr("Region %1").arg(row.index + 1)
                    }
                    RowLayout {
                        Repeater {
                            model: [{key: "x", label: qsTr("x"), min: -100000}, {key: "y", label: qsTr("y"), min: -100000}, {key: "w", label: qsTr("width"), min: 2}, {key: "h", label: qsTr("height"), min: 2}]
                            delegate: RowLayout {
                                required property var modelData
                                QQC2.Label { text: modelData.label; opacity: 0.7 }
                                QQC2.SpinBox {
                                    from: modelData.min
                                    to: 100000
                                    editable: true
                                    Accessible.name: qsTr("Region %1, %2").arg(row.index + 1).arg(modelData.label)
                                    value: Math.round(row.r[modelData.key] ?? 0)
                                    onValueModified: dialog.setRegion(row.index, modelData.key, value)
                                }
                            }
                        }
                    }
                    RowLayout {
                        QQC2.Button {
                            icon.name: "transform-crop"
                            text: qsTr("Pick on the screen")
                            onClicked: dialog.pick(row.index)
                        }
                        QQC2.Button {
                            icon.name: "camera-photo"
                            text: qsTr("References…")
                            enabled: dialog.editing && row.rectangle
                            onClicked: capture.openFor(dialog.triggerId, row.index)
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                            QQC2.ToolTip.text: dialog.editing ? qsTr("Take the pictures this region is compared with") : qsTr("Save the trigger first; then its regions can take their reference pictures")
                        }
                        QQC2.ToolButton {
                            icon.name: "edit-delete"
                            text: qsTr("Remove region")
                            display: QQC2.AbstractButton.IconOnly
                            onClicked: dialog.removeRegion(row.index)
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.text: text
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            opacity: 0.7
                            font: Kirigami.Theme.smallFont
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: {
                                const parts = [];
                                if (!row.rectangle)
                                    parts.push(qsTr("a %1, matched as its bounding rectangle").arg(String(row.r.kind).slice(0, 12)));
                                if (row.references >= 0)
                                    parts.push(qsTr("%n reference picture(s)", "", row.references));
                                if (row.score >= 0)
                                    parts.push(qsTr("now %1%").arg((row.score / 10).toFixed(0)));
                                return parts.join(" · ");
                            }
                        }
                    }
                    QQC2.ProgressBar {
                        Layout.fillWidth: true
                        visible: row.score >= 0
                        from: 0
                        to: 1000
                        value: row.score
                    }
                }
            }

            RowLayout {
                QQC2.Button {
                    icon.name: "list-add"
                    text: qsTr("Draw a region on the screen")
                    enabled: dialog.regions.length < 8
                    onClicked: dialog.pick(-1)
                }
                QQC2.Button {
                    icon.name: "list-add"
                    text: qsTr("Add one to type")
                    enabled: dialog.regions.length < 8
                    onClicked: dialog.addRegion(0, 0, 200, 100)
                }
            }
            QQC2.Label {
                Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("A region is a rectangle in your desktop's coordinates. A package made on another screen size may need its regions moved. Your own pets over a region are left out of the comparison (a setting).")
            }
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: qsTr("How it decides") }

        QQC2.ComboBox {
            id: modeBox
            Kirigami.FormData.label: qsTr("Compared by:")
            Layout.fillWidth: true
            model: [qsTr("A fingerprint of the picture (fast; the same picture, not brighter or shifted)"), qsTr("Its structure (also when brighter, or a little shifted)")]
        }
        RowLayout {
            Kirigami.FormData.label: qsTr("Looks the same at:")
            QQC2.Slider {
                id: thresholdSlider
                Layout.fillWidth: true
                from: 0.3
                to: 1.0
                stepSize: 0.01
                value: 0.65
            }
            QQC2.Label { text: Math.round(thresholdSlider.value * 100) + "%"; Layout.minimumWidth: Kirigami.Units.gridUnit * 2.5 }
        }
        QQC2.ComboBox {
            id: aggBox
            visible: dialog.regions.length > 1
            Kirigami.FormData.label: qsTr("It is there when:")
            model: dialog.aggs
            textRole: "text"
        }
        QQC2.SpinBox {
            id: framesBox
            Kirigami.FormData.label: qsTr("Looks in a row to be sure:")
            from: 1
            to: 60
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: qsTr("The picture must match this many looks in a row. With 5 looks a second, 3 is about half a second.")
        }
        QQC2.CheckBox {
            id: stopBox
            text: qsTr("It stops when the picture goes away")
        }
        QQC2.SpinBox {
            id: graceBox
            enabled: stopBox.checked
            Kirigami.FormData.label: qsTr("Gone for (ms) before it stops:")
            from: 0
            to: 10000
            stepSize: 50
            editable: true
        }
        QQC2.SpinBox {
            id: cooldownBox
            Kirigami.FormData.label: qsTr("Wait before it can fire again (ms):")
            from: 0
            to: 86400000
            stepSize: 100
            editable: true
        }
        QQC2.ComboBox {
            id: fromBox
            Kirigami.FormData.label: qsTr("The wait counts from:")
            model: [qsTr("when it fires"), qsTr("when it stops")]
        }
        QQC2.CheckBox {
            id: sequenceBox
            text: qsTr("The reference is a sequence of pictures, one after another")
        }
        QQC2.SpinBox {
            id: seqLenBox
            visible: sequenceBox.checked
            Kirigami.FormData.label: qsTr("Pictures in the sequence:")
            from: 1
            to: 64
        }
        QQC2.SpinBox {
            id: seqStepBox
            visible: sequenceBox.checked
            Kirigami.FormData.label: qsTr("Between pictures:")
            from: 1
            to: 100
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
        QQC2.ComboBox {
            id: muteBox
            visible: dialog.actionType === "mute"
            Kirigami.FormData.label: qsTr("Sound:")
            model: [qsTr("mute"), qsTr("unmute"), qsTr("toggle")]
        }
        QQC2.Label {
            visible: dialog.canHold
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("A screen trigger holds: it does this while the picture is there and undoes it when the picture goes (a timeline plays for as long as it stays, hiding or muting is reversed).")
        }

        Kirigami.Separator { Kirigami.FormData.isSection: true }

        QQC2.Switch {
            id: enabledBox
            Kirigami.FormData.label: qsTr("Enabled:")
        }
    }
}
