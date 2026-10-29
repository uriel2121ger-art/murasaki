import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, Steam account: the SteamCMD sign-in for Workshop downloads.
Kirigami.FormLayout {
    id: page

    SteamCmdDialog {
        id: steamCmdDialog
    }

    QQC2.Label {
        Kirigami.FormData.label: qsTr("Status:")
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: {
            if (!Daemon.steamCmdSupported)
                return qsTr("Needs a newer Murasaki service.");
            switch (Daemon.steamCmd.state ?? "unavailable") {
            case "unavailable": return qsTr("SteamCMD is not installed.");
            case "logging-in": return qsTr("Signing in…");
            case "logged-in": return qsTr("Signed in as %1.").arg(Daemon.steamCmd.user ?? "");
            default: return qsTr("Not signed in.");
            }
        }
    }

    RowLayout {
        Kirigami.FormData.label: qsTr("Account:")
        spacing: Kirigami.Units.smallSpacing
        QQC2.Button {
            icon.name: "im-user"
            text: qsTr("Sign in…")
            enabled: Daemon.steamCmdSupported && (Daemon.steamCmd.available ?? false)
            visible: (Daemon.steamCmd.state ?? "") !== "logged-in"
            onClicked: steamCmdDialog.open()
        }
        QQC2.Button {
            icon.name: "system-log-out"
            text: qsTr("Sign out")
            visible: (Daemon.steamCmd.state ?? "") === "logged-in"
            onClicked: Daemon.steamCmdLogout()
        }
    }

    Hint {
        text: qsTr("Lets the Workshop page download items with your own Steam account through SteamCMD, Valve's official tool, even without the Steam client. You must own Wallpaper Engine on that account. Murasaki never stores your password; signing out also ends SteamCMD's own session.")
    }
}
