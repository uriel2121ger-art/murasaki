import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs

// Asks for the name of a playlist (a new one, or the new name of an existing one).
KirigamiDialogs.PromptDialog {
    id: dialog

    property string initialName: ""
    // Emitted with the trimmed name when the user confirms a non-empty one.
    signal named(string name)

    title: qsTr("New playlist")
    standardButtons: KirigamiDialogs.PromptDialog.Ok | KirigamiDialogs.PromptDialog.Cancel

    onOpened: {
        field.text = dialog.initialName;
        field.forceActiveFocus();
        field.selectAll();
    }
    onAccepted: {
        const name = field.text.trim();
        if (name !== "")
            dialog.named(name);
    }

    QQC2.TextField {
        id: field
        Layout.fillWidth: true
        placeholderText: qsTr("Name")
        maximumLength: 100
        onAccepted: dialog.accept()
    }
}
