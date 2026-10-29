import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App
import "props"

// Edits a PropertyModel: one row per visible property, a delegate per property type.
ColumnLayout {
    id: editor

    spacing: Kirigami.Units.largeSpacing

    required property PropertyModel model

    Repeater {
        model: editor.model

        delegate: RowLayout {
            id: row

            required property int index
            required property var model

            Layout.fillWidth: true
            visible: model.visible
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                Layout.maximumWidth: Kirigami.Units.gridUnit * 10
                Layout.alignment: Qt.AlignTop
                horizontalAlignment: Text.AlignRight
                wrapMode: Text.Wrap
                text: row.model.text
                textFormat: Text.PlainText
            }

            Loader {
                id: loader
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                Layout.alignment: Qt.AlignTop
                sourceComponent: row.component(row.model.type)
            }

            function commit(value) {
                editor.model.setValue(index, value);
            }

            function component(type) {
                switch (type) {
                case "slider": return sliderComponent;
                case "bool": return boolComponent;
                case "color": return colorComponent;
                case "combo": return comboComponent;
                case "textinput": return textComponent;
                case "file":
                case "directory": return fileComponent;
                case "usershortcut": return shortcutComponent;
                }
                return null;
            }

            Component {
                id: shortcutComponent
                ShortcutProp {
                    value: String(row.model.value ?? "")
                    onEdited: v => row.commit(v)
                }
            }
            Component {
                id: sliderComponent
                SliderProp {
                    value: Number(row.model.value)
                    from: row.model.min
                    to: row.model.max
                    step: row.model.effectiveStep
                    onEdited: v => row.commit(v)
                }
            }
            Component {
                id: boolComponent
                BoolProp {
                    value: Boolean(row.model.value)
                    onEdited: v => row.commit(v)
                }
            }
            Component {
                id: colorComponent
                ColorProp {
                    value: String(row.model.value)
                    onEdited: v => row.commit(v)
                }
            }
            Component {
                id: comboComponent
                ComboProp {
                    value: row.model.value
                    options: row.model.options
                    onEdited: v => row.commit(v)
                }
            }
            Component {
                id: textComponent
                TextProp {
                    value: String(row.model.value)
                    onEdited: v => row.commit(v)
                }
            }
            Component {
                id: fileComponent
                FileProp {
                    value: String(row.model.value)
                    directory: row.model.type === "directory"
                    onEdited: v => row.commit(v)
                }
            }
        }
    }
}
