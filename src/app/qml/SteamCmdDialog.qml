import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Signs in to Steam through SteamCMD with the user's own account. The password goes to the daemon once, when SteamCMD
// asks for it, and is cleared from the field at once; it is never stored (SteamCMD keeps its own login).
KirigamiDialogs.Dialog {
    id: dialog

    readonly property var status: Daemon.steamCmd
    readonly property string state: status.state ?? "unavailable"
    readonly property string prompt: status.prompt ?? ""
    // Typed before SteamCMD asked for it; sent when it does, then cleared.
    property string pendingPassword: ""
    // The account this dialog started a login for: only its password prompt is answered with the typed password.
    property string startedUser: ""
    // Emitted once an account is signed in as a result of this dialog (a download waiting for it goes ahead).
    signal signedIn()

    title: qsTr("Steam account (SteamCMD)")
    standardButtons: KirigamiDialogs.Dialog.NoButton
    preferredWidth: Kirigami.Units.gridUnit * 26
    padding: Kirigami.Units.largeSpacing

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    function startLogin() {
        if (userField.text.trim() === "" || passwordField.text === "")
            return;
        dialog.pendingPassword = passwordField.text;
        dialog.startedUser = userField.text.trim();
        passwordField.text = "";
        Daemon.steamCmdLogin(dialog.startedUser);
    }

    onOpened: {
        userField.text = status.user ?? "";
        passwordField.text = "";
        codeField.text = "";
        if (state === "logged-out")
            (userField.text === "" ? userField : passwordField).forceActiveFocus();
    }
    onClosed: {
        pendingPassword = "";
        startedUser = "";
        askedPasswordField.text = "";
        passwordField.text = "";
        codeField.text = "";
        if (state === "logging-in")
            Daemon.steamCmdCancelLogin();
    }

    Connections {
        target: Daemon
        function onSteamCmdChanged() {
            if (dialog.pendingPassword !== "" && dialog.prompt === "password") {
                if (dialog.startedUser !== "" && (dialog.status.user ?? "") === dialog.startedUser) {
                    const secret = dialog.pendingPassword;
                    dialog.pendingPassword = "";
                    Daemon.steamCmdSubmit("password", secret);
                } else {
                    dialog.pendingPassword = ""; // another account is asking: the typed password is not for it
                }
            }
            if (dialog.state === "logged-in" && dialog.opened) {
                dialog.close();
                dialog.signedIn();
            } else if (dialog.state === "logged-out" && dialog.pendingPassword !== "" && (dialog.status.message ?? "") !== "") {
                dialog.pendingPassword = ""; // the login failed before it asked for the password
            }
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("This signs in with your own Steam account through SteamCMD, Valve's official command-line tool, to download Workshop items. You must own Wallpaper Engine on that account. Murasaki never stores your password: SteamCMD keeps its own login.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: dialog.state === "unavailable"
            type: Kirigami.MessageType.Warning
            text: qsTr("SteamCMD is not installed. Install it with your package manager (on Ubuntu: sudo apt install steamcmd), then restart the Murasaki service.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: (dialog.status.message ?? "") !== "" && dialog.state === "logged-out"
            type: Kirigami.MessageType.Error
            text: dialog.plain(dialog.status.message)
        }

        // Signed out: account and password.
        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: dialog.state === "logged-out"

            QQC2.TextField {
                id: userField
                Kirigami.FormData.label: qsTr("Account name:")
                maximumLength: 64
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                onAccepted: passwordField.forceActiveFocus()
            }
            QQC2.TextField {
                id: passwordField
                Kirigami.FormData.label: qsTr("Password:")
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onAccepted: dialog.startLogin()
            }
            QQC2.Button {
                icon.name: "dialog-ok-apply"
                text: qsTr("Sign in")
                enabled: userField.text.trim() !== "" && passwordField.text !== ""
                onClicked: dialog.startLogin()
            }
        }

        // A login is running.
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.state === "logging-in" && dialog.prompt === ""
            spacing: Kirigami.Units.largeSpacing
            QQC2.BusyIndicator { running: parent.visible }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Signing in…")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: dialog.state === "logging-in" && dialog.prompt === "confirm"
            spacing: Kirigami.Units.largeSpacing
            QQC2.BusyIndicator { running: parent.visible }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Confirm the sign-in in the Steam Mobile app on your phone.")
            }
        }

        // A login that this dialog did not start (another client, or after the dialog was reopened) asks for a password:
        // say so and let the user answer it, instead of sending a password typed for something else.
        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: dialog.state === "logging-in" && dialog.prompt === "password" && dialog.pendingPassword === ""

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("SteamCMD asks for the password of %1.").arg(dialog.status.user ?? "")
            }
            QQC2.TextField {
                id: askedPasswordField
                Kirigami.FormData.label: qsTr("Password:")
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onAccepted: sendAsked.clicked()
            }
            QQC2.Button {
                id: sendAsked
                icon.name: "dialog-ok-apply"
                text: qsTr("Send password")
                enabled: askedPasswordField.text !== ""
                onClicked: {
                    const secret = askedPasswordField.text;
                    askedPasswordField.text = "";
                    Daemon.steamCmdSubmit("password", secret);
                }
            }
        }

        // Steam Guard code (email or authenticator).
        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: dialog.state === "logging-in" && dialog.prompt === "guard"

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Steam Guard needs a code: the one from the email Steam sent you, or from your authenticator app.")
            }
            QQC2.TextField {
                id: codeField
                Kirigami.FormData.label: qsTr("Steam Guard code:")
                maximumLength: 16
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhUppercaseOnly
                onAccepted: submitCode.clicked()
                onVisibleChanged: if (visible) forceActiveFocus()
            }
            QQC2.Button {
                id: submitCode
                icon.name: "dialog-ok-apply"
                text: qsTr("Submit code")
                enabled: codeField.text.trim() !== ""
                onClicked: {
                    Daemon.steamCmdSubmit("guard", codeField.text.trim());
                    codeField.text = "";
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: dialog.state === "logging-in"
            QQC2.Button {
                icon.name: "dialog-cancel"
                text: qsTr("Cancel")
                onClicked: Daemon.steamCmdCancelLogin()
            }
        }

        // Signed in.
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.state === "logged-in"
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("Signed in as %1.").arg(dialog.status.user ?? "")
            }
            QQC2.Button {
                icon.name: "system-log-out"
                text: qsTr("Sign out")
                onClicked: Daemon.steamCmdLogout()
            }
        }
    }
}
