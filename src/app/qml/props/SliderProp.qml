import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

// A numeric range. Shows whole numbers when the step is integral.
RowLayout {
    id: root

    property real value: 0
    property real from: 0
    property real to: 1
    property real step: 1
    signal edited(var newValue)

    readonly property bool integral: Math.abs(step - Math.round(step)) < 1e-9

    function format(v) {
        return integral ? Math.round(v).toString() : Number(v).toFixed(2);
    }

    QQC2.Slider {
        Layout.fillWidth: true
        Layout.minimumWidth: 60
        from: root.from
        to: root.to
        stepSize: root.step
        value: root.value
        onMoved: root.edited(value)
    }

    QQC2.Label {
        Layout.minimumWidth: 40
        horizontalAlignment: Text.AlignRight
        text: root.format(root.value)
    }
}
