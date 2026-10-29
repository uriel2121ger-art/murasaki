import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, Automation: the triggers switch and the screen triggers.
Kirigami.FormLayout {
    id: page

    // Puts the controls back to the daemon's values (after SettingsChanged, or a rejected change).
    function sync() {
        automationSwitch.checked = Daemon.settings.automationEnabled ?? true;
        fireByNameSwitch.checked = Daemon.settings.fireByTimelineName ?? false;
        cooldownBox.value = Daemon.settings.triggerCooldownMs ?? 500;
        screenSwitch.checked = Daemon.settings.screenTriggersEnabled ?? false;
        screenRateBox.value = Daemon.settings.screenTicksPerSecond ?? 5;
        screenRememberSwitch.checked = Daemon.settings.screenRemember ?? false;
        screenLoginSwitch.checked = Daemon.settings.screenWatchAtLogin ?? false;
        screenPetsSwitch.checked = Daemon.settings.screenIgnorePets ?? true;
    }

    Component.onCompleted: sync()

    Connections {
        target: Daemon
        function onSettingsChanged() { page.sync(); }
        function onError() { page.sync(); }
    }

    QQC2.Switch {
        id: automationSwitch
        Kirigami.FormData.label: qsTr("Triggers:")
        enabled: Daemon.automationSupported
        text: qsTr("Triggers are on")
        checked: Daemon.settings.automationEnabled ?? true
        onToggled: Daemon.setSettings({automationEnabled: checked})
    }

    QQC2.Switch {
        id: fireByNameSwitch
        enabled: Daemon.automationSupported
        text: qsTr("Calling a name that no trigger has plays the timeline of that name")
        checked: Daemon.settings.fireByTimelineName ?? false
        onToggled: Daemon.setSettings({fireByTimelineName: checked})
    }

    QQC2.SpinBox {
        id: cooldownBox
        Kirigami.FormData.label: qsTr("A new trigger waits (ms):")
        enabled: Daemon.automationSupported
        from: 100
        to: 86400000
        stepSize: 100
        editable: true
        value: Daemon.settings.triggerCooldownMs ?? 500
        onValueModified: Daemon.setSettings({triggerCooldownMs: value})
    }

    Hint {
        text: qsTr("Triggers and timelines act on the pets only; they never run a program or open a file or a web address. With the switch off nothing fires and nothing is watched.")
    }

    Kirigami.Separator {
        Kirigami.FormData.isSection: true
        Kirigami.FormData.label: qsTr("Screen triggers")
        visible: Daemon.screenSupported
    }

    QQC2.Switch {
        id: screenSwitch
        visible: Daemon.screenSupported
        text: qsTr("Allow triggers that look at regions of the screen")
        checked: Daemon.settings.screenTriggersEnabled ?? false
        onToggled: Daemon.setSettings({screenTriggersEnabled: checked})
    }

    QQC2.SpinBox {
        id: screenRateBox
        visible: Daemon.screenSupported
        Kirigami.FormData.label: qsTr("Looks a second:")
        enabled: screenSwitch.checked
        from: 1
        to: 16
        value: Daemon.settings.screenTicksPerSecond ?? 5
        onValueModified: Daemon.setSettings({screenTicksPerSecond: value})
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        QQC2.ToolTip.text: qsTr("More looks react sooner and use more of the processor. On battery at most 2 a second.")
    }

    QQC2.Switch {
        id: screenRememberSwitch
        visible: Daemon.screenSupported
        enabled: screenSwitch.checked
        text: qsTr("Remember which screens I shared (the desktop may then not ask again)")
        checked: Daemon.settings.screenRemember ?? false
        onToggled: Daemon.setSettings({screenRemember: checked})
    }

    QQC2.Switch {
        id: screenLoginSwitch
        visible: Daemon.screenSupported
        enabled: screenSwitch.checked
        text: qsTr("Ask to start watching when the session starts")
        checked: Daemon.settings.screenWatchAtLogin ?? false
        onToggled: Daemon.setSettings({screenWatchAtLogin: checked})
    }

    QQC2.Switch {
        id: screenPetsSwitch
        visible: Daemon.screenSupported
        enabled: screenSwitch.checked
        text: qsTr("Ignore a region that my own pets mostly cover")
        checked: Daemon.settings.screenIgnorePets ?? true
        onToggled: Daemon.setSettings({screenIgnorePets: checked})
    }

    Hint {
        visible: Daemon.screenSupported
        text: qsTr("Off until you turn it on, and even then watching starts only when you press Watch the screen in Automations, Advanced view and answer the desktop's question. Only a small separate program sees the pixels, without files or network; the desktop shows its own sign while it is on. Nothing of the screen is stored except the reference pictures you capture.")
    }
}
