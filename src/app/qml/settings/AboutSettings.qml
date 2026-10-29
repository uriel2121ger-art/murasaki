import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, About.
Kirigami.FormLayout {
    id: page

    QQC2.Label {
        Kirigami.FormData.label: qsTr("Version:")
        text: SystemControl.appVersion
    }

    QQC2.Label {
        Kirigami.FormData.label: qsTr("Service API version:")
        text: Daemon.apiVersion > 0 ? String(Daemon.apiVersion) : qsTr("Unknown")
    }

    QQC2.Label {
        Kirigami.FormData.label: qsTr("License:")
        text: qsTr("GPL-3.0-or-later. Murasaki is a local project.")
    }

    QQC2.Label {
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        wrapMode: Text.Wrap
        text: qsTr("Compatible with Wallpaper Engine items. Not affiliated with Wallpaper Engine or Valve.")
    }
}
