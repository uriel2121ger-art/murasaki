import QtQuick
import QtQuick.Controls as QQC2

QQC2.Switch {
    id: root

    property bool value: false
    signal edited(var newValue)

    checked: value
    onToggled: root.edited(checked)
}
