import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// The pets on the desktop saved under a name, and switched in one step (docs/pets/automation.md). A profile holds the pets only; switching keeps the windows of the
// pets that exist in both. Names come from files: they are shown as plain text.
Kirigami.ScrollablePage {
    id: page

    title: qsTr("Profiles")

    property string renaming: ""
    property string deleting: ""

    // For the messages (Kirigami.InlineMessage reads markup): no angle bracket or ampersand of a name reaches them.
    function safe(s) {
        return s === undefined || s === null ? "" : String(s).replace(/&/g, "＆").replace(/</g, "‹").replace(/>/g, "›");
    }

    function summary(profile) {
        return qsTr("%n pet(s)", "", (profile.pets ?? []).length);
    }

    function reasonText(reason) {
        switch (reason) {
        case "profile: item missing": return qsTr("its animation is no longer in the library");
        case "profile: variant missing": return qsTr("its animation is gone");
        default: return String(reason);
        }
    }

    // For the smoke test: opens and closes the dialogs once, and presses "Try it".
    function smoke() {
        demo.run();
        demoRemoveDialog.open();
        demoRemoveDialog.close();
        card.dismiss();
        saveDialog.open();
        saveDialog.close();
        renameDialog.open();
        renameDialog.close();
        deleteDialog.open();
        deleteDialog.close();
    }

    actions: [
        Kirigami.Action {
            id: saveAction
            icon.name: "document-save"
            text: qsTr("Save the pets as…")
            enabled: Daemon.automationSupported && Daemon.petsAvailable
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: saveDialog.open()
        },
        Kirigami.Action {
            icon.name: "media-playback-start"
            text: qsTr("Try it")
            tooltip: qsTr("Save the pets as an example profile and switch to it")
            enabled: Daemon.automationSupported && Daemon.demosSupported && Daemon.petsAvailable
            onTriggered: demo.run()
        },
        Kirigami.Action {
            icon.name: "edit-clear-all"
            text: qsTr("Remove examples")
            visible: Daemon.hasDemos
            enabled: Daemon.automationSupported
            onTriggered: demoRemoveDialog.open()
        }
    ]

    DemoSupport {
        id: demo
        kind: "profiles"
        onDone: result => {
            message.profileId = result.profile;
            message.say(qsTr("Saved your pets as an example profile; nothing else changed. To see switching work, move or remove a pet and then switch to the example: the pets come back as they are saved. Switching also makes your timelines that use a pet slot follow the pets of that profile; removing the examples goes back to the profile you had."), qsTr("Switch to this profile"));
        }
        onNeedsPet: message.missingPet()
    }

    DemoRemoveDialog { id: demoRemoveDialog }

    PlaylistNameDialog {
        id: saveDialog
        title: qsTr("Save the pets as a profile")
        initialName: ""
        onNamed: name => Daemon.saveProfile(name)
    }

    PlaylistNameDialog {
        id: renameDialog
        title: qsTr("Rename the profile")
        initialName: page.renaming === "" ? "" : ((Daemon.profiles ?? []).find(p => p.id === page.renaming)?.name ?? "")
        onNamed: name => {
            if (page.renaming !== "")
                Daemon.renameProfile(page.renaming, name);
        }
    }

    KirigamiDialogs.PromptDialog {
        id: deleteDialog
        title: qsTr("Delete the profile?")
        subtitle: qsTr("The pets on the desktop stay as they are.")
        standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel
        onAccepted: Daemon.deleteProfile(page.deleting)
    }

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        DemoMessage {
            id: message
            property string profileId: ""
            onExtraTriggered: Daemon.switchProfile(profileId)
        }

        DemoCard {
            id: card
            kind: "profiles"
            body: qsTr("A profile is your pets saved under a name, to come back to that setup in one step. Try it to save the pets you have now as an example and see how switching works. Nothing about your pets changes.")
            onTried: demo.run()
        }

        // What the last switch could not do.
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: (Daemon.lastSwitchReport.skipped ?? []).length > 0
            type: Kirigami.MessageType.Warning
            text: {
                const skipped = Daemon.lastSwitchReport.skipped ?? [];
                const lines = skipped.slice(0, 5).map(s => qsTr("a pet of %1: %2").arg(page.safe(String(s.itemId).slice(0, 60))).arg(page.safe(page.reasonText(s.reason))));
                return qsTr("Some pets were skipped when switching to \"%1\" (%2).").arg(page.safe(String(Daemon.lastSwitchReport.profile ?? "").slice(0, 60))).arg(lines.join("; "));
            }
            showCloseButton: true
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: Daemon.activeProfile !== "" && Daemon.profileModified
            type: Kirigami.MessageType.Information
            text: qsTr("The pets on the desktop differ from the active profile. Save them under its name to keep the changes, or switch to it to go back.")
        }

        Repeater {
            model: Daemon.profiles

            delegate: QQC2.ItemDelegate {
                id: row

                required property var modelData

                Layout.fillWidth: true

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Icon {
                        source: "view-preview"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.bold: row.modelData.id === Daemon.activeProfile
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            opacity: 0.7
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            text: page.summary(row.modelData) + (row.modelData.origin ? " · " + qsTr("from %1").arg(String(row.modelData.origin).slice(0, 80)) : "")
                        }
                    }

                    QQC2.Label {
                        visible: row.modelData.id === Daemon.activeProfile
                        color: Kirigami.Theme.highlightColor
                        text: Daemon.profileModified ? qsTr("Active · modified") : qsTr("Active")
                    }
                    QQC2.Button {
                        text: qsTr("Switch")
                        icon.name: "media-playback-start"
                        enabled: Daemon.automationSupported && Daemon.petsAvailable
                        onClicked: Daemon.switchProfile(row.modelData.id)
                    }
                    QQC2.ToolButton {
                        icon.name: "edit-rename"
                        text: qsTr("Rename")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: {
                            page.renaming = row.modelData.id;
                            renameDialog.open();
                        }
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: text
                    }
                    QQC2.ToolButton {
                        icon.name: "edit-delete"
                        text: qsTr("Delete")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: {
                            page.deleting = row.modelData.id;
                            deleteDialog.open();
                        }
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: text
                    }
                }
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.gridUnit * 3
            visible: (Daemon.profiles ?? []).length === 0
            icon.name: Daemon.automationSupported ? "view-preview" : "dialog-warning"
            text: Daemon.automationSupported ? qsTr("No profiles yet") : qsTr("Profiles need a newer Murasaki service")
            explanation: Daemon.automationSupported
                ? qsTr("Put your pets where you like them, then save them as a profile to come back to that setup in one step. A profile holds the pets only: not the wallpapers, not the settings.")
                : qsTr("Update and restart the Murasaki service.")
            helpfulAction: Daemon.automationSupported && Daemon.petsAvailable ? saveAction : null
        }
    }
}
