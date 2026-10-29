import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, Privacy and security: the sandbox and what wallpapers may use.
Kirigami.FormLayout {
    id: page

    // What the daemon reports, with its defaults while it has not answered.
    readonly property bool sandboxEngines: Daemon.settings.sandboxEngines ?? true
    readonly property bool requireSandbox: Daemon.settings.requireSandbox ?? false

    // Puts the controls back to the daemon's values (after SettingsChanged, or a rejected change).
    function sync() {
        sandboxSwitch.checked = page.sandboxEngines;
        requireSwitch.checked = page.requireSandbox;
    }

    Component.onCompleted: sync()

    Connections {
        target: Daemon
        function onSettingsChanged() { page.sync(); }
        function onError() { page.sync(); }
    }

    QQC2.Switch {
        id: sandboxSwitch
        Kirigami.FormData.label: qsTr("Sandbox:")
        text: qsTr("Run scene engines in a sandbox")
        enabled: Daemon.available
        checked: page.sandboxEngines
        onToggled: Daemon.setSettings({sandboxEngines: checked})
    }

    QQC2.Switch {
        Kirigami.FormData.label: qsTr("Media:")
        text: qsTr("Tell wallpapers that use it what is playing (title, artist and cover)")
        enabled: Daemon.available
        checked: Daemon.settings.mediaShare ?? true
        onToggled: Daemon.setSettings({mediaShare: checked})
    }

    QQC2.Switch {
        Kirigami.FormData.label: qsTr("Media cover:")
        text: qsTr("Download the cover of the playing media for scenes that show it")
        enabled: Daemon.available
        checked: Daemon.settings.mediaRemoteArt ?? true
        onToggled: Daemon.setSettings({mediaRemoteArt: checked})
    }

    QQC2.Switch {
        Kirigami.FormData.label: qsTr("Shortcuts:")
        text: qsTr("Let wallpapers start the applications I chose")
        enabled: Daemon.available
        checked: Daemon.settings.shortcutsLaunch ?? true
        onToggled: Daemon.setSettings({shortcutsLaunch: checked})
    }

    QQC2.Label {
        Kirigami.FormData.label: qsTr("This system:")
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: SystemControl.sandboxStatus() || qsTr("murasaki-sandbox is not installed: scene engines run without a sandbox.")
    }

    QQC2.Switch {
        id: requireSwitch
        Kirigami.FormData.label: qsTr("Strict:")
        text: qsTr("Do not start a scene engine that would run without the sandbox")
        enabled: Daemon.available
        checked: page.requireSandbox
        onToggled: Daemon.setSettings({requireSandbox: checked})
    }

    Hint {
        text: qsTr("Workshop scenes are not reviewed by anyone. In the sandbox the scene engine can only read the wallpaper, the system's libraries and the graphics drivers' information, can only write the graphics cache, and has no network. Turn it off only to find out whether it causes a problem; scenes on screen start again.")
    }
}
