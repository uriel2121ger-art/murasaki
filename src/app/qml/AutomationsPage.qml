import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Automations: one place for what happens by itself, for the wallpapers and for the pets (spec 2026-10-25, layout B). "My
// automations" on top, each as a sentence with a switch; "Ideas to start with" under it, added with one click. The wallpaper
// rules are today's settings (pause while gaming, when covered, on battery, for listed applications): their switch writes the
// setting. The pet automations are the daemon's triggers. The former Triggers page stays reachable as the advanced view.
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Automations")

    AutomationText { id: words }

    // The wallpaper rules, from the daemon's settings with its defaults while it has not answered.
    readonly property bool gameOn: Daemon.settings.pauseOnGame ?? true
    readonly property bool coveredOn: Daemon.settings.pauseWhenCovered ?? SystemControl.defaultPauseWhenCovered
    readonly property string batteryMode: Daemon.settings.batteryMode ?? "off"
    readonly property var pauseApps: Daemon.settings.pauseApps ?? []
    readonly property bool petsGameOn: Daemon.settings.petsHideOnGame ?? false

    // A rule switched off while the page is open stays in place (a switch should not jump away under the pointer); next time
    // it is back among the ideas.
    property var keptRules: []
    // What was there before a rule was switched off, to bring back when it is switched on again.
    property string lastBatteryMode: "lowfps"
    property string lastPauseApps: "[]"

    Settings {
        category: "automations"
        property alias lastBatteryMode: page.lastBatteryMode
        property alias lastPauseApps: page.lastPauseApps
    }

    function activeRules() {
        const keys = [];
        if (gameOn)
            keys.push("game");
        if (coveredOn)
            keys.push("covered");
        if (batteryMode !== "off")
            keys.push("battery");
        if (pauseApps.length > 0)
            keys.push("apps");
        if (petsGameOn)
            keys.push("petsGame");
        return keys;
    }

    Component.onCompleted: keptRules = activeRules()

    function appList(classes) {
        const names = classes.slice(0, 3).map(c => words.appName(c));
        return classes.length > 3 ? qsTr("%1 and %n more", "", classes.length - 3).arg(names.join(", ")) : names.join(", ");
    }

    readonly property var wallpaperRules: [
        {key: "game", title: qsTr("Game mode"), icon: "input-gaming", on: gameOn,
         sentence: qsTr("When I play a game, pause the wallpaper of its screen")},
        {key: "covered", title: qsTr("Fullscreen"), icon: "view-fullscreen", on: coveredOn,
         sentence: qsTr("When a window covers a screen, pause its wallpaper")},
        {key: "battery", title: qsTr("Battery saver"), icon: "battery-040", on: batteryMode !== "off",
         sentence: (batteryMode === "pause" || (batteryMode === "off" && lastBatteryMode === "pause"))
             ? qsTr("When I unplug the charger, pause the wallpapers")
             : qsTr("When I unplug the charger, limit the wallpapers to 15 frames per second")},
        {key: "apps", title: qsTr("Pause for applications"), icon: "window", on: pauseApps.length > 0,
         sentence: pauseApps.length > 0 ? qsTr("When I use %1, pause the wallpapers").arg(appList(pauseApps))
                                        : qsTr("When I use the applications I choose, pause the wallpapers")},
        // A setting too (the pets' overlay follows the game's screens itself): a pair of triggers could only hide every pet.
        {key: "petsGame", title: qsTr("Pets out of the game"), icon: "input-gaming", on: petsGameOn,
         sentence: qsTr("While I play, hide the pets on the game's screen")}
    ]
    // The rows are made once from these fixed keys and shown or hidden: a model rebuilt on every settings change would recreate
    // them under the pointer (and under the keyboard focus).
    readonly property var ruleKeys: ["game", "covered", "battery", "apps", "petsGame"]
    function ruleOf(key) {
        return wallpaperRules.find(r => r.key === key);
    }
    // Whether the daemon has what a rule writes (petsHideOnGame came with revision 24).
    function ruleSupported(key) {
        return key !== "petsGame" || Daemon.petsGameSupported;
    }
    function isMine(key) {
        return ruleSupported(key) && (ruleOf(key).on || keptRules.includes(key));
    }
    readonly property int ideaCount: !Daemon.available ? 0
        : ruleKeys.filter(k => ruleSupported(k) && !isMine(k)).length + petIdeas.filter(i => ideaOffered(i)).length
    readonly property int myRuleCount: ruleKeys.filter(k => isMine(k)).length
    // A pet idea already added (by its name) is not offered twice.
    function ideaAdded(idea) {
        return (Daemon.triggers ?? []).some(t => t.name === idea.trigger.name);
    }
    // Offered on the gallery: not added yet, and what it needs is there.
    function ideaOffered(idea) {
        if (idea.key === "play" && Daemon.petsGameSupported)
            return false; // (the rule "petsGame" does it per screen)
        return Daemon.available && Daemon.automationSupported && !ideaAdded(idea);
    }

    // Ideas for the pets: made as triggers. Those that need a choice (which application) open the editor first.
    readonly property var petIdeas: [
        {key: "calls", title: qsTr("Quiet during calls"), icon: "call-start", ask: true,
         sentence: qsTr("When I open Discord, mute the pets"),
         trigger: {name: qsTr("Quiet during calls"), source: {kind: "app-open", app: "discord", on: "open"}, action: {type: "mute", mode: "on"}}},
        {key: "back", title: qsTr("Welcome back"), icon: "system-lock-screen",
         sentence: qsTr("When I unlock the session, show the pets"),
         trigger: {name: qsTr("Welcome back"), source: {kind: "session", state: "unlocked"}, action: {type: "show-all"}}},
        {key: "morning", title: qsTr("Good morning"), icon: "clock",
         sentence: qsTr("At 9:00, show the pets"),
         trigger: {name: qsTr("Good morning"), source: {kind: "time", at: "09:00", days: []}, action: {type: "show-all"}}},
        // A game trigger cannot hold, so a second trigger shows the pets again when the game ends. Only for a daemon older than
        // revision 24: the rule "petsGame" replaces it and hides only the pets on the game's screen.
        {key: "play", title: qsTr("Pets out of the game"), icon: "input-gaming",
         sentence: qsTr("While I play, hide the pets"),
         trigger: {name: qsTr("Pets out of the game"), source: {kind: "game", state: "started"}, action: {type: "hide-all"}},
         also: {name: qsTr("Pets back after the game"), source: {kind: "game", state: "ended"}, action: {type: "show-all"}}}
    ]

    function setRule(key, on) {
        if (on && !keptRules.includes(key))
            keptRules = keptRules.concat([key]);
        switch (key) {
        case "game":
            Daemon.setSettings({pauseOnGame: on});
            break;
        case "covered":
            Daemon.setSettings({pauseWhenCovered: on});
            break;
        case "petsGame":
            Daemon.setSettings({petsHideOnGame: on});
            break;
        case "battery":
            if (!on && batteryMode !== "off")
                lastBatteryMode = batteryMode;
            Daemon.setSettings({batteryMode: on ? lastBatteryMode : "off"});
            break;
        case "apps": {
            if (!on) {
                if (pauseApps.length > 0)
                    lastPauseApps = JSON.stringify(pauseApps);
                Daemon.setSettings({pauseApps: []});
                break;
            }
            let previous = [];
            try {
                previous = JSON.parse(lastPauseApps);
            } catch (e) {
                previous = [];
            }
            if (Array.isArray(previous) && previous.length > 0)
                Daemon.setSettings({pauseApps: previous.map(String).slice(0, 64)});
            else
                appsDialog.openWith([]);
            break;
        }
        }
    }

    function editRule(key) {
        if (key === "apps")
            appsDialog.openWith(pauseApps);
        else if (key === "battery")
            batteryMenu.popup();
    }

    function addIdea(idea) {
        const t = Object.assign({enabled: true, cooldownMs: Daemon.settings.triggerCooldownMs ?? 500, mode: "press"}, idea.trigger);
        if (idea.ask)
            editor.openFor(t);
        else
            Daemon.setTrigger("", t);
        if (idea.also)
            Daemon.setTrigger("", Object.assign({}, t, {name: idea.also.name, source: idea.also.source, action: idea.also.action}));
    }

    function editTrigger(t) {
        if (t.source?.kind === "screen")
            screenDialog.openFor(t);
        else
            editor.openFor(t);
    }

    // What a wallpaper rule does right now, from the surface's view of the screens ("" when it does nothing).
    function actingNow(key) {
        if (!ruleOf(key).on)
            return "";
        const outputs = Surface.outputs ?? [];
        const names = matches => outputs.filter(matches).map(o => words.plain(o.name)).join(", ");
        let screens = "";
        switch (key) {
        case "game":
            screens = names(o => o.pauseReason === "game");
            break;
        case "covered":
            screens = names(o => o.paused && o.occluded && !o.pauseReason);
            break;
        case "battery": {
            screens = names(o => o.pauseReason === "battery");
            const capped = outputs.find(o => (o.fpsCap ?? 0) > 0);
            if (screens === "" && capped)
                return qsTr("Acting now: %1 frames per second on %2").arg(capped.fpsCap).arg(names(o => (o.fpsCap ?? 0) > 0));
            break;
        }
        case "apps": {
            const paused = outputs.find(o => o.pauseReason === "app");
            return paused ? qsTr("Acting now: %1 paused the wallpapers").arg(words.appName(paused.pauseDetail ?? "")) : "";
        }
        }
        return screens === "" ? "" : qsTr("Acting now: the wallpaper of %1 is paused").arg(screens);
    }

    // When a pet automation last ran ("" if it has not since the service started). The daemon gives how long ago, as of the
    // moment its states arrived.
    property double statesAt: Date.now()
    Connections {
        target: Daemon
        function onTriggerStatesChanged() {
            page.statesAt = Date.now();
        }
    }
    function lastRunFor(id) {
        const state = (Daemon.triggerStates.states ?? []).find(s => s.id === id);
        if (!state || state.outcome !== "ran" || state.agoMs === undefined)
            return "";
        const at = new Date(statesAt - state.agoMs);
        if (at.toDateString() === new Date().toDateString())
            return qsTr("Last ran today at %1").arg(at.toLocaleTimeString(Qt.locale(), Locale.ShortFormat));
        return qsTr("Last ran on %1").arg(at.toLocaleString(Qt.locale(), Locale.ShortFormat));
    }

    // A note under a pet automation when the session cannot give what it watches.
    function noteFor(id) {
        const state = (Daemon.triggerStates.states ?? []).find(s => s.id === id) ?? ({});
        if (state.available === false)
            return qsTr("This session does not tell Murasaki what this automation watches.");
        return "";
    }

    // For --page automations:<idea>: the editor on an idea of the gallery ("new" for an empty one).
    function openEditorFor(key) {
        const idea = petIdeas.find(i => i.key === key);
        const preset = idea ? Object.assign({enabled: true, cooldownMs: 500, mode: "press"}, idea.trigger) : null;
        openLater.preset = preset;
        openLater.start(); // once the window has its size
    }

    Timer {
        id: openLater
        property var preset: null
        interval: 1000
        onTriggered: editor.openFor(preset)
    }

    // For the smoke test: every condition and action in the editor once, then the other dialogs.
    function smoke() {
        editor.openFor(null);
        for (const c of words.conditions.concat(words.technicalConditions))
            editor.chooseKind(c.kind);
        for (const a of words.actions)
            editor.actionKey = a.key;
        editor.buildChanges();
        editor.close();
        appsDialog.openWith(["firefox", "steam_app_*"]);
        appsDialog.close();
        deleteDialog.open();
        deleteDialog.close();
    }

    actions: [
        Kirigami.Action {
            id: newAction
            icon.name: "list-add"
            text: qsTr("New…")
            tooltip: qsTr("A new automation for the pets")
            enabled: Daemon.automationSupported
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: editor.openFor(null)
        },
        Kirigami.Action {
            icon.name: "view-preview"
            text: qsTr("New screen automation…")
            visible: Daemon.screenSupported
            enabled: Daemon.automationSupported
            onTriggered: screenDialog.openFor(null)
        },
        Kirigami.Action {
            icon.name: "configure"
            text: qsTr("Advanced view")
            tooltip: qsTr("Every trigger with its details, screen watching and the examples")
            onTriggered: applicationWindow().openTriggers()
        }
    ]

    AutomationEditor { id: editor }
    PauseAppsDialog {
        id: appsDialog
        // an empty list saved on purpose is not brought back by the switch
        onSaved: apps => {
            if (apps.length === 0)
                page.lastPauseApps = "[]";
        }
    }
    ScreenTriggerDialog { id: screenDialog; regionPicker: picker }
    RegionPicker { id: picker }

    QQC2.Menu {
        id: batteryMenu
        QQC2.MenuItem {
            text: qsTr("Limit the wallpapers to 15 frames per second")
            checkable: true
            checked: page.batteryMode === "lowfps"
            onTriggered: Daemon.setSettings({batteryMode: "lowfps"})
        }
        QQC2.MenuItem {
            text: qsTr("Pause the wallpapers")
            checkable: true
            checked: page.batteryMode === "pause"
            onTriggered: Daemon.setSettings({batteryMode: "pause"})
        }
    }

    property string deleting: ""
    KirigamiDialogs.PromptDialog {
        id: deleteDialog
        title: qsTr("Delete this automation?")
        subtitle: qsTr("It stops at once. This cannot be undone.")
        standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel
        onAccepted: Daemon.deleteTrigger(page.deleting)
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: Daemon.automationSupported && !(Daemon.settings.automationEnabled ?? true)
            type: Kirigami.MessageType.Warning
            text: qsTr("The automations of the pets are off: none of them runs.")
            actions: [
                Kirigami.Action {
                    icon.name: "media-playback-start"
                    text: qsTr("Turn them on")
                    onTriggered: Daemon.setSettings({automationEnabled: true})
                }
            ]
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: (Daemon.triggerStates.suspendedMs ?? 0) > 0
            type: Kirigami.MessageType.Warning
            text: qsTr("The automations are paused for a while: too many ran at once.")
            actions: [
                Kirigami.Action {
                    icon.name: "media-playback-start"
                    text: qsTr("Resume now")
                    onTriggered: Daemon.resumeAutomation()
                }
            ]
        }

        Kirigami.Heading {
            Layout.fillWidth: true
            level: 2
            text: qsTr("My automations")
        }

        Repeater {
            model: page.ruleKeys
            AutomationRow {
                required property string modelData
                readonly property var rule: page.ruleOf(modelData)
                visible: Daemon.available && page.isMine(modelData)
                iconName: rule.icon
                title: rule.title
                sentence: rule.sentence
                status: page.actingNow(modelData)
                statusPositive: true
                active: rule.on
                canEdit: modelData === "battery" || modelData === "apps"
                onSwitched: on => page.setRule(modelData, on)
                onEditRequested: page.editRule(modelData)
            }
        }

        Repeater {
            model: Daemon.automationSupported ? Daemon.triggers : []
            AutomationRow {
                required property var modelData
                iconName: words.conditionOf(modelData.source?.kind).icon
                title: words.plain(modelData.name)
                sentence: words.sentence(modelData)
                note: page.noteFor(modelData.id)
                status: page.lastRunFor(modelData.id)
                active: modelData.enabled === true
                canEdit: modelData.source?.kind !== "dbus" && (modelData.source?.kind !== "screen" || Daemon.screenSupported)
                canTest: true
                canDelete: true
                onSwitched: on => Daemon.setTriggerEnabled(modelData.id, on)
                onEditRequested: page.editTrigger(modelData)
                onTestRequested: Daemon.testTrigger(modelData.id)
                onRemoveRequested: {
                    page.deleting = modelData.id;
                    deleteDialog.open();
                }
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: Daemon.available && page.myRuleCount === 0 && (Daemon.triggers ?? []).length === 0
            opacity: 0.7
            wrapMode: Text.Wrap
            text: qsTr("Nothing happens by itself yet. Add an idea below, or make your own with New.")
        }

        Kirigami.Heading {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            level: 2
            text: qsTr("Ideas to start with")
            visible: page.ideaCount > 0
        }
        Hint {
            visible: page.ideaCount > 0
            text: qsTr("They do nothing until you add them. Add one and adjust it to your taste.")
        }

        GridLayout {
            Layout.fillWidth: true
            columns: Math.max(1, Math.floor(width / (Kirigami.Units.gridUnit * 16)))
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing
            uniformCellWidths: true

            Repeater {
                id: ideas
                model: page.ruleKeys.concat(page.petIdeas.map(i => i.key))

                Kirigami.AbstractCard {
                    id: idea
                    required property string modelData
                    readonly property var petIdea: page.petIdeas.find(i => i.key === modelData) ?? null
                    readonly property var rule: petIdea ?? page.ruleOf(modelData)
                    readonly property bool offered: petIdea ? page.ideaOffered(petIdea) : Daemon.available && page.ruleSupported(modelData) && !page.isMine(modelData)
                    visible: offered
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        RowLayout {
                            Kirigami.Icon {
                                source: idea.rule.icon
                                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            }
                            Kirigami.Heading {
                                Layout.fillWidth: true
                                level: 4
                                text: idea.rule.title
                                elide: Text.ElideRight
                            }
                            // An idea does nothing until it is added: said on the card, so it is not taken for a rule.
                            QQC2.Label {
                                text: qsTr("Not on")
                                font: Kirigami.Theme.smallFont
                                color: Kirigami.Theme.disabledTextColor
                            }
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: idea.rule.sentence
                            wrapMode: Text.Wrap
                            opacity: 0.8
                        }
                        QQC2.Button {
                            Layout.alignment: Qt.AlignRight
                            icon.name: "list-add"
                            text: qsTr("Add")
                            highlighted: true
                            onClicked: idea.petIdea ? page.addIdea(idea.petIdea) : page.setRule(idea.modelData, true)
                        }
                    }
                }
            }
        }
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: !Daemon.available
        icon.name: "dialog-warning"
        text: qsTr("The Murasaki service is not running")
    }
}
