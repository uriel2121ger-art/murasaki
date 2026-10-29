import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// A desktop pet (Anima Engine item or local animation file): pick a variant, show it on a screen, watch its
// optimized copy being made, and edit the pets already placed on the desktop.
Kirigami.ScrollablePage {
    id: page

    required property string itemId

    property var item: ({})
    property string loadError: ""
    // The variants with their transcode state; replaced as a whole when the daemon reports progress.
    property var variants: []
    property string variantWanted: ""
    property string outputWanted: "*"
    property bool playOriginalWanted: false
    // Ids of the pets of this item, in the daemon's order. Only replaced when the set changes, so that editing a
    // pet does not rebuild the controls under the pointer.
    property var instanceIds: []

    readonly property bool loaded: item.id !== undefined
    readonly property bool isLocal: (item.source ?? "") === "local"
    // A puppet (docs/pets/puppets.md): a folder with a model that the puppet engine draws, not an animation file.
    readonly property bool isPuppet: (item.format ?? "") === "puppet"
    readonly property var puppet: item.puppet ?? ({})
    // Made by an import: its files live in the data folder and are the program's to delete.
    readonly property bool generated: item.generated ?? false
    readonly property var scenes: item.scenes ?? []
    readonly property var importable: item.importable ?? []
    readonly property var variant: variants.find(v => v.name === variantWanted) ?? variants[0] ?? null
    readonly property bool canShow: loaded && Daemon.petsAvailable && variant !== null && (item.status ?? "ok") === "ok"
    readonly property var outputNames: Surface.outputs.map(o => o.name)
    readonly property var outputChoices: [{text: qsTr("Main screen"), value: "*"}].concat(
        outputNames.map(n => ({text: plain(n), value: n})))
    readonly property string targetOutput: outputChoices.some(c => c.value === outputWanted) ? outputWanted : "*"
    readonly property var variantChoices: variants.map(v => ({text: plain(v.name), value: v.name}))

    title: loaded ? plain(item.title) : qsTr("Pet")

    // Item files and compositor names are untrusted and some Kirigami/Controls items auto-detect rich text:
    // replace angle brackets so none of it can ever parse as markup.
    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    // The words for a name the puppet check or the overlay reports (docs/pets/puppets.md section 10); the name itself, cleaned, for one we do not know.
    function puppetNote(name) {
        const base = String(name).split(": ")[0];
        const detail = plain(String(name).substring(base.length + 2)).substring(0, 120);
        switch (base) {
        case "puppet: not a puppet": return qsTr("This folder is not a puppet: it needs exactly one .inp file or one .model3.json file.");
        case "puppet: unsafe path": return qsTr("The folder holds a link or a file that is not a plain file, or a path that leaves it: %1").arg(detail);
        case "puppet: file missing": return qsTr("A file the model names is missing: %1").arg(detail);
        case "puppet: too large": return qsTr("The model is over a limit (%1).").arg(detail);
        case "puppet: manifest invalid": return qsTr("The model's description cannot be read: %1").arg(detail);
        case "puppet: unsupported format": return qsTr("A file of the model is of a kind Murasaki does not read: %1").arg(detail);
        case "puppet: needs the Cubism runtime": return qsTr("This is a Cubism model. Murasaki does not ship the Cubism runtime and does not draw Cubism models yet; an Inochi2D puppet (.inp) is drawn.");
        case "puppet: physics rig skipped": return qsTr("A physics rig of the model is not simulated: %1").arg(detail);
        case "puppet: motion skipped": return qsTr("A motion or expression of the model cannot be read and is left out: %1").arg(detail);
        case "puppet: check failed": return qsTr("The check could not run: %1").arg(detail);
        case "puppet: payload not read": return "";
        default: return plain(name);
        }
    }

    // The words for a degrade name of the fullscreen mode; "" for any other name (the behaviour ones are shown elsewhere) and for one we do not know.
    function stageNote(name) {
        const hidden = "stage: hidden by a fullscreen window";
        if (name.startsWith(hidden)) {
            const app = plain(name.substring(hidden.length + 2)).substring(0, 100);
            return app === "" ? qsTr("Hidden while a window is fullscreen on its screen.") : qsTr("Hidden while %1 is fullscreen on its screen.").arg(app);
        }
        switch (name) {
        case "stage: too many outputs": return qsTr("More screens than it can cover: only the first ones show it.");
        case "stage: not fullscreen on this session": return qsTr("This session cannot cover a screen: it is shown as a pet.");
        case "stage: fullscreen apps unknown": return qsTr("This session cannot tell which window is fullscreen: it stays visible.");
        case "stage: resolution reduced": return "";
        case "stage: below windows unsupported": return qsTr("This session cannot put it below the windows.");
        case "stage: transcode pending": return qsTr("A copy of the right size is being made; the original plays meanwhile.");
        case "stage: layer-shell missing": return qsTr("This compositor cannot draw over a whole screen.");
        case "stage: needs a compositor": return qsTr("This X11 session has no compositor, so it cannot be drawn over the screen: it is shown as a pet.");
        case "stage: no copy big enough": return qsTr("No prepared copy is big enough for this screen: the original plays, which is heavier.");
        case "stage: output missing": return qsTr("That screen is not connected: nothing is shown until it is.");
        case "stage: output too large": return qsTr("That screen is too large to cover: it is shown as a pet.");
        case "stage: output already has a stage": return qsTr("Another animation already covers that screen: this one is not shown.");
        }
        return "";
    }

    function fileUrl(path) {
        return path ? "file://" + String(path).split("/").map(encodeURIComponent).join("/") : "";
    }

    function megabytes(bytes) {
        return qsTr("%1 MB").arg((bytes / 1048576).toFixed(1));
    }

    function variantSummary(v) {
        if (!v)
            return "";
        const parts = [qsTr("%1 × %2 px").arg(v.width).arg(v.height)];
        if (v.frames > 1)
            parts.push(qsTr("%n frame(s)", "", v.frames));
        if (v.fps > 0)
            parts.push(qsTr("%1 fps").arg(v.fps));
        parts.push(megabytes(v.bytes));
        return parts.join(" · ");
    }

    function petById(petId) {
        return Daemon.pets.find(p => p.petId === petId);
    }

    function outputLabel(output) {
        return output === "*" ? qsTr("Main screen") : String(output);
    }

    // The overlay's view of a placement (failures, blocked), or undefined while the overlay is not running.
    function windowOf(petId) {
        return Overlay.windows.find(w => w.petId === petId);
    }

    // The screen a pet whose output is gone can be moved to: the first connected output the compositor and this
    // window both know, else the main screen.
    function currentScreen() {
        const screens = Qt.application.screens;
        if (screens.length === 0)
            return null;
        for (const name of outputNames) {
            const screen = screens.find(s => s.name === name);
            if (screen)
                return {name: screen.name, width: screen.width, height: screen.height};
        }
        return {name: "*", width: screens[0].width, height: screens[0].height};
    }

    // Moves the pet onto that screen, with its position clamped so that the whole pet is inside it.
    function moveToCurrentScreen(petId) {
        const pet = petById(petId);
        const screen = currentScreen();
        if (!pet || !screen)
            return;
        const shown = variants.find(v => v.name === pet.variant);
        const width = Math.min(pet.width, screen.width);
        const height = shown && shown.width > 0 ? width * shown.height / shown.width : width;
        Daemon.updatePet(petId, {
            output: screen.name,
            x: Math.round(Math.max(0, Math.min(pet.x, screen.width - width))),
            y: Math.round(Math.max(0, Math.min(pet.y, screen.height - height)))
        });
    }

    function refreshInstances() {
        const ids = Daemon.pets.filter(p => p.itemId === itemId).map(p => p.petId);
        if (JSON.stringify(ids) !== JSON.stringify(instanceIds))
            instanceIds = ids;
    }

    function show() {
        const options = {};
        if (playOriginalWanted)
            options.playOriginal = true;
        Daemon.addPet(itemId, variant.name, targetOutput, options);
        playOriginalWanted = false; // the next pet starts with the light copy again
    }

    Component.onCompleted: {
        refreshInstances();
        Daemon.getItem(itemId);
    }

    ImportDialog {
        id: importDialog
    }

    KirigamiDialogs.PromptDialog {
        id: removeConfirm
        title: qsTr("Remove this pet?")
        subtitle: qsTr("It was made by an import.")
        standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel
        onAccepted: {
            Daemon.removeImportedPet(page.itemId, deleteFilesBox.checked);
            applicationWindow().pageStack.pop();
        }

        QQC2.CheckBox {
            id: deleteFilesBox
            text: qsTr("Also delete the converted files")
            checked: true
        }
    }

    // Edits still waiting out their debounce must not be lost when the page goes away.
    Component.onDestruction: Daemon.flushPetUpdates()

    Connections {
        target: Daemon

        function onItemLoaded(id, loaded) {
            if (id !== page.itemId)
                return;
            page.item = loaded;
            page.loadError = "";
            page.variants = loaded.variants ?? [];
            if (!page.variants.some(v => v.name === page.variantWanted))
                page.variantWanted = page.variants.length > 0 ? page.variants[0].name : "";
        }

        function onItemLoadFailed(id, message) {
            if (id === page.itemId)
                page.loadError = message;
        }

        function onItemsChanged(added, changed, removed) {
            if (changed.includes(page.itemId) || added.includes(page.itemId))
                Daemon.getItem(page.itemId);
            else if (removed.includes(page.itemId))
                page.loadError = qsTr("This pet is no longer in the library");
        }

        function onTranscodeChanged(id) {
            if (id === page.itemId)
                Daemon.transcodeState(page.itemId);
        }

        function onTranscodeStateLoaded(id, states) {
            if (id === page.itemId)
                page.variants = states;
        }

        function onPetsChanged() { page.refreshInstances(); }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.loadError !== ""
            type: Kirigami.MessageType.Error
            text: qsTr("Could not load this pet: %1").arg(page.plain(page.loadError))
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.loaded && (page.item.status ?? "ok") === "invalid"
            type: Kirigami.MessageType.Error
            text: qsTr("This pet is invalid: %1").arg(page.plain(page.item.statusDetail))
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.loaded && (page.item.risks ?? []).length > 0
            type: Kirigami.MessageType.Warning
            text: qsTr("This item contains programs (%1). Murasaki never runs them; keep in mind it may be malicious if you use it in another app.")
                .arg((page.item.risks ?? []).slice(0, 5).map(r => page.plain(r.file)).join(", ")
                    + ((page.item.risks ?? []).length > 5 ? ", \u2026" : ""))
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !Daemon.petsAvailable
            type: Kirigami.MessageType.Warning
            text: qsTr("Desktop pets need a newer Murasaki service. Update and restart it.")
        }

        // Preview, with the title beside it on wide pages.
        GridLayout {
            Layout.fillWidth: true
            columns: page.width > Kirigami.Units.gridUnit * 40 ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing

            Rectangle {
                Layout.preferredWidth: Math.min(Kirigami.Units.gridUnit * 20, page.width - Kirigami.Units.gridUnit * 4)
                Layout.preferredHeight: Layout.preferredWidth * 9 / 16
                Layout.alignment: Qt.AlignTop
                color: Kirigami.Theme.alternateBackgroundColor
                radius: Kirigami.Units.cornerRadius
                clip: true

                // The cached Workshop image, or the first frame of the chosen variant.
                Image {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    source: page.item.preview ? page.fileUrl(page.item.preview)
                                              : page.variant ? page.fileUrl(page.variant.file) : ""
                    fillMode: page.item.preview ? Image.PreserveAspectCrop : Image.PreserveAspectFit
                    asynchronous: true
                    sourceSize.width: 640
                    sourceSize.height: 360
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 1
                    text: page.loaded ? page.item.title : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    visible: page.loaded
                    opacity: 0.7
                    textFormat: Text.PlainText
                    text: page.isPuppet ? qsTr("Puppet · Local folder") : page.isLocal ? qsTr("Pet · Local file") : qsTr("Pet · Anima Engine")
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    visible: (page.item.tags ?? []).length > 0

                    Repeater {
                        model: page.item.tags ?? []
                        Kirigami.Chip {
                            required property string modelData
                            text: page.plain(modelData)
                            checkable: false
                            closable: false
                            hoverEnabled: false
                        }
                    }
                }

                QQC2.Button {
                    visible: page.isLocal
                    icon.name: "edit-delete"
                    text: qsTr("Remove from library")
                    enabled: Daemon.petsAvailable && page.instanceIds.length === 0
                    onClicked: {
                        if (page.generated) {
                            deleteFilesBox.checked = true;
                            removeConfirm.open();
                        } else {
                            if (page.isPuppet) {
                                if (!Daemon.puppetsSupported)
                                    return; // (the call would do nothing, and the page must not look as if the item were gone)
                                Daemon.removeLocalPuppetItem(page.itemId);
                            } else {
                                Daemon.removeLocalPetItem(page.itemId);
                            }
                            applicationWindow().pageStack.pop();
                        }
                    }
                    QQC2.ToolTip.visible: !enabled && hovered && Daemon.petsAvailable
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: qsTr("Remove its pets from the desktop first")
                }

                // Reopens the import form with the values this pet was made with; the source files are chosen again.
                QQC2.Button {
                    visible: page.generated && Daemon.importsSupported && (page.item.importSpec ?? null) !== null
                    icon.name: "document-edit"
                    text: qsTr("Edit import…")
                    onClicked: importDialog.openFor(page.item.importSpec.kind, page.item.importSpec)
                }
            }
        }

        // Files of a Workshop item that are not animations but can become one: the user chooses (or the setting does it for them).
        Kirigami.Heading {
            visible: page.importable.length > 0
            level: 2
            text: qsTr("Files you can import")
        }
        Repeater {
            model: page.importable
            RowLayout {
                required property var modelData
                Layout.fillWidth: true
                QQC2.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    textFormat: Text.PlainText
                    text: page.plain(modelData.name)
                }
                QQC2.Button {
                    icon.name: "document-import"
                    text: modelData.kind === "mdaa" ? qsTr("Import this package") : qsTr("Import this file")
                    enabled: Daemon.importsSupported
                    onClicked: importDialog.openFor(modelData.kind, {name: page.item.title}, modelData.path)
                    QQC2.ToolTip.visible: !enabled && hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: qsTr("The Murasaki service is too old for this")
                }
            }
        }

        // The scenes of an imported package (actions.mddo): start one on the first pet of this item.
        Kirigami.Heading {
            visible: page.scenes.length > 0
            level: 2
            text: qsTr("Scenes")
        }
        Repeater {
            model: page.scenes
            RowLayout {
                required property var modelData
                required property int index
                Layout.fillWidth: true
                QQC2.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: modelData.show ? qsTr("%1: %2").arg(index + 1).arg(page.plain(modelData.variant))
                                         : qsTr("%1: hides the pet").arg(index + 1)
                    opacity: modelData.show ? 1 : 0.6
                }
                QQC2.Button {
                    text: qsTr("Start")
                    icon.name: "media-playback-start"
                    enabled: page.instanceIds.length > 0 && Daemon.importsSupported && (modelData.show || Daemon.automationSupported)
                    onClicked: Daemon.startScene(page.instanceIds[0], index)
                    QQC2.ToolTip.visible: !enabled && hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: page.instanceIds.length === 0 ? qsTr("Show the pet on the desktop first") : qsTr("Hiding needs a newer Murasaki service")
                }
                QQC2.Button {
                    text: qsTr("Make a timeline")
                    icon.name: "view-time-schedule"
                    enabled: Daemon.automationSupported
                    onClicked: Daemon.timelineFromScene(page.itemId, index)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: qsTr("Make this scene a timeline that a trigger can play (see Timelines)")
                }
            }
        }

        // A Triggers pack: its animations are pets, its timelines are made on request, disabled.
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.item.hasPack === true
            type: Kirigami.MessageType.Information
            text: qsTr("This package is a Triggers pack. Its timeline can be made here; the triggers it brings are made disabled, and its screen matching is never active. Add a pet for each animation of the pack first.")
            actions: Kirigami.Action {
                text: qsTr("Make the timeline and triggers")
                icon.name: "view-time-schedule"
                enabled: Daemon.automationSupported
                onTriggered: Daemon.importPackAutomation(page.itemId)
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Kirigami.Heading {
            level: 2
            text: qsTr("Show on desktop")
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            enabled: page.loaded && Daemon.petsAvailable

            QQC2.ComboBox {
                id: variantBox
                Kirigami.FormData.label: qsTr("Variant:")
                Layout.minimumWidth: Kirigami.Units.gridUnit * 14
                textRole: "text"
                valueRole: "value"
                model: page.variantChoices
                onActivated: page.variantWanted = currentValue
                onModelChanged: currentIndex = Math.max(0, indexOfValue(page.variantWanted))
                Binding on currentIndex {
                    value: Math.max(0, page.variantChoices.findIndex(c => c.value === page.variantWanted))
                }
            }

            QQC2.Label {
                visible: page.variant !== null
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 28
                wrapMode: Text.Wrap
                opacity: 0.7
                textFormat: Text.PlainText
                text: page.variantSummary(page.variant)
                    + (page.variant && page.variant.heavy ? " · " + qsTr("Heavy") : "")
            }

            QQC2.ComboBox {
                id: outputBox
                Kirigami.FormData.label: qsTr("Screen:")
                textRole: "text"
                valueRole: "value"
                model: page.outputChoices
                onActivated: page.outputWanted = currentValue
                onModelChanged: currentIndex = Math.max(0, indexOfValue(page.targetOutput))
                Binding on currentIndex {
                    value: Math.max(0, page.outputChoices.findIndex(c => c.value === page.targetOutput))
                }
            }

            ColumnLayout {
                Kirigami.FormData.label: qsTr("Quality:")
                spacing: 0

                QQC2.Switch {
                    id: wantedOriginalSwitch
                    text: qsTr("Play original (heavy)")
                    onToggled: page.playOriginalWanted = checked
                    // Reset by show(): the choice is for one pet, not for every pet that follows.
                    Binding {
                        target: wantedOriginalSwitch; property: "checked"; value: page.playOriginalWanted
                        restoreMode: Binding.RestoreNone
                    }
                }
                QQC2.Label {
                    Layout.leftMargin: wantedOriginalSwitch.indicator.width + wantedOriginalSwitch.spacing
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: qsTr("Uses much more CPU")
                }
            }

            QQC2.Button {
                icon.name: "list-add"
                text: qsTr("Show on desktop")
                enabled: page.canShow
                onClicked: page.show()
            }
        }

        // What is known about a puppet: how it was checked, what it can do, and what it lacks.
        ColumnLayout {
            id: puppetBox

            Layout.fillWidth: true
            visible: page.loaded && page.isPuppet
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                level: 2
                text: qsTr("Puppet")
            }

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: {
                    const p = page.puppet;
                    const size = (p.canvas ?? []).length === 2 ? qsTr("canvas %1 × %2").arg(p.canvas[0]).arg(p.canvas[1]) : "";
                    const kind = (p.backend ?? "") === "cubism" ? qsTr("Cubism model") : qsTr("Inochi2D puppet");
                    const checked = p.checked ? qsTr("checked") : qsTr("being checked…");
                    return [kind, size, checked].filter(s => s !== "").join(" · ");
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                opacity: 0.7
                visible: (page.puppet.motionGroups ?? []).length > 0 || (page.puppet.expressions ?? []).length > 0
                text: qsTr("Motion groups: %1. Expressions: %2.")
                    .arg((page.puppet.motionGroups ?? []).map(g => page.plain(g.name)).join(", ") || qsTr("none"))
                    .arg((page.puppet.expressions ?? []).map(e => page.plain(e)).join(", ") || qsTr("none"))
            }

            Repeater {
                model: (page.puppet.reasons ?? []).concat(page.puppet.degraded ?? []).map(n => page.puppetNote(n)).filter(s => s !== "")
                Kirigami.InlineMessage {
                    required property string modelData
                    Layout.fillWidth: true
                    visible: true
                    type: (page.puppet.reasons ?? []).length > 0 ? Kirigami.MessageType.Error : Kirigami.MessageType.Information
                    text: modelData
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("The variants are the model itself, and its motion groups and expressions: a state of the pet (below) can play one.")
            }
        }

        // The optimized copy of the chosen variant.
        ColumnLayout {
            id: transcodeBox

            readonly property string phase: page.variant?.transcode?.state ?? "none"
            readonly property real progress: page.variant?.transcode?.progress ?? 0

            Layout.fillWidth: true
            visible: page.variant !== null && Daemon.petsAvailable && !page.isPuppet // (a puppet is drawn live: nothing is converted)
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                level: 2
                text: qsTr("Optimized copy")
            }

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                visible: page.variant && page.variant.heavy && transcodeBox.phase !== "done"
                text: qsTr("This animation is large. A smaller copy plays with much less CPU; it is made once, in the background.")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                QQC2.Label {
                    text: {
                        switch (transcodeBox.phase) {
                        case "queued": return qsTr("Waiting to convert…");
                        case "running": return qsTr("Converting… %1%").arg(Math.round(transcodeBox.progress * 100));
                        case "done": return qsTr("Ready");
                        case "failed": return qsTr("Conversion failed");
                        default: return qsTr("Not converted");
                        }
                    }
                    font.bold: true
                }

                // A plain bar: it looks the same in every style and costs nothing.
                Rectangle {
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    Layout.preferredHeight: Kirigami.Units.smallSpacing * 2
                    Layout.alignment: Qt.AlignVCenter
                    visible: transcodeBox.phase === "running"
                    radius: height / 2
                    color: Kirigami.Theme.alternateBackgroundColor
                    border.width: 1
                    border.color: Qt.alpha(Kirigami.Theme.textColor, 0.25)

                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, transcodeBox.progress))
                        height: parent.height
                        radius: parent.radius
                        color: Kirigami.Theme.highlightColor
                    }
                }

                QQC2.Button {
                    visible: transcodeBox.phase === "none" || transcodeBox.phase === "failed"
                    icon.name: transcodeBox.phase === "failed" ? "view-refresh" : "run-build"
                    text: transcodeBox.phase === "failed" ? qsTr("Try again") : qsTr("Convert now")
                    onClicked: Daemon.requestTranscode(page.itemId, page.variant.name)
                }

                Item { Layout.fillWidth: true }
            }

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: Kirigami.Theme.negativeTextColor
                visible: transcodeBox.phase === "failed"
                text: page.variant?.transcode?.error ?? ""
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: page.instanceIds.length > 0
        }

        Kirigami.Heading {
            level: 2
            visible: page.instanceIds.length > 0
            text: qsTr("On the desktop")
        }

        Repeater {
            model: page.instanceIds

            Kirigami.AbstractCard {
                id: card

                required property string modelData
                readonly property string petId: modelData
                readonly property var pet: page.petById(petId)
                readonly property bool screenMissing: pet !== undefined && pet.output !== "*"
                    && !page.outputNames.includes(pet.output)
                // The overlay gave up on this pet (its file failed or crashed the overlay).
                readonly property var overlayWindow: page.windowOf(petId)
                readonly property bool playbackFailed: overlayWindow !== undefined && overlayWindow.blocked === true
                // Edits go out after a short pause; until then the controls keep what the user set.
                readonly property bool settled: pet !== undefined && !Daemon.pendingPetIds.includes(petId)
                // The fullscreen mode, and what the overlay says it does less than asked (the names of docs/pets/stage.md; the text is ours, only the application's name is
                // the system's and is shown as plain text).
                readonly property bool stageOn: (pet?.stage?.mode ?? "off") !== "off"
                readonly property var stageNotes: {
                    const out = [];
                    if (!stageOn || overlayWindow === undefined)
                        return out;
                    for (const name of (overlayWindow.degraded ?? [])) {
                        const note = page.stageNote(String(name));
                        if (note !== "")
                            out.push(note);
                    }
                    if ((overlayWindow.stageScale ?? 1) < 1)
                        out.push(qsTr("Drawn at %1% of the full size to keep within the limit in Settings.").arg(Math.round(overlayWindow.stageScale * 100)));
                    return out;
                }
                // The behaviour fields came with revision 12 of the daemon's API.
                readonly property bool behavioursEnabled: Daemon.petBehavioursAvailable
                // Whether the overlay acts on each group of fields; the card says so under the ones it does not yet. Flip each to true once its
                // code is merged and checked (phase 2 of the Anima parity: pointer = tasks 4 and 7, layer = task 6, transform = task 5).
                readonly property bool pointerApplied: true    // click through, lock, under the cursor, dodge
                readonly property bool layerApplied: true      // keep above the windows
                readonly property bool transformApplied: true  // rotation and flips

                Layout.fillWidth: true

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: card.pet ? qsTr("%1 on %2").arg(card.pet.variant).arg(page.outputLabel(card.pet.output)) : ""
                                textFormat: Text.PlainText
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                visible: card.screenMissing
                                text: qsTr("This screen is not connected; the pet reappears with it.")
                                color: Kirigami.Theme.neutralTextColor
                                wrapMode: Text.Wrap
                            }
                        }

                        QQC2.Button {
                            visible: card.screenMissing
                            enabled: page.currentScreen() !== null
                            icon.name: "computer"
                            text: qsTr("Move to current screen")
                            onClicked: page.moveToCurrentScreen(card.petId)
                        }

                        QQC2.Button {
                            icon.name: "edit-delete"
                            text: qsTr("Remove")
                            onClicked: Daemon.removePet(card.petId)
                        }
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: card.playbackFailed
                        type: Kirigami.MessageType.Error
                        text: qsTr("This pet could not be played.") + (card.overlayWindow?.error
                            ? " " + page.plain(card.overlayWindow.error) : "")
                        actions: [
                            Kirigami.Action {
                                icon.name: "view-refresh"
                                text: qsTr("Try again")
                                onTriggered: Overlay.resetFailures(card.petId)
                            }
                        ]
                    }

                    Kirigami.FormLayout {
                        Layout.fillWidth: true
                        twinFormLayouts: []

                        RowLayout {
                            Kirigami.FormData.label: qsTr("Position:")
                            spacing: Kirigami.Units.smallSpacing

                            QQC2.Label { text: qsTr("X") }
                            QQC2.SpinBox {
                                id: xBox
                                from: -100000
                                to: 100000
                                editable: true
                                onValueModified: Daemon.queuePetUpdate(card.petId, {x: value})
                                Binding {
                                    target: xBox; property: "value"; value: Math.round(card.pet?.x ?? 0)
                                    when: card.settled && !xBox.activeFocus; restoreMode: Binding.RestoreNone
                                }
                            }
                            QQC2.Label { text: qsTr("Y") }
                            QQC2.SpinBox {
                                id: yBox
                                from: -100000
                                to: 100000
                                editable: true
                                onValueModified: Daemon.queuePetUpdate(card.petId, {y: value})
                                Binding {
                                    target: yBox; property: "value"; value: Math.round(card.pet?.y ?? 0)
                                    when: card.settled && !yBox.activeFocus; restoreMode: Binding.RestoreNone
                                }
                            }
                        }

                        QQC2.SpinBox {
                            id: widthBox
                            Kirigami.FormData.label: qsTr("Width:")
                            from: 16
                            to: 16384
                            editable: true
                            textFromValue: (value, locale) => qsTr("%1 px").arg(value)
                            valueFromText: (text, locale) => parseInt(text) || from
                            onValueModified: Daemon.queuePetUpdate(card.petId, {width: value})
                            Binding {
                                target: widthBox; property: "value"; value: Math.round(card.pet?.width ?? 256)
                                when: card.settled && !widthBox.activeFocus; restoreMode: Binding.RestoreNone
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: qsTr("Opacity:")
                            spacing: Kirigami.Units.largeSpacing

                            QQC2.Slider {
                                id: opacitySlider
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                from: 0.1
                                to: 1
                                stepSize: 0.05
                                snapMode: QQC2.Slider.SnapAlways
                                onMoved: Daemon.queuePetUpdate(card.petId, {opacity: Math.round(value * 100) / 100})
                                Binding {
                                    target: opacitySlider; property: "value"; value: card.pet?.opacity ?? 1
                                    when: card.settled && !opacitySlider.pressed; restoreMode: Binding.RestoreNone
                                }
                            }
                            QQC2.Label {
                                Layout.minimumWidth: Kirigami.Units.gridUnit * 3
                                text: qsTr("%1%").arg(Math.round(opacitySlider.value * 100))
                            }
                        }

                        QQC2.Switch {
                            id: pauseSwitch
                            Kirigami.FormData.label: qsTr("Playback:")
                            text: qsTr("Paused")
                            onToggled: Daemon.updatePet(card.petId, {paused: checked})
                            Binding {
                                target: pauseSwitch; property: "checked"; value: card.pet?.paused ?? false
                                restoreMode: Binding.RestoreNone
                            }
                            Connections {
                                target: Daemon
                                function onError() { pauseSwitch.checked = card.pet?.paused ?? false; }
                            }
                        }

                        QQC2.ComboBox {
                            id: speedBox
                            Kirigami.FormData.label: qsTr("Speed:")
                            // Speed and rates above 60 fps came with revision 11 of the daemon's API.
                            enabled: Daemon.apiVersion >= 11
                            QQC2.ToolTip.visible: hovered && !enabled
                            QQC2.ToolTip.text: qsTr("The running daemon is too old for this; restart it after updating.")
                            textRole: "text"
                            // How fast the animation plays. "Original timing" shows every frame as long as the file says;
                            // a rate shows every frame for the same time (Anima Engine plays all its pets like that, 15
                            // frames a second by default); a multiple plays the file's own delays faster or slower.
                            model: [
                                {text: qsTr("Original timing"), fps: 0, speed: 1},
                                {text: qsTr("%1x speed").arg("0.25"), fps: 0, speed: 0.25},
                                {text: qsTr("%1x speed").arg("0.5"), fps: 0, speed: 0.5},
                                {text: qsTr("%1x speed").arg("0.75"), fps: 0, speed: 0.75},
                                {text: qsTr("%1x speed").arg("1.5"), fps: 0, speed: 1.5},
                                {text: qsTr("%1x speed").arg("2"), fps: 0, speed: 2},
                                {text: qsTr("%1x speed").arg("4"), fps: 0, speed: 4},
                                {text: qsTr("%1 fps (like Anima Engine)").arg(15), fps: 15, speed: 1},
                                {text: qsTr("%1 fps").arg(20), fps: 20, speed: 1},
                                {text: qsTr("%1 fps (default)").arg(24), fps: 24, speed: 1},
                                {text: qsTr("%1 fps").arg(30), fps: 30, speed: 1},
                                {text: qsTr("%1 fps").arg(60), fps: 60, speed: 1},
                                {text: qsTr("%1 fps").arg(120), fps: 120, speed: 1}
                            ]
                            function indexOfTiming(pet) {
                                const fps = pet?.fps ?? 0, speed = pet?.speed ?? 1;
                                return model.findIndex(m => m.fps === fps && m.speed === speed);
                            }
                            // A timing set from outside the list (murasakictl --fps 12) is shown as it is.
                            displayText: currentIndex >= 0 ? currentText
                                : ((card.pet?.fps ?? 0) > 0 ? qsTr("%1 fps").arg(card.pet.fps) : qsTr("%1x speed").arg(card.pet?.speed ?? 1))
                            onActivated: index => Daemon.updatePet(card.petId, {fps: model[index].fps, speed: model[index].speed})
                            Binding {
                                target: speedBox; property: "currentIndex"
                                value: speedBox.indexOfTiming(card.pet)
                                restoreMode: Binding.RestoreNone
                            }
                            Connections {
                                target: Daemon
                                function onError() { speedBox.currentIndex = speedBox.indexOfTiming(card.pet); }
                            }
                        }

                        // What the pet does with the pointer, and how it is drawn (API revision 12; the overlay learns to act on each of them in
                        // phase 2 of the Anima parity, tasks 4 to 7). The values are stored with the pet either way.
                        Kirigami.Separator {
                            Kirigami.FormData.isSection: true
                            Kirigami.FormData.label: qsTr("Behaviour")
                        }

                        QQC2.Switch {
                            id: clickThroughSwitch
                            Kirigami.FormData.label: qsTr("Pointer:")
                            enabled: card.behavioursEnabled
                            text: qsTr("Click through the pet")
                            onToggled: Daemon.updatePet(card.petId, {clickThrough: checked})
                            Binding { target: clickThroughSwitch; property: "checked"; value: card.pet?.clickThrough ?? true; restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { clickThroughSwitch.checked = card.pet?.clickThrough ?? true; } }
                        }

                        QQC2.Switch {
                            id: lockSwitch
                            Kirigami.FormData.label: qsTr("Position:")
                            enabled: card.behavioursEnabled
                            text: qsTr("Lock (a drag does not move it)")
                            onToggled: Daemon.updatePet(card.petId, {locked: checked})
                            Binding { target: lockSwitch; property: "checked"; value: card.pet?.locked ?? false; restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { lockSwitch.checked = card.pet?.locked ?? false; } }
                        }

                        QQC2.Switch {
                            id: keepAboveSwitch
                            Kirigami.FormData.label: qsTr("Layer:")
                            enabled: card.behavioursEnabled
                            text: qsTr("Keep above the windows")
                            onToggled: Daemon.updatePet(card.petId, {keepAbove: checked})
                            Binding { target: keepAboveSwitch; property: "checked"; value: card.pet?.keepAbove ?? true; restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { keepAboveSwitch.checked = card.pet?.keepAbove ?? true; } }
                        }

                        QQC2.Label {
                            Kirigami.FormData.label: ""
                            visible: !card.layerApplied
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                            wrapMode: Text.Wrap
                            opacity: 0.7
                            font: Kirigami.Theme.smallFont
                            text: qsTr("Saved with the pet, but the overlay does not use the layer setting yet.")
                        }

                        QQC2.ComboBox {
                            id: underCursorBox
                            Kirigami.FormData.label: qsTr("Under the cursor:")
                            enabled: card.behavioursEnabled
                            textRole: "text"
                            valueRole: "value"
                            // hover hide and focus fade exclude each other, as in Anima: one choice
                            model: [
                                {text: qsTr("Nothing"), value: "none"},
                                {text: qsTr("Hide"), value: "hide"},
                                {text: qsTr("Fade and let the cursor through"), value: "fade"}
                            ]
                            function valueOf(pet) { return pet?.hoverHide ? "hide" : pet?.focusFade ? "fade" : "none"; }
                            onActivated: Daemon.updatePet(card.petId, {hoverHide: currentValue === "hide", focusFade: currentValue === "fade"})
                            Binding { target: underCursorBox; property: "currentIndex"; value: underCursorBox.indexOfValue(underCursorBox.valueOf(card.pet)); restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { underCursorBox.currentIndex = underCursorBox.indexOfValue(underCursorBox.valueOf(card.pet)); } }
                        }

                        QQC2.Switch {
                            id: dodgeSwitch
                            Kirigami.FormData.label: qsTr("Dodge:")
                            enabled: card.behavioursEnabled
                            text: qsTr("Step away from the cursor")
                            onToggled: Daemon.updatePet(card.petId, {smartDodge: checked})
                            Binding { target: dodgeSwitch; property: "checked"; value: card.pet?.smartDodge ?? false; restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { dodgeSwitch.checked = card.pet?.smartDodge ?? false; } }
                        }

                        RowLayout {
                            Kirigami.FormData.label: qsTr("Rotation:")
                            spacing: Kirigami.Units.largeSpacing
                            enabled: card.behavioursEnabled

                            QQC2.Slider {
                                id: rotationSlider
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                from: 0
                                to: 359
                                stepSize: 1
                                snapMode: QQC2.Slider.SnapAlways
                                onMoved: Daemon.queuePetUpdate(card.petId, {rotation: value})
                                Binding {
                                    target: rotationSlider; property: "value"; value: Math.min(359, card.pet?.rotation ?? 0)
                                    when: card.settled && !rotationSlider.pressed; restoreMode: Binding.RestoreNone
                                }
                            }
                            QQC2.Label {
                                Layout.minimumWidth: Kirigami.Units.gridUnit * 3
                                text: qsTr("%1°").arg(Math.round(rotationSlider.value))
                            }
                        }

                        RowLayout {
                            Kirigami.FormData.label: qsTr("Flip:")
                            spacing: Kirigami.Units.largeSpacing
                            enabled: card.behavioursEnabled

                            QQC2.CheckBox {
                                id: flipHBox
                                text: qsTr("Left to right")
                                onToggled: Daemon.updatePet(card.petId, {flipH: checked})
                                Binding { target: flipHBox; property: "checked"; value: card.pet?.flipH ?? false; restoreMode: Binding.RestoreNone }
                                Connections { target: Daemon; function onError() { flipHBox.checked = card.pet?.flipH ?? false; } }
                            }
                            QQC2.CheckBox {
                                id: flipVBox
                                text: qsTr("Top to bottom")
                                onToggled: Daemon.updatePet(card.petId, {flipV: checked})
                                Binding { target: flipVBox; property: "checked"; value: card.pet?.flipV ?? false; restoreMode: Binding.RestoreNone }
                                Connections { target: Daemon; function onError() { flipVBox.checked = card.pet?.flipV ?? false; } }
                            }
                        }

                        QQC2.Label {
                            Kirigami.FormData.label: ""
                            visible: !card.transformApplied
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                            wrapMode: Text.Wrap
                            opacity: 0.7
                            font: Kirigami.Theme.smallFont
                            text: qsTr("Saved with the pet, but the overlay does not draw rotation and flips yet.")
                        }

                        QQC2.Label {
                            Kirigami.FormData.label: ""
                            visible: !card.behavioursEnabled
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                            wrapMode: Text.Wrap
                            opacity: 0.7
                            font: Kirigami.Theme.smallFont
                            text: qsTr("The running daemon is too old for this; restart it after updating.")
                        }

                        // The sound of the pet (API revision 15): off until a file is chosen and a mode is set.
                        Kirigami.Separator {
                            Kirigami.FormData.isSection: true
                            Kirigami.FormData.label: qsTr("Sound")
                        }

                        PetSound {
                            Kirigami.FormData.label: ""
                            petId: card.petId
                            pet: card.pet
                            settled: card.settled
                            fromVideo: page.generated
                            itemId: page.itemId
                            overlayWindow: card.overlayWindow
                        }

                        // The states of the pet and its Mouse Tracking (API revision 19, docs/pets/states.md): off until chosen.
                        Kirigami.Separator {
                            Kirigami.FormData.isSection: true
                            Kirigami.FormData.label: qsTr("States and mouse")
                        }

                        PetStates {
                            Kirigami.FormData.label: ""
                            petId: card.petId
                            pet: card.pet
                            settled: card.settled
                            variantChoices: page.variantChoices
                            overlayWindow: card.overlayWindow
                        }

                        // The fullscreen mode (API revision 17, docs/pets/stage.md): the pet as a transparent animation across a screen or all of them.
                        Kirigami.Separator {
                            Kirigami.FormData.isSection: true
                            Kirigami.FormData.label: qsTr("Fullscreen")
                        }

                        QQC2.ComboBox {
                            id: stageModeBox
                            Kirigami.FormData.label: qsTr("Show as:")
                            enabled: Daemon.petStageSupported
                            textRole: "text"
                            valueRole: "value"
                            model: [
                                {text: qsTr("A pet"), value: "off"},
                                {text: qsTr("Across this screen"), value: "output"},
                                {text: qsTr("Across all screens"), value: "desktop"}
                            ]
                            function valueOf(pet) { return pet?.stage?.mode ?? "off"; }
                            onActivated: Daemon.updatePet(card.petId, {stage: {mode: currentValue}})
                            Binding { target: stageModeBox; property: "currentIndex"; value: stageModeBox.indexOfValue(stageModeBox.valueOf(card.pet)); restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { stageModeBox.currentIndex = stageModeBox.indexOfValue(stageModeBox.valueOf(card.pet)); } }
                            QQC2.ToolTip.visible: hovered && !enabled
                            QQC2.ToolTip.text: qsTr("The running daemon is too old for this; restart it after updating.")
                        }

                        QQC2.ComboBox {
                            id: stageFitBox
                            Kirigami.FormData.label: qsTr("Fit:")
                            visible: card.stageOn
                            textRole: "text"
                            valueRole: "value"
                            model: [
                                {text: qsTr("Whole picture, empty bands if the shape differs"), value: "contain"},
                                {text: qsTr("Fill the screen, cut the excess"), value: "cover"},
                                {text: qsTr("Stretch to the screen"), value: "stretch"}
                            ]
                            function valueOf(pet) { return pet?.stage?.fit ?? "contain"; }
                            onActivated: Daemon.updatePet(card.petId, {stage: {fit: currentValue}})
                            Binding { target: stageFitBox; property: "currentIndex"; value: stageFitBox.indexOfValue(stageFitBox.valueOf(card.pet)); restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { stageFitBox.currentIndex = stageFitBox.indexOfValue(stageFitBox.valueOf(card.pet)); } }
                        }

                        QQC2.ComboBox {
                            id: stageOverBox
                            Kirigami.FormData.label: qsTr("Fullscreen windows:")
                            visible: card.stageOn
                            textRole: "text"
                            valueRole: "value"
                            model: [
                                {text: qsTr("Hide it, as the setting says"), value: "inherit"},
                                {text: qsTr("Always show it"), value: "show"},
                                {text: qsTr("Always hide it"), value: "hide"}
                            ]
                            function valueOf(pet) { return pet?.stage?.overFullscreen ?? "inherit"; }
                            onActivated: Daemon.updatePet(card.petId, {stage: {overFullscreen: currentValue}})
                            Binding { target: stageOverBox; property: "currentIndex"; value: stageOverBox.indexOfValue(stageOverBox.valueOf(card.pet)); restoreMode: Binding.RestoreNone }
                            Connections { target: Daemon; function onError() { stageOverBox.currentIndex = stageOverBox.indexOfValue(stageOverBox.valueOf(card.pet)); } }
                        }

                        QQC2.Label {
                            Kirigami.FormData.label: ""
                            visible: card.stageOn
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            opacity: 0.7
                            font: Kirigami.Theme.smallFont
                            text: qsTr("It never takes the mouse or the keyboard. The position, size and pointer settings above are kept and come back when it is a pet again. The layer switch above puts it below the windows. One animation per screen.")
                        }

                        Repeater {
                            model: card.stageNotes
                            delegate: QQC2.Label {
                                required property string modelData
                                Kirigami.FormData.label: ""
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                                font: Kirigami.Theme.smallFont
                                text: modelData
                            }
                        }

                        ColumnLayout {
                            Kirigami.FormData.label: qsTr("Quality:")
                            spacing: 0

                            QQC2.Switch {
                                id: originalSwitch
                                text: qsTr("Play original (heavy)")
                                onToggled: Daemon.updatePet(card.petId, {playOriginal: checked})
                                Binding {
                                    target: originalSwitch; property: "checked"; value: card.pet?.playOriginal ?? false
                                    restoreMode: Binding.RestoreNone
                                }
                                Connections {
                                    target: Daemon
                                    function onError() { originalSwitch.checked = card.pet?.playOriginal ?? false; }
                                }
                            }
                            QQC2.Label {
                                Layout.leftMargin: originalSwitch.indicator.width + originalSwitch.spacing
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: qsTr("Uses much more CPU")
                            }
                        }
                    }
                }
            }
        }
    }
}
