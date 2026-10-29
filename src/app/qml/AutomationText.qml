import QtQuick
import Murasaki.App

// The words of the Automations area: how each condition and action reads in a sentence ("When I open Discord, mute the pets"),
// with its icon from the system theme. Names come from files and imports: they are cut here and shown as plain text by whoever
// displays them.
QtObject {
    id: words

    // Cut, and without angle brackets: some of these texts reach controls that auto-detect rich text.
    function plain(s) {
        return s === undefined || s === null ? "" : String(s).slice(0, 120).replace(/</g, "\u2039").replace(/>/g, "\u203a");
    }

    // An application by the window class a trigger stores: its installed name, else the class as written.
    function appName(appClass) {
        if (!appClass)
            return qsTr("an application");
        ShortcutCatalog.total; // evaluated again when applications are installed or removed
        const entry = ShortcutCatalog.entryForClass(appClass);
        return entry.name ? plain(entry.name) : plain(appClass);
    }

    function appIcon(appClass) {
        ShortcutCatalog.total;
        return appClass ? (ShortcutCatalog.entryForClass(appClass).icon ?? "") : "";
    }

    // What can start an automation, in the order the editor offers them.
    readonly property var conditions: [
        {kind: "app-open", icon: "window-new", text: qsTr("I open an application")},
        {kind: "app-focus", icon: "window", text: qsTr("I use an application")},
        {kind: "game", icon: "input-gaming", text: qsTr("I play a game")},
        {kind: "fullscreen", icon: "view-fullscreen", text: qsTr("something fills the screen")},
        {kind: "power", icon: "battery-080-charging", text: qsTr("I plug or unplug the charger")},
        {kind: "time", icon: "clock", text: qsTr("it is a certain time")},
        {kind: "interval", icon: "view-refresh", text: qsTr("some minutes pass")},
        {kind: "idle", icon: "system-suspend", text: qsTr("I am away for a while")},
        {kind: "session", icon: "system-lock-screen", text: qsTr("I lock or unlock the session")},
        {kind: "output", icon: "video-display", text: qsTr("I plug or unplug a screen")},
        {kind: "shortcut", icon: "input-keyboard", text: qsTr("I press a key")}
    ]
    // For scripts: offered under "More options" only.
    readonly property var technicalConditions: [
        {kind: "signal-file", icon: "document-new", text: qsTr("a signal file is touched")},
        {kind: "fire", icon: "utilities-terminal", text: qsTr("a command calls it")}
    ]
    // What an automation can do: the closed list of the daemon (nothing runs a program or opens a file or a web address).
    readonly property var actions: [
        {key: "hide-all", icon: "view-hidden", text: qsTr("hide the pets")},
        {key: "show-all", icon: "view-visible", text: qsTr("show the pets")},
        {key: "toggle-hidden", icon: "visibility", text: qsTr("hide or show the pets")},
        {key: "mute-on", icon: "audio-volume-muted", text: qsTr("mute the pets")},
        {key: "mute-off", icon: "audio-volume-high", text: qsTr("unmute the pets")},
        {key: "mute-toggle", icon: "audio-volume-medium", text: qsTr("mute or unmute the pets")},
        {key: "stop-sounds", icon: "media-playback-stop", text: qsTr("stop the pets' sounds")},
        {key: "play-sound", icon: "media-playback-start", text: qsTr("play a pet's sound")},
        {key: "timeline", icon: "view-time-schedule", text: qsTr("play a timeline")},
        {key: "profile", icon: "view-preview", text: qsTr("switch to a profile")},
        {key: "scene", icon: "media-playlist-play", text: qsTr("start a scene of a package")}
    ]

    readonly property var dayNames: [qsTr("Sun"), qsTr("Mon"), qsTr("Tue"), qsTr("Wed"), qsTr("Thu"), qsTr("Fri"), qsTr("Sat")]

    function conditionOf(kind) {
        return conditions.concat(technicalConditions).find(c => c.kind === kind)
            ?? (kind === "screen" ? {kind: "screen", icon: "view-preview", text: qsTr("a picture shows on the screen")}
                                  : {kind: kind, icon: "dialog-question", text: plain(kind)});
    }

    // The key of an action in `actions`: mute is three entries, one per mode.
    function actionKey(action) {
        const a = action ?? ({});
        return a.type === "mute" ? "mute-" + (a.mode ?? "toggle") : (a.type ?? "");
    }

    function actionOf(action) {
        const key = actionKey(action);
        return actions.find(a => a.key === key) ?? {key: key, icon: "dialog-question", text: plain(key)};
    }

    // The "when" half of a sentence, from a trigger's source.
    function whenText(source) {
        const s = source ?? ({});
        switch (s.kind) {
        case "app-open": return s.on === "close" ? qsTr("I close %1").arg(appName(s.app)) : qsTr("I open %1").arg(appName(s.app));
        case "app-focus": return s.on === "leave" ? qsTr("I stop using %1").arg(appName(s.app)) : qsTr("I use %1").arg(appName(s.app));
        case "fullscreen":
            if (s.on === "leave")
                return s.app ? qsTr("%1 leaves fullscreen").arg(appName(s.app)) : qsTr("nothing fills the screen any more");
            return s.app ? qsTr("%1 fills the screen").arg(appName(s.app)) : qsTr("something fills the screen");
        case "time": {
            const days = (s.days ?? []).filter(d => d >= 0 && d <= 6);
            if (days.length > 0 && days.length < 7)
                return qsTr("it is %1 on %2").arg(plain(s.at)).arg(days.slice().sort().map(d => dayNames[d]).join(", "));
            return qsTr("it is %1").arg(plain(s.at));
        }
        case "interval": return qsTr("%n minute(s) pass", "", s.everyMinutes ?? 1);
        case "idle": return s.state === "back" ? qsTr("I come back after %n minute(s) away", "", s.minutes ?? 1)
                                               : qsTr("I am away for %n minute(s)", "", s.minutes ?? 1);
        case "power": return s.state === "battery" ? qsTr("I unplug the charger") : qsTr("I plug in the charger");
        case "session": return s.state === "locked" ? qsTr("I lock the session") : qsTr("I unlock the session");
        case "game": return s.state === "ended" ? qsTr("the game ends") : qsTr("a game starts");
        case "output": return s.state === "removed" ? qsTr("I unplug a screen") : qsTr("I plug in a screen");
        case "shortcut": return qsTr("I press the key \"%1\"").arg(plain(s.description || s.id));
        case "signal-file": return qsTr("the file %1 is touched").arg(plain(s.name));
        case "fire": return qsTr("a command calls %1").arg(plain(s.name));
        case "screen": return qsTr("a picture shows on the screen");
        case "dbus": return qsTr("a D-Bus signal arrives (%1)").arg(plain(s.member));
        }
        return plain(s.kind);
    }

    // The "then" half, from a trigger's action.
    function thenText(action) {
        const a = action ?? ({});
        switch (a.type) {
        case "timeline": {
            const found = (Daemon.timelines ?? []).find(t => t.id === a.id);
            return found ? qsTr("play the timeline %1").arg(plain(found.name)) : qsTr("play a timeline");
        }
        case "profile": {
            const found = (Daemon.profiles ?? []).find(p => p.id === a.id);
            return found ? qsTr("switch to the profile %1").arg(plain(found.name)) : qsTr("switch to a profile");
        }
        case "scene": return qsTr("start scene %1 of a package").arg((a.index ?? 0) + 1);
        }
        return actionOf(a).text;
    }

    function sentence(trigger) {
        return qsTr("When %1, %2").arg(whenText(trigger?.source)).arg(thenText(trigger?.action));
    }
}
