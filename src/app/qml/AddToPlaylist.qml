import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Murasaki.App

// "Add to playlist" for one wallpaper: a menu of the playlists plus "New playlist…". Declare one per page and call
// popup(itemId); it has no size of its own.
Item {
    id: root

    property string itemId: ""
    // The wallpaper waiting for the playlist that is being created.
    property string pendingItemId: ""

    visible: false
    width: 0
    height: 0

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    function popup(id) {
        root.itemId = id;
        menu.popup();
    }

    QQC2.Menu {
        id: menu
        title: qsTr("Add to playlist")

        Instantiator {
            model: Daemon.playlists
            delegate: QQC2.MenuItem {
                required property var modelData
                text: root.plain(modelData.name)
                onTriggered: Daemon.addToPlaylist(modelData.id, [root.itemId])
            }
            onObjectAdded: (index, object) => menu.insertItem(index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }

        QQC2.MenuSeparator {
            visible: Daemon.playlists.length > 0
        }

        QQC2.MenuItem {
            icon.name: "list-add"
            text: qsTr("New playlist…")
            onTriggered: {
                root.pendingItemId = root.itemId;
                nameDialog.open();
            }
        }
    }

    PlaylistNameDialog {
        id: nameDialog
        onNamed: name => Daemon.createPlaylist(name)
    }

    Connections {
        target: Daemon
        function onPlaylistCreated(name, id) {
            if (root.pendingItemId !== "" && id !== "") {
                Daemon.addToPlaylist(id, [root.pendingItemId]);
                root.pendingItemId = "";
            }
        }
        function onError() { root.pendingItemId = ""; }
    }
}
