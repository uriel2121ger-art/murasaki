import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

Kirigami.ScrollablePage {
    id: page

    required property string itemId

    property var item: ({})
    property string loadError: ""
    // Whether the chosen target currently plays this item (edits then apply live).
    property bool targetPlaysItem: false
    // The item plays on the chosen target only through a broader assignment (a wildcard key): the entry, or null.
    property var broaderAssignment: null
    // The target plays a playlist that is showing this item right now: the playlist entry, or null. Edits here do
    // not apply live (that would replace the playlist); Apply does, on purpose.
    property var playlistHere: null
    // Set when the next ResolveAssignment answer should prefill the form (the target changed).
    property bool prefillPending: false
    property bool forceReload: false
    // The property model is loaded once per target; remember what it was loaded with.
    property bool modelLoaded: false
    property string loadedOverrides: ""
    property var outputNames: []
    property var desktopList: []
    // What the user picked; the effective target falls back to "*" while the pick is not available.
    property string outputWanted: "*"
    property string desktopWanted: "*"

    readonly property bool loaded: item.id !== undefined
    readonly property string kind: item.kind ?? ""
    // The switch shows what the daemon reported when the item loaded, then what the user chose.
    property bool networkAllowed: false
    readonly property bool hasMedia: kind === "video" || kind === "web"
    readonly property bool isScene: kind === "scene"
    // Scenes take the fill mode too: the engine starts with it (--scaling).
    readonly property bool hasFill: hasMedia || isScene
    readonly property bool canApply: loaded && (item.status ?? "ok") === "ok" && kind !== "pet" && kind !== "asset"
    readonly property var outputChoices: [{text: qsTr("All screens"), value: "*"}].concat(
        outputNames.map(n => ({text: plain(n), value: n})))
    readonly property var desktopChoices: [{text: qsTr("All desktops"), value: "*"}].concat(
        desktopList.map(d => ({text: plain(d.name) || qsTr("Desktop %1").arg(d.number ?? ""), value: d.id})))
    // One wallpaper across all screens: offered for "All screens" when there is more than one.
    readonly property bool spanAvailable: targetOutput === "*" && outputNames.length > 1
    readonly property string targetOutput: outputChoices.some(c => c.value === outputWanted) ? outputWanted : "*"
    readonly property string targetDesktop: desktopChoices.some(c => c.value === desktopWanted) ? desktopWanted : "*"
    // Narrower settings inside the chosen target: they keep their own wallpaper and win over this one.
    // A setting for a desktop that no longer exists never applies; that can only be told once the desktops are known.
    // The daemon client works both lists out (also for the Workshop's "Use now").
    readonly property var knownDesktopIds: desktopList.map(d => d.id)
    readonly property var overridingAssignments: (Daemon.assignments, Daemon.overridingAssignments(targetOutput, targetDesktop, knownDesktopIds))
    // Those leftovers inside the target (only an "all desktops" target reaches desktops by itself): replacing clears them too.
    readonly property var orphanAssignments: (Daemon.assignments, Daemon.orphanAssignments(targetOutput, targetDesktop, knownDesktopIds))

    title: loaded ? item.title : qsTr("Wallpaper")

    // Item files and compositor names are untrusted, and Kirigami messages, chips and combo
    // delegates auto-detect rich text: replace angle brackets so none of it can ever parse as markup.
    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "\u2039").replace(/>/g, "\u203a");
    }

    function fileUrl(path) {
        return path ? "file://" + String(path).split("/").map(encodeURIComponent).join("/") : "";
    }

    function targetName(output, desktop) {
        const screen = output === "*" ? qsTr("All screens") : output;
        const desk = desktop === "*" ? qsTr("All desktops")
            : (desktopList.find(d => d.id === desktop)?.name ?? qsTr("(removed desktop)"));
        return desktop === "*" ? screen : qsTr("%1, %2").arg(screen).arg(desk);
    }

    // The chosen target changed (or the item arrived): drop live state and unsaved edits, then read the target.
    function targetChanged() {
        targetPlaysItem = false;
        broaderAssignment = null;
        playlistHere = null;
        liveTimer.stop();
        prefillPending = true;
        forceReload = true;
        requery();
    }

    // Asks what plays on the chosen target; the reply updates targetPlaysItem and broaderAssignment. Does not touch
    // pending live edits, so the page's own Apply (which announces AssignmentsChanged) cannot drop one.
    function requery() {
        Daemon.resolveAssignment(targetOutput, targetDesktop);
    }

    // Loads the property model, unless it already holds exactly these overrides (a target change always reloads).
    function loadModel(overrides) {
        const json = JSON.stringify(overrides);
        const force = forceReload;
        forceReload = false;
        if (!force && modelLoaded && json === loadedOverrides)
            return;
        modelLoaded = true;
        loadedOverrides = json;
        propertyModel.load(item, overrides);
    }

    function apply() {
        const options = {properties: propertyModel.overrides()};
        if (hasFill)
            options.fill = fillBox.currentValue;
        if (hasMedia)
            options.muted = muteSwitch.checked;
        if (spanAvailable && spanSwitch.checked)
            options.span = true;
        Daemon.setAssignment(targetOutput, targetDesktop, itemId, options);
        targetPlaysItem = true;
        broaderAssignment = null;
        playlistHere = null;
    }

    function refreshOutputs() {
        const names = Surface.outputs.map(o => o.name);
        if (JSON.stringify(names) !== JSON.stringify(outputNames))
            outputNames = names;
        const desktops = Surface.desktops.map(d => ({id: d.id, name: d.name}));
        if (JSON.stringify(desktops) !== JSON.stringify(desktopList))
            desktopList = desktops;
    }

    onTargetOutputChanged: if (loaded) targetChanged()
    onTargetDesktopChanged: if (loaded) targetChanged()

    Component.onCompleted: {
        refreshOutputs();
        Daemon.getItem(itemId);
    }

    AddToPlaylist {
        id: addToPlaylist
    }

    PropertyModel {
        id: propertyModel
        onEdited: liveTimer.restart()
    }

    // Live edits: only when this item already plays on the chosen target.
    Timer {
        id: liveTimer
        interval: 150
        onTriggered: {
            if (page.targetPlaysItem && page.canApply)
                page.apply();
        }
    }

    Timer {
        id: fallbackLoad
        interval: 1000
        onTriggered: page.loadModel({})
    }

    Connections {
        target: Daemon

        function onItemLoaded(id, loaded) {
            if (id !== page.itemId)
                return;
            page.item = loaded;
            page.networkAllowed = (loaded.permissions ?? {}).network === true;
            page.loadError = "";
            page.modelLoaded = false;
            // The model loads when the target resolves (once); the timer covers an answer that never comes.
            fallbackLoad.restart();
            page.targetChanged();
        }

        function onItemLoadFailed(id, message) {
            if (id === page.itemId)
                page.loadError = message;
        }

        function onAssignmentResolved(output, desktop, assignment, resolvedItem, playlist) {
            if (output !== page.targetOutput || desktop !== page.targetDesktop)
                return;
            // A playlist showing this item is not this item being assigned: the assignment names the item that
            // shows, but a live edit would overwrite the playlist.
            const viaPlaylist = playlist !== undefined && playlist.id !== undefined;
            page.playlistHere = viaPlaylist && assignment.itemId === page.itemId
                ? (Daemon.playlists ?? []).find(p => p.id === playlist.id) ?? {name: ""} : null;
            const here = assignment.itemId === page.itemId && !viaPlaylist;
            // Live edits need an assignment for exactly this target; a wildcard match would be a different key.
            const exact = here && assignment.output === output && assignment.desktop === desktop;
            page.targetPlaysItem = exact;
            page.broaderAssignment = here && !exact ? assignment : null;
            if (page.prefillPending) {
                page.prefillPending = false;
                fallbackLoad.stop();
                if (here) {
                    fillBox.currentIndex = Math.max(0, fillBox.indexOfValue(assignment.fill ?? "fill"));
                    muteSwitch.checked = assignment.muted ?? false;
                    spanSwitch.checked = assignment.span ?? false;
                    page.loadModel(assignment.properties ?? {});
                } else {
                    spanSwitch.checked = false;
                    page.loadModel({});
                }
            }
        }

        function onAssignmentsChanged() {
            if (page.loaded)
                page.requery();
        }
    }

    Connections {
        target: Surface
        function onOutputsChanged() { page.refreshOutputs(); }
        function onDesktopsChanged() { page.refreshOutputs(); }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.loadError !== ""
            type: Kirigami.MessageType.Error
            text: qsTr("Could not load this wallpaper: %1").arg(page.plain(page.loadError))
        }

        // The screens that show this wallpaper and could not draw it.
        Repeater {
            model: page.loaded ? (Surface.outputs, Surface.outputs.filter(o => o.itemId === page.itemId && o.failure && o.failure.kind)) : []
            EngineFailureMessage {
                required property var modelData
                output: modelData.name
                failure: modelData.failure
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.loaded && page.kind === "asset"
            type: Kirigami.MessageType.Information
            text: qsTr("Asset pack (used by the Wallpaper Engine editor, not a wallpaper)")
        }

        Kirigami.InlineMessage {
            id: dependencyMessage
            Layout.fillWidth: true
            visible: page.loaded && page.item.status === "missing-dependency"
            type: Kirigami.MessageType.Warning
            text: qsTr("Requires Workshop item %1").arg(page.plain(page.item.statusDetail))
            actions: [
                Kirigami.Action {
                    icon.name: "internet-services"
                    text: qsTr("Open in Workshop")
                    onTriggered: applicationWindow().openWorkshop(
                        "https://steamcommunity.com/sharedfiles/filedetails/?id=" + page.item.statusDetail)
                }
            ]
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.loaded && page.item.status === "invalid"
            type: Kirigami.MessageType.Error
            text: qsTr("This wallpaper is invalid: %1").arg(page.plain(page.item.statusDetail))
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
            visible: page.loaded && page.isScene && !SystemControl.sceneEngineAvailable()
            type: Kirigami.MessageType.Warning
            text: qsTr("The scene engine is not installed. Install Murasaki again to get it.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.broaderAssignment !== null
            type: Kirigami.MessageType.Information
            text: page.broaderAssignment === null ? "" : qsTr("Playing here through '%1' — Apply to make a separate setting for this target")
                .arg(page.plain(page.targetName(page.broaderAssignment.output, page.broaderAssignment.desktop)))
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.playlistHere !== null
            type: Kirigami.MessageType.Information
            text: page.playlistHere === null ? "" : qsTr("Playing here as part of the playlist '%1' — Apply to play only this wallpaper")
                .arg(page.plain(page.playlistHere.name))
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.canApply && page.overridingAssignments.length > 0
            type: Kirigami.MessageType.Warning
            text: qsTr("%n screen or desktop setting(s) inside this target keep their own wallpaper and take priority over it:", "", page.overridingAssignments.length)
                + " " + page.overridingAssignments.map(a => page.plain(page.targetName(a.output, a.desktop))).join(", ")
            actions: [
                Kirigami.Action {
                    icon.name: "edit-clear"
                    text: qsTr("Replace them")
                    onTriggered: {
                        for (const a of page.overridingAssignments.concat(page.orphanAssignments))
                            Daemon.clearAssignment(a.output, a.desktop);
                    }
                }
            ]
        }

        // Preview, with the title beside it on wide pages.
        GridLayout {
            Layout.fillWidth: true
            columns: page.width > Kirigami.Units.gridUnit * 40 ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing

            Rectangle {
                // Sized from the page, not from its own width, so the layout cannot collapse it.
                Layout.preferredWidth: Math.min(Kirigami.Units.gridUnit * 30, page.width - Kirigami.Units.gridUnit * 4)
                Layout.preferredHeight: Layout.preferredWidth * 9 / 16
                Layout.alignment: Qt.AlignTop
                color: Kirigami.Theme.alternateBackgroundColor
                radius: Kirigami.Units.cornerRadius
                clip: true

                Loader {
                    anchors.fill: parent
                    active: page.loaded && page.item.preview !== ""
                    sourceComponent: String(page.item.preview).toLowerCase().endsWith(".gif") ? animated : still
                }

                Component {
                    id: still
                    Image {
                        source: page.fileUrl(page.item.preview)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 960
                        sourceSize.height: 540
                    }
                }
                Component {
                    id: animated
                    AnimatedImage {
                        source: page.fileUrl(page.item.preview)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        playing: true
                    }
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
                    text: {
                        const kinds = {video: qsTr("Video"), web: qsTr("Web"), scene: qsTr("Scene"), pet: qsTr("Pet"), asset: qsTr("Asset pack")};
                        const parts = [kinds[page.kind] ?? qsTr("Unknown")];
                        if (page.item.contentRating)
                            parts.push(qsTr("Rating: %1").arg(page.item.contentRating));
                        return parts.join(" · ");
                    }
                }

                // The engine that will draw a scene, and why it is not the Vulkan one when that was asked for.
                QQC2.Label {
                    visible: page.loaded && page.isScene
                    opacity: 0.7
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    text: {
                        if (!page.loaded || !page.isScene)
                            return "";
                        SystemControl.sceneEngineRevision; // evaluated again when a background check finishes
                        const label = SystemControl.sceneEngineLabel(Daemon.settings.sceneEngine ?? "auto", page.item.contentDir,
                                                                      Daemon.assetsDir);
                        return qsTr("Engine: %1").arg(label !== "" ? label : qsTr("not installed"));
                    }
                }

                // A web page can only use the internet when the user says so, per wallpaper.
                QQC2.Switch {
                    id: networkSwitch
                    visible: page.loaded && page.kind === "web" && Daemon.available && Daemon.apiVersion >= 8
                    text: qsTr("This wallpaper can use the internet")
                    checked: page.networkAllowed
                    onToggled: {
                        page.networkAllowed = checked;
                        Daemon.setItemPermission(page.itemId, "network", checked);
                    }
                }

                QQC2.Label {
                    visible: networkSwitch.visible
                    Layout.fillWidth: true
                    opacity: 0.7
                    wrapMode: Text.Wrap
                    text: qsTr("Off by default. Web wallpapers that show data such as weather or news need it. Allowed pages can also track you or use your computer to mine; only enable it for wallpapers you trust.")
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
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        // Apply section.
        Kirigami.Heading {
            level: 2
            text: qsTr("Apply")
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            enabled: page.loaded

            QQC2.ComboBox {
                id: outputBox
                Kirigami.FormData.label: qsTr("Screen:")
                textRole: "text"
                valueRole: "value"
                model: page.outputChoices
                onActivated: page.outputWanted = currentValue
                // A new model resets the selection: restore the picked output while it is still there.
                onModelChanged: currentIndex = Math.max(0, indexOfValue(page.targetOutput))
                Binding on currentIndex {
                    value: Math.max(0, page.outputChoices.findIndex(c => c.value === page.targetOutput))
                }
            }

            QQC2.ComboBox {
                id: desktopBox
                Kirigami.FormData.label: qsTr("Desktop:")
                // With a single virtual desktop the choice means nothing and only invites stray settings.
                visible: page.desktopList.length > 1 || page.targetDesktop !== "*"
                textRole: "text"
                valueRole: "value"
                model: page.desktopChoices
                onActivated: page.desktopWanted = currentValue
                onModelChanged: currentIndex = Math.max(0, indexOfValue(page.targetDesktop))
                Binding on currentIndex {
                    value: Math.max(0, page.desktopChoices.findIndex(c => c.value === page.targetDesktop))
                }
            }

            QQC2.ComboBox {
                id: fillBox
                Kirigami.FormData.label: qsTr("Fill mode:")
                visible: page.hasFill
                textRole: "text"
                valueRole: "value"
                model: [
                    {text: qsTr("Fill (crop)"), value: "fill"},
                    {text: qsTr("Fit (letterbox)"), value: "fit"},
                    {text: qsTr("Stretch"), value: "stretch"}
                ]
            }

            QQC2.Switch {
                id: muteSwitch
                Kirigami.FormData.label: qsTr("Mute:")
                visible: page.hasMedia
            }

            QQC2.Switch {
                id: spanSwitch
                Kirigami.FormData.label: qsTr("Span:")
                text: qsTr("Span across all screens")
                visible: page.spanAvailable
                onToggled: liveTimer.restart()

                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: qsTr("Draw one wallpaper over all screens as a single picture, instead of one on each")
            }

            RowLayout {
                spacing: Kirigami.Units.smallSpacing

                QQC2.Button {
                    id: applyButton
                    icon.name: "dialog-ok-apply"
                    text: qsTr("Apply")
                    highlighted: page.canApply && !page.targetPlaysItem
                    enabled: page.canApply
                    onClicked: page.apply()

                    QQC2.ToolTip.visible: !enabled && hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: page.kind === "pet" ? qsTr("Pets cannot be set as a wallpaper") : qsTr("This wallpaper cannot be applied")
                }

                QQC2.Button {
                    icon.name: "media-playlist-append"
                    text: qsTr("Add to playlist")
                    enabled: page.canApply && Daemon.playlistsAvailable
                    onClicked: addToPlaylist.popup(page.itemId)

                    QQC2.ToolTip.visible: !Daemon.playlistsAvailable && hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: qsTr("Playlists need a newer Murasaki service")
                }
            }
        }

        // Live edits are not obvious: say when the properties below change the wallpaper on screen at once.
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.canApply && page.targetPlaysItem
            type: Kirigami.MessageType.Positive
            icon.name: "media-playback-start"
            text: propertyModel.count > 0 ? qsTr("Showing here now. Changes to the properties below apply at once.")
                                          : qsTr("Showing here now.")
        }

        // Properties section.
        RowLayout {
            Layout.fillWidth: true
            visible: propertyModel.count > 0

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 2
                text: qsTr("Properties")
            }

            QQC2.Button {
                icon.name: "edit-reset"
                text: qsTr("Reset to defaults")
                onClicked: propertyModel.resetToDefaults()
            }
        }

        PropertyEditor {
            Layout.fillWidth: true
            visible: propertyModel.count > 0
            model: propertyModel
        }
    }
}
