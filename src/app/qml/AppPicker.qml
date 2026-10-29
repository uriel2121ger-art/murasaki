import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Picks an installed application (from its .desktop file, followed live as applications come and go) and gives the window class
// its windows carry, which is what a trigger or a pause rule stores. Typing a class by hand stays possible under "More options".
QQC2.Button {
    id: picker

    // The chosen window class ("" for none).
    property string appClass: ""
    property string placeholder: qsTr("Choose an application…")
    signal picked(string appClass)

    AutomationText { id: words }

    readonly property string appIcon: words.appIcon(appClass)

    // Application names come from files. The desktop style paints the text itself, as plain text (a contentItem on top doubled
    // it); "&&" keeps an "&" in a name from turning into a mnemonic.
    text: (appClass === "" ? placeholder : words.appName(appClass)).replace(/&/g, "&&")
    icon.name: appClass === "" ? "application-x-executable" : (appIcon !== "" ? appIcon : "application-x-executable")
    onClicked: popup.open()

    QQC2.Popup {
        id: popup

        y: picker.height
        width: Math.max(picker.width, Kirigami.Units.gridUnit * 18)
        height: Kirigami.Units.gridUnit * 20
        padding: Kirigami.Units.smallSpacing
        focus: true

        // The catalogue's filter is shared: it is this picker's only while the list is open.
        onOpened: {
            ShortcutCatalog.filter = ""; // another picker may have left one
            search.text = "";
            search.forceActiveFocus();
        }
        onClosed: ShortcutCatalog.filter = ""

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            Kirigami.SearchField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search by name or window class…")
                onTextChanged: ShortcutCatalog.filter = text
                Keys.onDownPressed: list.forceActiveFocus()
                onAccepted: if (list.count > 0) list.choose(0)
            }

            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: list

                    function choose(row) {
                        const item = itemAtIndex(row);
                        if (!item)
                            return;
                        picker.picked(item.appClass); // the owner sets appClass: a write here would break its binding
                        popup.close();
                    }

                    model: ShortcutCatalog
                    clip: true
                    keyNavigationEnabled: true
                    Keys.onReturnPressed: if (currentIndex >= 0) choose(currentIndex)

                    delegate: QQC2.ItemDelegate {
                        id: row

                        required property int index
                        required property string appName
                        required property string appIcon
                        required property string appClass

                        width: ListView.view.width
                        text: appName
                        icon.name: appIcon !== "" ? appIcon : "application-x-executable"
                        highlighted: ListView.isCurrentItem
                        onClicked: list.choose(index)
                        contentItem: RowLayout {
                            spacing: Kirigami.Units.largeSpacing
                            Kirigami.Icon {
                                source: row.icon.name
                                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: row.appName
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: row.appClass
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    opacity: 0.7
                                    font: Kirigami.Theme.smallFont
                                }
                            }
                        }
                    }

                    Kirigami.PlaceholderMessage {
                        anchors.centerIn: parent
                        width: parent.width - Kirigami.Units.gridUnit * 2
                        visible: list.count === 0
                        text: ShortcutCatalog.total === 0 ? qsTr("Looking for applications…") : qsTr("No application matches")
                    }
                }
            }
        }
    }
}
