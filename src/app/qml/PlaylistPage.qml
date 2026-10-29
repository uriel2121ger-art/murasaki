import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

Kirigami.ScrollablePage {
    id: page

    required property string playlistId

    readonly property var playlist: (Daemon.playlists ?? []).find(p => p.id === playlistId) ?? null
    readonly property bool loaded: playlist !== null
    readonly property var items: playlist ? (playlist.items ?? []) : []
    // The assignments that play this playlist, and what the first of them shows now.
    readonly property var playingOn: (Daemon.assignments ?? []).filter(a => a.itemId === "playlist:" + playlistId)
    // One wallpaper across all screens: offered for "All screens" when there is more than one.
    readonly property bool spanAvailable: target.output === "*" && target.outputNames.length > 1
    property string nowTitle: ""
    property int nowIndex: -1

    title: loaded ? playlist.name : qsTr("Playlist")

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    function fileUrl(path) {
        return path ? "file://" + String(path).split("/").map(encodeURIComponent).join("/") : "";
    }

    // What the library knows about an item id; the library count is a dependency so rows refresh when it loads.
    function entryOf(itemId) {
        Library.count;
        const row = Library.indexOf(itemId);
        return row >= 0 ? Library.get(row) : null;
    }

    function setItems(list) {
        Daemon.updatePlaylist(playlistId, {items: list});
    }

    function move(index, delta) {
        const list = items.slice();
        const target = index + delta;
        if (target < 0 || target >= list.length)
            return;
        [list[index], list[target]] = [list[target], list[index]];
        setItems(list);
    }

    function removeAt(index) {
        const list = items.slice();
        list.splice(index, 1);
        setItems(list);
    }

    // Puts the controls back to the daemon's values (after PlaylistsChanged, or a refused edit).
    function syncControls() {
        if (playlist === null) // not `loaded`: that binding may not have followed the playlist yet
            return;
        orderBox.currentIndex = Math.max(0, orderBox.indexOfValue(playlist.order ?? "sequential"));
        if (!intervalTimer.running && !intervalBox.activeFocus)
            intervalBox.value = playlist.intervalMinutes ?? 15;
        loginSwitch.checked = playlist.changeOnLogin ?? false;
    }

    function requery() {
        if (playingOn.length > 0)
            Daemon.resolveAssignment(playingOn[0].output, playingOn[0].desktop);
        else {
            nowTitle = "";
            nowIndex = -1;
        }
    }

    onPlaylistChanged: syncControls()
    onPlayingOnChanged: requery()
    Component.onCompleted: {
        syncControls();
        requery();
    }

    // Leaving the page with a change still waiting out its debounce must not lose it.
    Component.onDestruction: {
        if (intervalTimer.running) {
            intervalTimer.stop();
            intervalTimer.triggered();
        }
    }

    // A playlist that was deleted (here or elsewhere) takes its page with it.
    Timer {
        id: goneTimer
        interval: 1500
        running: Daemon.available && Daemon.playlistsAvailable && !page.loaded
        onTriggered: if (!page.loaded && applicationWindow().pageStack.currentItem === page) applicationWindow().pageStack.pop()
    }

    Timer {
        id: intervalTimer
        interval: 400
        onTriggered: Daemon.updatePlaylist(page.playlistId, {intervalMinutes: intervalBox.value})
    }

    Connections {
        target: Daemon
        function onAssignmentResolved(output, desktop, assignment, item, playlist) {
            if (page.playingOn.length === 0 || output !== page.playingOn[0].output || desktop !== page.playingOn[0].desktop)
                return;
            page.nowTitle = playlist.id === page.playlistId ? (item.title ?? "") : "";
            page.nowIndex = playlist.id === page.playlistId ? (playlist.index ?? -1) : -1;
        }
        // A rotation announces AssignmentsChanged.
        function onAssignmentsChanged() { page.requery(); }
        function onError() { page.syncControls(); }
    }

    TargetState {
        id: target
    }

    PlaylistNameDialog {
        id: renameDialog
        title: qsTr("Rename playlist")
        onNamed: name => Daemon.updatePlaylist(page.playlistId, {name: name})
    }

    KirigamiDialogs.PromptDialog {
        id: deleteDialog
        title: qsTr("Delete playlist")
        subtitle: qsTr("The wallpapers stay in your library.")
        standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel
        onAccepted: Daemon.deletePlaylist(page.playlistId)
    }

    // Library picker for "Add wallpapers".
    KirigamiDialogs.Dialog {
        id: pickDialog

        property var picked: ({})
        property int pickedCount: 0

        title: qsTr("Add wallpapers")
        standardButtons: KirigamiDialogs.Dialog.Ok | KirigamiDialogs.Dialog.Cancel
        preferredWidth: Kirigami.Units.gridUnit * 24
        preferredHeight: Kirigami.Units.gridUnit * 26
        onAboutToShow: {
            picked = ({});
            pickedCount = 0;
        }
        onAccepted: {
            const ids = Object.keys(picked).filter(id => picked[id]);
            if (ids.length > 0)
                Daemon.addToPlaylist(page.playlistId, ids);
        }

        ListView {
            id: pickList
            clip: true
            implicitHeight: Kirigami.Units.gridUnit * 20
            model: Library
            delegate: QQC2.CheckDelegate {
                id: pick

                required property string itemId
                required property string title
                required property string kind
                required property string status

                readonly property bool offered: kind !== "pet" && !page.items.includes(itemId)

                width: ListView.view.width
                visible: offered
                height: offered ? implicitHeight : 0
                text: title
                checked: pickDialog.picked[itemId] === true
                onToggled: {
                    pickDialog.picked[itemId] = checked;
                    pickDialog.pickedCount = Object.keys(pickDialog.picked).filter(id => pickDialog.picked[id]).length;
                }
            }
            QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: !page.loaded
            icon.name: "dialog-warning"
            text: Daemon.available ? qsTr("This playlist no longer exists") : qsTr("The Murasaki service is not running")
        }

        RowLayout {
            Layout.fillWidth: true
            visible: page.loaded

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 1
                text: page.loaded ? page.playlist.name : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            QQC2.Button {
                icon.name: "edit-rename"
                text: qsTr("Rename")
                onClicked: {
                    renameDialog.initialName = page.playlist.name;
                    renameDialog.open();
                }
            }
            QQC2.Button {
                icon.name: "edit-delete"
                text: qsTr("Delete")
                onClicked: deleteDialog.open()
            }
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: page.loaded

            QQC2.ComboBox {
                id: orderBox
                Kirigami.FormData.label: qsTr("Order:")
                textRole: "text"
                valueRole: "value"
                model: [
                    {text: qsTr("In order"), value: "sequential"},
                    {text: qsTr("Shuffle"), value: "shuffle"}
                ]
                onActivated: Daemon.updatePlaylist(page.playlistId, {order: currentValue})
            }

            QQC2.SpinBox {
                id: intervalBox
                Kirigami.FormData.label: qsTr("Change every:")
                from: 1
                to: 1440
                editable: true
                textFromValue: (value, locale) => qsTr("%n minute(s)", "", value)
                valueFromText: (text, locale) => parseInt(text) || 15
                onValueModified: intervalTimer.restart()
            }

            QQC2.Switch {
                id: loginSwitch
                Kirigami.FormData.label: qsTr("On login:")
                text: qsTr("Start with the next wallpaper")
                onToggled: Daemon.updatePlaylist(page.playlistId, {changeOnLogin: checked})
            }
        }

        // Now playing, with Next and Previous.
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: page.loaded && page.playingOn.length > 0
            type: Kirigami.MessageType.Information
            text: (page.nowTitle !== "" ? qsTr("Showing: %1").arg(page.plain(page.nowTitle)) + "\n" : "")
                + qsTr("Playing on: %1").arg(page.playingOn.map(a => page.plain(target.nameOf(a.output, a.desktop))).join(", "))
            actions: [
                Kirigami.Action {
                    icon.name: "media-skip-backward"
                    text: qsTr("Previous")
                    onTriggered: {
                        for (const a of page.playingOn)
                            Daemon.previousInPlaylist(a.output, a.desktop);
                    }
                },
                Kirigami.Action {
                    icon.name: "media-skip-forward"
                    text: qsTr("Next")
                    onTriggered: {
                        for (const a of page.playingOn)
                            Daemon.nextInPlaylist(a.output, a.desktop);
                    }
                },
                Kirigami.Action {
                    icon.name: "media-playback-stop"
                    text: qsTr("Stop")
                    onTriggered: {
                        for (const a of page.playingOn)
                            Daemon.clearAssignment(a.output, a.desktop);
                    }
                }
            ]
        }

        RowLayout {
            Layout.fillWidth: true
            visible: page.loaded

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 2
                text: qsTr("Wallpapers")
            }
            QQC2.Button {
                icon.name: "list-add"
                text: qsTr("Add wallpapers…")
                onClicked: pickDialog.open()
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: page.loaded && page.items.length === 0
            opacity: 0.7
            wrapMode: Text.Wrap
            text: qsTr("The playlist is empty. Add wallpapers here, or with “Add to playlist” on a wallpaper.")
        }

        Repeater {
            model: page.items

            delegate: RowLayout {
                id: entry

                required property int index
                required property string modelData

                readonly property var info: page.entryOf(modelData)

                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                Rectangle {
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 6
                    Layout.preferredHeight: Layout.preferredWidth * 9 / 16
                    color: Kirigami.Theme.alternateBackgroundColor
                    radius: Kirigami.Units.cornerRadius
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: entry.info ? entry.info.preview : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 240
                        sourceSize.height: 135
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    QQC2.Label {
                        Layout.fillWidth: true
                        text: entry.info ? entry.info.title : entry.modelData
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: entry.info === null || entry.info.status !== "ok"
                        color: Kirigami.Theme.neutralTextColor
                        text: entry.info === null ? qsTr("Not in the library: skipped") : qsTr("Cannot be shown: skipped")
                        font: Kirigami.Theme.smallFont
                    }
                }

                QQC2.ToolButton {
                    icon.name: "go-up"
                    enabled: entry.index > 0
                    onClicked: page.move(entry.index, -1)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: qsTr("Move up")
                }
                QQC2.ToolButton {
                    icon.name: "go-down"
                    enabled: entry.index < page.items.length - 1
                    onClicked: page.move(entry.index, 1)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: qsTr("Move down")
                }
                QQC2.ToolButton {
                    icon.name: "list-remove"
                    onClicked: page.removeAt(entry.index)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: qsTr("Remove from the playlist")
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: page.loaded
        }

        Kirigami.Heading {
            level: 2
            visible: page.loaded
            text: qsTr("Apply")
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: page.loaded

            QQC2.ComboBox {
                Kirigami.FormData.label: qsTr("Screen:")
                textRole: "text"
                valueRole: "value"
                model: target.outputChoices
                onActivated: target.outputWanted = currentValue
                onModelChanged: currentIndex = Math.max(0, indexOfValue(target.output))
                Binding on currentIndex {
                    value: Math.max(0, target.outputChoices.findIndex(c => c.value === target.output))
                }
            }

            QQC2.ComboBox {
                Kirigami.FormData.label: qsTr("Desktop:")
                visible: target.desktopList.length > 1 || target.desktop !== "*"
                textRole: "text"
                valueRole: "value"
                model: target.desktopChoices
                onActivated: target.desktopWanted = currentValue
                onModelChanged: currentIndex = Math.max(0, indexOfValue(target.desktop))
                Binding on currentIndex {
                    value: Math.max(0, target.desktopChoices.findIndex(c => c.value === target.desktop))
                }
            }

            QQC2.ComboBox {
                id: fillBox
                Kirigami.FormData.label: qsTr("Fill mode:")
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
            }

            QQC2.Switch {
                id: spanSwitch
                Kirigami.FormData.label: qsTr("Span:")
                text: qsTr("Span across all screens")
                visible: page.spanAvailable

                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: qsTr("Draw each wallpaper of the playlist over all screens as a single picture, instead of one on each")
            }

            QQC2.Button {
                icon.name: "dialog-ok-apply"
                text: qsTr("Apply")
                enabled: page.loaded && page.items.length > 0
                onClicked: Daemon.setAssignment(target.output, target.desktop, "playlist:" + page.playlistId,
                                                {fill: fillBox.currentValue, muted: muteSwitch.checked,
                                                 span: page.spanAvailable && spanSwitch.checked})
            }
        }
    }
}
