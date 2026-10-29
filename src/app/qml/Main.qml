import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Murasaki.App

Kirigami.ApplicationWindow {
    id: root

    title: "Murasaki"
    width: Kirigami.Units.gridUnit * 50
    height: Kirigami.Units.gridUnit * 34
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 20

    // Library page, then the item page on top of it.
    // Pages are created here with a parent: pushing a Component makes Qt warn that a parentless item was created.
    function create(component, properties) {
        return component.createObject(pageStack, properties || {});
    }

    // Pets have their own page: they are placed on the desktop, not assigned as a wallpaper.
    function openItem(itemId) {
        const row = Library.indexOf(itemId);
        const isPet = row >= 0 ? Library.get(row).kind === "pet" : String(itemId).startsWith("anima:") || String(itemId).startsWith("local:");
        pageStack.push(create(isPet ? petPage : itemPage, {itemId: itemId}));
    }

    // A playlist's page opens on top of the Playlists section.
    function openPlaylist(playlistId) {
        if (section !== "playlists")
            goTo("playlists", playlistsPage);
        while (pageStack.depth > 1)
            pageStack.pop();
        pageStack.push(create(playlistPage, {playlistId: playlistId}));
    }

    // The drawer section on screen; picking it again only returns from an item page.
    property string section: "library"

    function goTo(name, page, properties) {
        if (name === section) {
            if (name === "workshop" && properties && properties.url) {
                pageStack.currentItem.url = properties.url;
            }
            while (pageStack.depth > 1)
                pageStack.pop();
            return;
        }
        section = name;
        pageStack.clear();
        pageStack.push(create(page, properties));
    }

    function openAutomations() {
        goTo("automations", automationsPage);
    }

    // The former Triggers page, as the advanced view of Automations.
    function openTriggers() {
        if (section !== "automations")
            goTo("automations", automationsPage);
        while (pageStack.depth > 1)
            pageStack.pop();
        pageStack.push(create(triggersPage));
    }

    function openWorkshop(url) {
        goTo("workshop", workshopPage, {url: url});
    }

    Component { id: libraryPage; LibraryPage {} }
    Component { id: itemPage; ItemPage {} }
    Component { id: petPage; PetPage {} }
    Component { id: playlistsPage; PlaylistsPage {} }
    Component { id: playlistPage; PlaylistPage {} }
    Component { id: profilesPage; ProfilesPage {} }
    Component { id: triggersPage; TriggersPage {} }
    Component { id: automationsPage; AutomationsPage {} }
    Component { id: timelinesPage; TimelinesPage {} }
    Component { id: workshopPage; WorkshopPage {} }
    Component { id: screensPage; ScreensPage {} }
    Component { id: settingsPage; SettingsPage {} }

    // The Workshop's browser profile: created on first use, one for the whole run (see WorkshopProfile.qml).
    function workshopProfile() {
        return workshopProfileHolder.instance();
    }

    WorkshopProfile { id: workshopProfileHolder }

    // Shows a drawer page by name (for --page).
    function showPage(name) {
        if (String(name).startsWith("playlist:")) {
            openPlaylist(String(name).slice(9)); // a playlist's own page, by id (for screenshots)
            return;
        }
        if (String(name).startsWith("automations:")) {
            goTo("automations", automationsPage); // the editor on an idea of the gallery, or empty (for screenshots)
            pageStack.currentItem.openEditorFor(String(name).slice(12));
            return;
        }
        if (String(name).startsWith("settings:")) {
            goTo("settings", settingsPage);
            pageStack.currentItem.current = String(name).slice(9); // one category of Settings (for screenshots)
            return;
        }
        switch (name) {
        case "workshop": openWorkshop(""); break;
        case "playlists": goTo("playlists", playlistsPage); break;
        case "profiles": goTo("profiles", profilesPage); break;
        case "triggers": openTriggers(); break;
        case "automations": goTo("automations", automationsPage); break;
        case "timelines": goTo("timelines", timelinesPage); break;
        case "screens": goTo("screens", screensPage); break;
        case "settings": goTo("settings", settingsPage); break;
        default: goTo("library", libraryPage);
        }
    }

    // For the smoke test: visits every drawer page.
    function smokeTour() {
        openWorkshop("");
        goTo("playlists", playlistsPage);
        openPlaylist("smoke-test-unknown"); // the page of a playlist that does not exist
        for (const [name, page] of [["profiles", profilesPage], ["triggers", triggersPage], ["automations", automationsPage], ["timelines", timelinesPage]]) {
            goTo(name, page);
            pageStack.currentItem.smoke(); // opens and closes its dialogs once, so that a binding error in them shows
        }
        goTo("screens", screensPage);
        goTo("settings", settingsPage);
        pageStack.currentItem.smoke(); // every category of Settings
        openWorkshop(""); // a second Workshop page must reuse the profile
        goTo("library", libraryPage);
        openFirstPet();
    }

    // For the smoke test: the pet page of the first pet of the library, or of an unknown one (its error state).
    function openFirstPet() {
        let id = "anima:smoke-test-unknown";
        for (let row = 0; row < Library.count; ++row) {
            if (Library.get(row).kind === "pet") {
                id = Library.get(row).itemId;
                break;
            }
        }
        openItem(id);
    }

    // For the smoke test: opens the item page of the first library entry.
    function openFirstItem() {
        if (LibraryFilter.count > 0)
            openItem(LibraryFilter.itemIdAt(0));
    }

    Connections {
        target: Daemon
        function onError(message) {
            root.showPassiveNotification(message);
        }
    }

    // The toggle shows the daemon's arrange mode (also changed by the shortcut and murasakictl); a call the
    // daemon refused puts it back.
    Binding {
        target: arrangeAction
        property: "checked"
        value: Daemon.petsArrange
        restoreMode: Binding.RestoreNone
    }

    Connections {
        target: Daemon
        function onError() { arrangeAction.checked = Daemon.petsArrange; }
    }

    // What the automation calls answer.
    Connections {
        target: Daemon
        function onPackAutomationMade(result) {
            const triggers = (result.triggers ?? []).length;
            root.showPassiveNotification(triggers > 0 ? qsTr("Timeline and triggers made. The triggers are disabled: switch one on under Triggers.")
                                                      : qsTr("Timeline made. See Timelines."));
        }
        function onProfileSwitched(report) {
            if ((report.skipped ?? []).length > 0)
                root.showPassiveNotification(qsTr("Switched, but %n pet(s) could not be shown.", "", report.skipped.length));
        }
        function onTriggerTested(triggerId, ran) {
            if (!ran)
                root.showPassiveNotification(qsTr("The trigger did not run (it is off, or waiting for its cooldown)."));
        }
    }

    Connections {
        target: SystemControl
        function onError(message) {
            root.showPassiveNotification(message);
        }
    }

    // Pages made by create() are not owned by the stack, so free them once they leave it.
    Connections {
        target: root.pageStack
        function onPageRemoved(page) {
            page.destroy(1000);
        }
    }

    // Ctrl+F searches the library from any page; the configure key opens Settings.
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: root.section !== "workshop" // there it is the web page's own search
        onActivated: {
            root.goTo("library", libraryPage);
            root.pageStack.currentItem.focusSearch();
        }
    }

    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: root.goTo("settings", settingsPage)
    }

    // The drawer marks the section on screen; wallpapers first, then the pets, then Settings.
    globalDrawer: Kirigami.GlobalDrawer {
        isMenu: false
        collapsible: true
        collapsed: false
        modal: !root.wideScreen
        title: "Murasaki"
        titleIcon: "preferences-desktop-wallpaper"
        actions: [
            Kirigami.Action {
                text: qsTr("Library")
                icon.name: "view-list-icons"
                checked: root.section === "library"
                onTriggered: root.goTo("library", libraryPage)
            },
            Kirigami.Action {
                text: qsTr("Workshop")
                icon.name: "internet-services"
                checked: root.section === "workshop"
                onTriggered: root.openWorkshop("")
            },
            Kirigami.Action {
                text: qsTr("Playlists")
                icon.name: "media-playlist-repeat"
                checked: root.section === "playlists"
                onTriggered: root.goTo("playlists", playlistsPage)
            },
            Kirigami.Action {
                text: qsTr("Screens")
                icon.name: "video-display"
                checked: root.section === "screens"
                onTriggered: root.goTo("screens", screensPage)
            },
            Kirigami.Action {
                text: qsTr("Automations")
                icon.name: "chronometer"
                checked: root.section === "automations"
                onTriggered: root.goTo("automations", automationsPage)
                tooltip: qsTr("What happens by itself: pause the wallpapers, hide or mute the pets…")
            },
            Kirigami.Action {
                separator: true
            },
            Kirigami.Action {
                text: qsTr("Profiles")
                icon.name: "view-preview"
                checked: root.section === "profiles"
                onTriggered: root.goTo("profiles", profilesPage)
            },
            Kirigami.Action {
                text: qsTr("Timelines")
                icon.name: "view-time-schedule"
                checked: root.section === "timelines"
                onTriggered: root.goTo("timelines", timelinesPage)
            },
            Kirigami.Action {
                id: arrangeAction
                text: qsTr("Arrange pets")
                icon.name: "transform-move"
                checkable: true
                enabled: Daemon.petsAvailable
                onToggled: Daemon.setPetsArrange(checked)
                tooltip: qsTr("Move, resize and remove the pets on the desktop with the mouse")
            },
            Kirigami.Action {
                separator: true
            },
            Kirigami.Action {
                text: qsTr("Settings")
                icon.name: "configure"
                checked: root.section === "settings"
                onTriggered: root.goTo("settings", settingsPage)
            }
        ]
    }

    // Item pages replace the library on screen instead of squeezing beside it.
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn
    pageStack.initialPage: LibraryPage {}
}
