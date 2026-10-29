import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

Kirigami.ScrollablePage {
    id: page

    title: qsTr("Library")
    keyboardNavigationEnabled: false

    // Card width in grid units for the three sizes of the View menu (0 small, 1 medium, 2 large).
    property int cardSize: 1
    readonly property real cardUnits: [10, 13.5, 18][Math.max(0, Math.min(2, cardSize))]

    // For Ctrl+F from anywhere in the window.
    function focusSearch() {
        searchField.forceActiveFocus();
        searchField.selectAll();
    }

    // The view choices are remembered between runs; what is filtered is not (an empty library on start would confuse).
    Settings {
        category: "library"
        property alias cardSize: page.cardSize
    }

    // Reason the grid cannot be shown, or "" when it can (an empty filtered result is handled apart).
    readonly property string blocker: {
        if (!Daemon.available)
            return Daemon.apiVersion > 0 && Daemon.apiVersion < 4 ? "outdated" : "offline";
        // Pets and local files do not need Wallpaper Engine: the notice only replaces an empty library.
        if ((Daemon.status === "steam-not-found" || Daemon.status === "wallpaper-engine-not-installed") && Library.count === 0)
            return Daemon.status;
        return "";
    }

    // Search and filters sit in a bar of their own that wraps on narrow windows (as page actions they would
    // vanish into the overflow menu).
    header: QQC2.ToolBar {
        Flow {
            width: parent.width
            spacing: Kirigami.Units.smallSpacing

            Kirigami.SearchField {
                id: searchField
                placeholderText: qsTr("Search…")
                width: Math.min(Kirigami.Units.gridUnit * 14, parent.width)
                text: LibraryFilter.searchText
                onTextChanged: LibraryFilter.searchText = text
                // Down goes to the results, Escape clears the search first.
                Keys.onDownPressed: grid.focusFirst()
                Keys.onEscapePressed: event => {
                    event.accepted = text !== "";
                    LibraryFilter.searchText = ""; // through the filter: a write to text would break the binding
                }
                onAccepted: grid.focusFirst()
            }

            Row {
                QQC2.ButtonGroup { id: kindGroup }

                Repeater {
                    model: [
                        {label: qsTr("All"), kind: ""},
                        {label: qsTr("Video"), kind: "video"},
                        {label: qsTr("Web"), kind: "web"},
                        {label: qsTr("Scene"), kind: "scene"},
                        {label: qsTr("Pets"), kind: "pet"}
                    ]
                    QQC2.ToolButton {
                        required property var modelData
                        text: modelData.label
                        checkable: true
                        checked: LibraryFilter.kindFilter === modelData.kind
                        QQC2.ButtonGroup.group: kindGroup
                        onClicked: LibraryFilter.kindFilter = modelData.kind
                    }
                }
            }

            QQC2.ToolButton {
                icon.name: "video-display"
                text: qsTr("On screen")
                checkable: true
                checked: LibraryFilter.onScreenOnly
                onToggled: LibraryFilter.onScreenOnly = checked
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: qsTr("Show only what a screen plays now")
            }

            QQC2.Label {
                height: searchField.height
                verticalAlignment: Text.AlignVCenter
                leftPadding: Kirigami.Units.smallSpacing
                text: LibraryFilter.kindFilter === "pet" ? qsTr("%n pet(s)", "", LibraryFilter.count)
                                                          : qsTr("%n wallpaper(s)", "", LibraryFilter.count)
                opacity: 0.7
            }
        }
    }

    actions: [
        Kirigami.Action {
            icon.name: "view-list-icons"
            text: qsTr("View")
            tooltip: qsTr("Size of the previews")
            displayHint: Kirigami.DisplayHint.IconOnly
            Kirigami.Action {
                text: qsTr("Small previews")
                checkable: true
                checked: page.cardSize === 0
                onTriggered: page.cardSize = 0
            }
            Kirigami.Action {
                text: qsTr("Medium previews")
                checkable: true
                checked: page.cardSize === 1
                onTriggered: page.cardSize = 1
            }
            Kirigami.Action {
                text: qsTr("Large previews")
                checkable: true
                checked: page.cardSize === 2
                onTriggered: page.cardSize = 2
            }
        },
        Kirigami.Action {
            id: addPetAction
            icon.name: "list-add"
            text: qsTr("Add pet")
            tooltip: qsTr("Make a desktop pet from an animation file, a sprite sheet, images, a ZIP, a GIF or a package")
            enabled: Daemon.petsAvailable
            displayHint: Kirigami.DisplayHint.KeepVisible
            Kirigami.Action {
                text: qsTr("From an animation file…")
                icon.name: "video-x-generic"
                onTriggered: petFileDialog.open()
            }
            Kirigami.Action {
                text: qsTr("A puppet folder…")
                icon.name: "user-identity"
                tooltip: qsTr("A folder with one Inochi2D puppet (.inp), or a Cubism model (.model3.json and its files). It is checked before it is shown; a Cubism model also needs the Cubism runtime, which Murasaki does not ship.")
                enabled: Daemon.puppetsSupported
                onTriggered: puppetFolderDialog.open()
            }
            Kirigami.Action {
                text: qsTr("From a sprite sheet…")
                icon.name: "view-grid"
                enabled: Daemon.importsSupported
                onTriggered: importDialog.openFor("sheet")
            }
            Kirigami.Action {
                text: qsTr("From images…")
                icon.name: "folder-pictures"
                enabled: Daemon.importsSupported
                onTriggered: importDialog.openFor("images")
            }
            Kirigami.Action {
                text: qsTr("From a ZIP of frames…")
                icon.name: "application-x-archive"
                enabled: Daemon.importsSupported
                onTriggered: importDialog.openFor("zip")
            }
            Kirigami.Action {
                text: qsTr("From a video…")
                icon.name: "video-x-generic"
                enabled: Daemon.importsSupported
                onTriggered: importDialog.openFor("video")
            }
            Kirigami.Action {
                text: qsTr("From a package (.mdaa)…")
                icon.name: "package-x-generic"
                enabled: Daemon.importsSupported
                onTriggered: importDialog.openFor("mdaa")
            }
            Kirigami.Action {
                text: qsTr("Convert a GIF to APNG…")
                icon.name: "view-refresh"
                enabled: Daemon.importsSupported
                onTriggered: importDialog.openFor("gif")
            }
        }
    ]

    ImportDialog {
        id: importDialog
    }

    FileDialog {
        id: petFileDialog
        title: qsTr("Add animation file")
        nameFilters: [qsTr("Animations (*.gif *.webp *.apng *.png)"), qsTr("All files (*)")]
        onAccepted: Daemon.addLocalPetItem(SystemControl.localFile(selectedFile))
    }

    FolderDialog {
        id: puppetFolderDialog
        title: qsTr("Add a puppet folder")
        onAccepted: Daemon.addLocalPuppetItem(SystemControl.localFile(selectedFolder))
    }

    // The new pet item appears in the library on its own; take the user to its page to show it.
    Connections {
        target: Daemon
        function onLocalPetAdded(path, itemId) { applicationWindow().openItem(itemId); }
        function onLocalPuppetAdded(folder, itemId) { applicationWindow().openItem(itemId); }
    }

    AddToPlaylist {
        id: addToPlaylist
    }

    // Right click on a wallpaper: open it, or add it to a playlist.
    QQC2.Menu {
        id: cardMenu

        property string itemId: ""
        property bool isWallpaper: false

        QQC2.MenuItem {
            icon.name: "document-open"
            text: qsTr("Open")
            onTriggered: applicationWindow().openItem(cardMenu.itemId)
        }
        QQC2.MenuItem {
            icon.name: "media-playlist-append"
            text: qsTr("Add to playlist…")
            visible: cardMenu.isWallpaper
            enabled: Daemon.playlistsAvailable
            height: visible ? implicitHeight : 0
            onTriggered: addToPlaylist.popup(cardMenu.itemId)
        }
    }

    // Whether the search or a filter hides part of the library.
    readonly property bool filtered: LibraryFilter.searchText !== "" || LibraryFilter.kindFilter !== "" || LibraryFilter.onScreenOnly

    Kirigami.Action {
        id: clearFiltersAction
        icon.name: "edit-clear-all"
        text: qsTr("Show everything")
        onTriggered: {
            LibraryFilter.searchText = "";
            LibraryFilter.kindFilter = "";
            LibraryFilter.onScreenOnly = false;
        }
    }

    Kirigami.Action {
        id: workshopAction
        icon.name: "internet-services"
        text: qsTr("Open the Workshop")
        onTriggered: applicationWindow().openWorkshop("")
    }

    Kirigami.Action {
        id: startAction
        icon.name: "media-playback-start"
        text: qsTr("Start service")
        onTriggered: Daemon.refreshItems()
    }

    GridView {
        id: grid

        readonly property int columns: Math.max(1, Math.floor(width / (Kirigami.Units.gridUnit * page.cardUnits)))

        // From the search field: the first result takes the keyboard (arrows move, Enter opens).
        function focusFirst() {
            if (count === 0)
                return;
            if (currentIndex < 0)
                currentIndex = 0;
            forceActiveFocus();
        }

        clip: true
        cellWidth: Math.floor(width / columns)
        cellHeight: cellWidth * 9 / 16 + Kirigami.Units.gridUnit * 3.4
        model: LibraryFilter
        reuseItems: true
        currentIndex: -1
        highlight: null
        keyNavigationEnabled: true
        activeFocusOnTab: true
        Keys.onReturnPressed: if (currentIndex >= 0) applicationWindow().openItem(LibraryFilter.itemIdAt(currentIndex))
        Keys.onEnterPressed: if (currentIndex >= 0) applicationWindow().openItem(LibraryFilter.itemIdAt(currentIndex))
        Keys.onMenuPressed: if (currentItem) currentItem.openMenu()
        // Leaving the top row with Up returns to the search.
        Keys.onUpPressed: event => {
            if (currentIndex < columns) {
                searchField.forceActiveFocus();
            } else {
                moveCurrentIndexUp();
            }
        }

        delegate: Item {
            id: cell

            required property var model
            required property int index

            function openMenu() {
                cardMenu.itemId = cell.model.itemId;
                cardMenu.isWallpaper = cell.model.kind !== "pet" && cell.model.status === "ok";
                cardMenu.popup(card);
            }

            width: grid.cellWidth
            height: grid.cellHeight

            ItemCard {
                id: card
                anchors {
                    fill: parent
                    margins: Kirigami.Units.smallSpacing
                }
                itemId: cell.model.itemId
                title: cell.model.title
                kind: cell.model.kind
                format: cell.model.format
                status: cell.model.status
                preview: cell.model.preview
                assigned: cell.model.assigned
                hasRisks: cell.model.hasRisks
                networkAllowed: cell.model.networkAllowed
                keyboardCurrent: grid.activeFocus && cell.index === grid.currentIndex
                onClicked: applicationWindow().openItem(itemId)

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        cardMenu.itemId = cell.model.itemId;
                        cardMenu.isWallpaper = cell.model.kind !== "pet" && cell.model.status === "ok";
                        cardMenu.popup();
                    }
                }
            }
        }

        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: page.blocker !== "" || LibraryFilter.count === 0
        icon.name: page.blocker === "" ? "edit-find" : "dialog-warning"
        text: {
            switch (page.blocker) {
            case "outdated": return qsTr("The Murasaki service is outdated — restart it");
            case "offline": return qsTr("The Murasaki service is not running");
            case "steam-not-found": return qsTr("Steam was not found");
            case "wallpaper-engine-not-installed": return qsTr("Wallpaper Engine is not installed");
            }
            if (LibraryFilter.onScreenOnly && LibraryFilter.searchText === "")
                return qsTr("Nothing is on screen");
            if (LibraryFilter.kindFilter === "pet" && LibraryFilter.searchText === "")
                return qsTr("No pets yet");
            return page.filtered ? qsTr("No wallpapers match") : qsTr("The library is empty");
        }
        explanation: {
            switch (page.blocker) {
            case "steam-not-found": return qsTr("Install Steam and sign in, then restart the Murasaki service.");
            case "wallpaper-engine-not-installed": return qsTr("Install Wallpaper Engine from Steam to get its wallpapers.");
            case "":
                if (LibraryFilter.onScreenOnly && LibraryFilter.searchText === "")
                    return qsTr("Open a wallpaper and press Apply to show it on a screen.");
                if (LibraryFilter.kindFilter === "pet" && LibraryFilter.searchText === "")
                    return qsTr("Subscribe to Anima Engine items on Steam, or add a GIF, WebP or APNG file.");
                return page.filtered ? qsTr("Try a different search or filter.") : qsTr("Subscribe to wallpapers on the Workshop to see them here.");
            }
            return "";
        }
        helpfulAction: {
            if (page.blocker === "offline")
                return startAction;
            if (page.blocker !== "")
                return null;
            if (LibraryFilter.kindFilter === "pet" && LibraryFilter.searchText === "" && !LibraryFilter.onScreenOnly)
                return Daemon.petsAvailable ? addPetAction : null;
            if (page.filtered)
                return clearFiltersAction;
            return workshopAction;
        }
    }
}
