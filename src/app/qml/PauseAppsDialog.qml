import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// The applications that pause the wallpapers while one of their windows has the focus or is fullscreen (the pauseApps setting),
// picked from the installed applications. A class typed by hand (with * and ?) stays possible under "More options".
KirigamiDialogs.Dialog {
    id: dialog

    property var apps: []
    signal saved(var apps)

    AutomationText { id: words }

    function openWith(current) {
        apps = (current ?? []).slice();
        typedField.text = "";
        moreOptions.checked = false;
        open();
    }

    function add(appClass) {
        // the daemon takes at most 64 classes of at most 100 characters
        const clean = String(appClass ?? "").trim();
        if (clean !== "" && clean.length <= 100 && apps.length < 64 && !apps.some(a => a.toLowerCase() === clean.toLowerCase()))
            apps = apps.concat([clean]);
    }

    title: qsTr("Pause for applications")
    standardButtons: KirigamiDialogs.Dialog.Save | KirigamiDialogs.Dialog.Cancel
    preferredWidth: Kirigami.Units.gridUnit * 30
    padding: Kirigami.Units.largeSpacing

    onAccepted: {
        Daemon.setSettings({pauseApps: apps});
        saved(apps);
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Hint {
            text: qsTr("The wallpapers pause while a window of one of these applications has the focus, or one of them is fullscreen. Needs KDE Plasma.")
        }

        Repeater {
            model: dialog.apps

            RowLayout {
                required property string modelData
                required property int index
                Layout.fillWidth: true

                Kirigami.Icon {
                    source: words.appIcon(modelData) || "application-x-executable"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: words.appName(modelData)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                }
                QQC2.ToolButton {
                    icon.name: "list-remove"
                    text: qsTr("Remove")
                    display: QQC2.AbstractButton.IconOnly
                    onClicked: dialog.apps = dialog.apps.filter((a, i) => i !== index)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: text
                }
            }
        }

        QQC2.Label {
            visible: dialog.apps.length === 0
            opacity: 0.7
            text: qsTr("No application yet.")
        }

        AppPicker {
            placeholder: qsTr("Add an application…")
            onPicked: cls => {
                dialog.add(cls);
                appClass = ""; // ready for the next one
            }
        }

        QQC2.Button {
            id: moreOptions
            flat: true
            checkable: true
            icon.name: checked ? "arrow-down" : "arrow-right"
            text: qsTr("More options")
        }

        RowLayout {
            visible: moreOptions.checked
            QQC2.TextField {
                id: typedField
                Layout.fillWidth: true
                maximumLength: 100
                placeholderText: qsTr("A window class such as steam_app_* (* and ? match)")
                onAccepted: {
                    dialog.add(text);
                    text = "";
                }
            }
            QQC2.Button {
                icon.name: "list-add"
                text: qsTr("Add")
                enabled: typedField.text.trim() !== ""
                onClicked: {
                    dialog.add(typedField.text);
                    typedField.text = "";
                }
            }
        }
    }
}
