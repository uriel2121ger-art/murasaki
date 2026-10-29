import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, Pause: when the wallpapers stop by themselves.
Kirigami.FormLayout {
    id: page

    // What the daemon reports, with its defaults while it has not answered.
    readonly property bool pauseWhenCovered: Daemon.settings.pauseWhenCovered ?? SystemControl.defaultPauseWhenCovered
    readonly property bool pauseOnGame: Daemon.settings.pauseOnGame ?? true
    readonly property string batteryMode: Daemon.settings.batteryMode ?? "off"
    readonly property var pauseApps: Daemon.settings.pauseApps ?? []

    // Puts the controls back to the daemon's values (after SettingsChanged, or a rejected change).
    function sync() {
        coveredSwitch.checked = page.pauseWhenCovered;
        gameSwitch.checked = page.pauseOnGame;
        batteryBox.currentIndex = Math.max(0, batteryBox.indexOfValue(page.batteryMode));
        if (!appsField.activeFocus)
            appsField.text = page.pauseApps.join(", ");
    }

    Component.onCompleted: sync()

    Connections {
        target: Daemon
        function onSettingsChanged() { page.sync(); }
        function onError() { page.sync(); }
    }

    // These rules read as sentences, with the pets' automations, on the Automations page.
    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: true
        type: Kirigami.MessageType.Information
        text: qsTr("These rules are also on the Automations page, with ideas to start with.")
        actions: [
            Kirigami.Action {
                icon.name: "chronometer"
                text: qsTr("Open Automations")
                onTriggered: applicationWindow().openAutomations()
            }
        ]
    }

    QQC2.Switch {
        id: coveredSwitch
        Kirigami.FormData.label: qsTr("Playback:")
        text: qsTr("Pause when covered")
        enabled: Daemon.available
        checked: page.pauseWhenCovered
        onToggled: Daemon.setSettings({pauseWhenCovered: checked})
    }

    Hint {
        text: qsTr("Stops a wallpaper while a window covers the whole screen, to save power.")
    }

    QQC2.Switch {
        id: gameSwitch
        Kirigami.FormData.label: qsTr("Games:")
        text: qsTr("Pause while a game is running")
        enabled: Daemon.available
        checked: page.pauseOnGame
        onToggled: Daemon.setSettings({pauseOnGame: checked})
    }

    Hint {
        text: qsTr("Follows GameMode: a game started through gamemoderun (in Steam, set the launch option to gamemoderun %command%) pauses the wallpaper of the screen it covers until it ends; the other screens go on. Where the desktop does not say which screens are covered, every wallpaper pauses. Nothing happens when GameMode is not installed.")
    }

    QQC2.ComboBox {
        id: batteryBox
        Kirigami.FormData.label: qsTr("On battery:")
        enabled: Daemon.available
        textRole: "text"
        valueRole: "value"
        model: [
            {text: qsTr("Keep playing"), value: "off"},
            {text: qsTr("Pause the wallpaper"), value: "pause"},
            {text: qsTr("Limit to 15 frames per second"), value: "lowfps"}
        ]
        currentIndex: Math.max(0, indexOfValue(page.batteryMode))
        onActivated: Daemon.setSettings({batteryMode: currentValue})
    }

    Hint {
        text: qsTr("Uses UPower to know when the computer runs on battery. Without it the computer counts as plugged in.")
    }

    QQC2.TextField {
        id: appsField
        Kirigami.FormData.label: qsTr("Applications:")
        Layout.preferredWidth: Kirigami.Units.gridUnit * 24
        enabled: Daemon.available
        placeholderText: qsTr("For example: obs, steam_app_*")
        text: page.pauseApps.join(", ")
        // Sent when the field is left or Enter is pressed, not on every key.
        onEditingFinished: Daemon.setSettings({pauseApps: text.split(",").map(s => s.trim()).filter(s => s.length > 0)})
    }

    Hint {
        text: qsTr("Wallpapers pause while a window of one of these applications has the focus, or one of them is fullscreen. Separate them with commas; use the application's window class (the name KDE shows in its window rules), and * for any characters. Needs KDE Plasma.")
    }
}
