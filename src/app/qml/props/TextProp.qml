import QtQuick
import QtQuick.Controls as QQC2

QQC2.TextField {
    id: root

    property string value: ""
    signal edited(var newValue)

    implicitWidth: 120
    text: value
    onValueChanged: text = value
    onEditingFinished: {
        if (text !== value)
            root.edited(text);
    }
}
