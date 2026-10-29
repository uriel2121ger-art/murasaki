import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// The application of a `usershortcut` property: a button with its icon and name that opens the list of installed applications.
// The stored value is the desktop id ("org.mozilla.firefox.desktop"); the wallpaper never sees it, only the application's name
// and icon (docs/design/specs/2026-10-21-user-shortcuts-design.md).
RowLayout {
    id: root

    property string value: ""
    signal edited(var newValue)

    readonly property string appName: value === "" ? "" : ShortcutCatalog.nameFor(value)
    readonly property string appIcon: value === "" ? "" : ShortcutCatalog.iconFor(value)

    QQC2.Button {
        id: choose
        Layout.fillWidth: true
        Layout.minimumWidth: 60
        icon.name: root.appIcon !== "" ? root.appIcon : "application-x-executable"
        text: root.value === "" ? qsTr("No application") : (root.appName !== "" ? root.appName : qsTr("Missing application: %1").arg(root.value))
        onClicked: {
            ShortcutCatalog.filter = "";
            ShortcutCatalog.refresh();
            popup.open();
            search.forceActiveFocus();
        }
    }

    QQC2.Button {
        icon.name: "edit-clear"
        visible: root.value !== ""
        QQC2.ToolTip.text: qsTr("Clear")
        QQC2.ToolTip.visible: hovered
        onClicked: root.edited("")
    }

    QQC2.Popup {
        id: popup
        parent: choose
        y: choose.height
        width: Math.max(choose.width, Kirigami.Units.gridUnit * 16)
        height: Kirigami.Units.gridUnit * 18
        modal: true
        focus: true

        ColumnLayout {
            anchors.fill: parent
            QQC2.TextField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search applications")
                onTextChanged: ShortcutCatalog.filter = text
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: ShortcutCatalog
                delegate: QQC2.ItemDelegate {
                    required property string appId
                    required property string appName
                    required property string appIcon
                    width: ListView.view.width
                    text: appName
                    icon.name: appIcon !== "" ? appIcon : "application-x-executable"
                    onClicked: {
                        popup.close();
                        root.edited(appId);
                    }
                }
                QQC2.Label {
                    anchors.centerIn: parent
                    visible: parent.count === 0
                    text: qsTr("No applications found")
                }
            }
        }
    }
}
