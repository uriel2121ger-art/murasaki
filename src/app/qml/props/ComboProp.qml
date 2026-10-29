import QtQuick
import QtQuick.Controls as QQC2

// Options are [{label, value}]; the stored value is compared loosely (WE mixes numbers and strings).
QQC2.ComboBox {
    id: root

    property var value
    property var options: []
    signal edited(var newValue)

    function indexOfOption(v) {
        for (let i = 0; i < options.length; ++i) {
            if (String(options[i].value) === String(v))
                return i;
        }
        return -1;
    }

    // Labels come from project.json, so the delegate shows plain text. The closed box keeps the style's own contentItem: the
    // desktop style paints displayText natively (plain text) and expects a TextField there; a Label on top doubled the text.
    delegate: QQC2.ItemDelegate {
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        highlighted: root.highlightedIndex === index
        contentItem: QQC2.Label {
            text: parent.modelData.label
            textFormat: Text.PlainText
            elide: Text.ElideRight
        }
    }
    model: options
    textRole: "label"
    currentIndex: indexOfOption(value)
    onActivated: index => root.edited(options[index].value)
}
