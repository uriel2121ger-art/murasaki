import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts

// A file or directory picker; stores an absolute local path.
RowLayout {
    id: root

    property string value: ""
    property bool directory: false
    signal edited(var newValue)

    function toPath(url) {
        return decodeURIComponent(url.toString().replace(/^file:\/\//, ""));
    }

    QQC2.TextField {
        Layout.fillWidth: true
        Layout.minimumWidth: 60
        text: root.value
        Connections {
            target: root
            function onValueChanged() { text = root.value; }
        }
        placeholderText: root.directory ? qsTr("No folder selected") : qsTr("No file selected")
        onEditingFinished: {
            if (text !== root.value)
                root.edited(text);
        }
    }

    QQC2.Button {
        icon.name: root.directory ? "folder-open" : "document-open"
        text: qsTr("Browse…")
        onClicked: (root.directory ? folderDialog : fileDialog).open()
    }

    FileDialog {
        id: fileDialog
        onAccepted: root.edited(root.toPath(selectedFile))
    }

    FolderDialog {
        id: folderDialog
        onAccepted: root.edited(root.toPath(selectedFolder))
    }
}
