import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Wallpaper Engine stores colors as "r g b" floats in 0..1.
RowLayout {
    id: root

    property string value: "0 0 0"
    signal edited(var newValue)

    // Some projects write 0..255 components; anything above 1 is taken as that scale.
    function toColor(text) {
        const parts = String(text).trim().split(/\s+/).map(Number);
        if (parts.length < 3 || parts.some(isNaN))
            return Qt.rgba(0, 0, 0, 1);
        const scale = Math.max(parts[0], parts[1], parts[2]) > 1 ? 255 : 1;
        return Qt.rgba(parts[0] / scale, parts[1] / scale, parts[2] / scale, 1);
    }

    function fromColor(color) {
        const f = c => String(Math.round(c * 100000) / 100000);
        return f(color.r) + " " + f(color.g) + " " + f(color.b);
    }

    readonly property color swatch: toColor(value)

    spacing: Kirigami.Units.smallSpacing

    QQC2.Button {
        id: button
        implicitWidth: Kirigami.Units.gridUnit * 6
        onClicked: {
            dialog.selectedColor = root.swatch;
            dialog.open();
        }

        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Rectangle {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                Layout.preferredHeight: Kirigami.Units.gridUnit
                color: root.swatch
                border.color: Kirigami.Theme.textColor
                border.width: 1
                radius: 2
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: root.swatch.toString().toUpperCase()
            }
        }
    }

    ColorDialog {
        id: dialog
        onAccepted: root.edited(root.fromColor(selectedColor))
    }
}
