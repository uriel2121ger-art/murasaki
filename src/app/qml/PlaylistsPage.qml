import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

Kirigami.ScrollablePage {
    id: page

    title: qsTr("Playlists")

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    // How many assignments play the playlist.
    function playingCount(playlistId) {
        return (Daemon.assignments ?? []).filter(a => a.itemId === "playlist:" + playlistId).length;
    }

    // Bumped when the library changes, so the previews look their items up again.
    property int libraryRevision: 0

    Connections {
        target: Library
        function onCountChanged() { page.libraryRevision++; }
        function onModelReset() { page.libraryRevision++; }
    }

    // The previews of the first few wallpapers of a playlist that are still in the library.
    function previews(playlist, max) {
        libraryRevision;
        const out = [];
        for (const id of playlist.items ?? []) {
            const row = Library.indexOf(id);
            const preview = row >= 0 ? String(Library.get(row).preview ?? "") : "";
            if (preview !== "")
                out.push(preview);
            if (out.length === max)
                break;
        }
        return out;
    }

    function summary(playlist) {
        const order = playlist.order === "shuffle" ? qsTr("shuffle") : qsTr("in order");
        return qsTr("%n wallpaper(s)", "", (playlist.items ?? []).length) + " · "
            + qsTr("every %n minute(s)", "", playlist.intervalMinutes ?? 15) + " · " + order;
    }

    actions: [
        Kirigami.Action {
            id: newAction
            icon.name: "list-add"
            text: qsTr("New playlist…")
            enabled: Daemon.playlistsAvailable
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: nameDialog.open()
        }
    ]

    PlaylistNameDialog {
        id: nameDialog
        onNamed: name => Daemon.createPlaylist(name)
    }

    // The new playlist opens on its own page, ready to be filled.
    Connections {
        target: Daemon
        function onPlaylistCreated(name, id) {
            if (id !== "" && applicationWindow().section === "playlists")
                applicationWindow().openPlaylist(id);
        }
    }

    ListView {
        id: list

        model: Daemon.playlists
        clip: true

        delegate: QQC2.ItemDelegate {
            id: row

            required property var modelData

            width: ListView.view.width
            onClicked: applicationWindow().openPlaylist(modelData.id)

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                // A strip of its first previews, or the playlist icon while it is empty.
                Item {
                    id: strip
                    readonly property var shown: page.previews(row.modelData, 3)

                    Layout.preferredWidth: Kirigami.Units.gridUnit * 9
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 9 * 9 / 16 / 1.6

                    Kirigami.Icon {
                        anchors.centerIn: parent
                        visible: strip.shown.length === 0
                        source: "media-playlist-repeat"
                        width: Kirigami.Units.iconSizes.medium
                        height: width
                    }

                    Row {
                        anchors.fill: parent
                        spacing: 2
                        visible: strip.shown.length > 0

                        Repeater {
                            model: strip.shown
                            Rectangle {
                                required property string modelData
                                width: (strip.width - 4) / 3
                                height: strip.height
                                radius: Kirigami.Units.cornerRadius
                                color: Kirigami.Theme.alternateBackgroundColor
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    source: parent.modelData
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    sourceSize.width: 160
                                    sourceSize.height: 90
                                }
                            }
                        }
                    }
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
                        text: page.summary(row.modelData)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                }

                QQC2.Label {
                    visible: page.playingCount(row.modelData.id) > 0
                    text: qsTr("Playing")
                    color: Kirigami.Theme.highlightColor
                }
            }
        }

        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: list.count === 0
        icon.name: Daemon.playlistsAvailable ? "media-playlist-repeat" : "dialog-warning"
        text: Daemon.playlistsAvailable ? qsTr("No playlists yet") : qsTr("Playlists need a newer Murasaki service")
        explanation: Daemon.playlistsAvailable
            ? qsTr("A playlist changes the wallpaper on a screen every few minutes.")
            : qsTr("Update and restart the Murasaki service.")
        helpfulAction: Daemon.playlistsAvailable ? newAction : null
    }
}
