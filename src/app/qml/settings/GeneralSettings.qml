import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, General: what starts with the session and how Plasma shows the wallpaper.
Kirigami.FormLayout {
    id: page

    Component.onCompleted: {
        SystemControl.refreshAutostart();
        SystemControl.refreshPlasma();
    }

    Connections {
        target: SystemControl
        function onAutostartEnabledChanged() { autostartSwitch.checked = SystemControl.autostartEnabled; }
        function onPlasmaEnabledChanged() { plasmaSwitch.checked = SystemControl.plasmaEnabled; }
    }

    QQC2.Switch {
        id: autostartSwitch
        Kirigami.FormData.label: qsTr("Autostart:")
        text: qsTr("Show the wallpaper when I log in")
        enabled: !SystemControl.autostartBusy
        checked: SystemControl.autostartEnabled
        onToggled: SystemControl.setAutostart(checked)
    }

    // Only on Plasma: the wallpaper becomes Plasma's own desktop wallpaper (behind icons and widgets).

    QQC2.Switch {
        id: plasmaSwitch
        visible: SystemControl.plasmaSupported
        Kirigami.FormData.label: qsTr("Desktop integration (Plasma):")
        text: qsTr("Show the wallpaper behind the desktop icons")
        enabled: !SystemControl.plasmaBusy
        checked: SystemControl.plasmaEnabled
        onToggled: SystemControl.setPlasma(checked)
    }
}
