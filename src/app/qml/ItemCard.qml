import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One wallpaper in the library grid.
QQC2.ItemDelegate {
    id: card

    required property string itemId
    required property string title
    required property string kind
    required property string status
    required property url preview
    required property bool assigned
    property string format: ""
    property bool hasRisks: false
    property bool networkAllowed: false
    // The grid's keyboard cursor is on this card.
    property bool keyboardCurrent: false

    readonly property bool isGif: preview.toString().toLowerCase().endsWith(".gif")

    function kindLabel() {
        switch (kind) {
        case "video": return qsTr("Video");
        case "web": return qsTr("Web");
        case "scene": return qsTr("Scene");
        case "pet": return format === "puppet" ? qsTr("Puppet") : qsTr("Pet");
        case "asset": return qsTr("Asset pack");
        default: return qsTr("Unknown");
        }
    }

    function statusLabel() {
        switch (status) {
        case "missing-dependency": return qsTr("Missing dependency");
        case "invalid": return qsTr("Invalid");
        default: return "";
        }
    }

    padding: Kirigami.Units.smallSpacing
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: Kirigami.Units.cornerRadius
        color: card.pressed ? Qt.alpha(Kirigami.Theme.highlightColor, 0.25)
                            : card.hovered || card.keyboardCurrent ? Qt.alpha(Kirigami.Theme.highlightColor, 0.12) : "transparent"
        border.width: card.keyboardCurrent ? 2 : card.hovered ? 1 : 0
        border.color: card.keyboardCurrent ? Kirigami.Theme.highlightColor : Qt.alpha(Kirigami.Theme.highlightColor, 0.5)
    }

    Accessible.name: title
    Accessible.description: kindLabel()

    // A standalone tooltip, so its text can be forced to plain text (titles come from item files).
    QQC2.ToolTip {
        visible: card.hovered && card.title.length > 0
        delay: Kirigami.Units.toolTipDelay
        contentItem: QQC2.Label {
            text: card.title
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }

    // Previews line up at the top whatever the length of the title under them.
    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Rectangle {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.preferredHeight: width * 9 / 16
            color: Kirigami.Theme.alternateBackgroundColor
            radius: Kirigami.Units.cornerRadius
            clip: true

            Image {
                anchors.fill: parent
                source: card.preview
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                sourceSize.width: 480
                sourceSize.height: 270
                visible: !gifLoader.active || gifLoader.status !== Loader.Ready
            }

            // The animated image is only created while the pointer is over a GIF card.
            Loader {
                id: gifLoader
                anchors.fill: parent
                active: card.isGif && card.hovered
                sourceComponent: AnimatedImage {
                    source: card.preview
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    playing: true
                }
            }

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.large
                height: width
                source: "image-missing"
                visible: card.preview.toString() === ""
            }

            // Badges over the preview.
            Row {
                anchors {
                    left: parent.left
                    bottom: parent.bottom
                    margins: Kirigami.Units.smallSpacing
                }
                spacing: Kirigami.Units.smallSpacing

                Badge { text: card.kindLabel() }
                Badge {
                    visible: card.statusLabel() !== ""
                    text: card.statusLabel()
                    warning: true
                }
                Badge {
                    visible: card.networkAllowed
                    iconName: "network-connect"
                    text: qsTr("Internet")
                    warning: true
                }
                Badge {
                    visible: card.hasRisks
                    text: qsTr("Contains programs")
                    warning: true
                }
            }

            Badge {
                anchors {
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.smallSpacing
                }
                visible: card.assigned
                iconName: "media-playback-start"
                text: qsTr("On screen")
                highlighted: true
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            text: card.title
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        Item {
            Layout.fillHeight: true
        }
    }

    component Badge: Rectangle {
        id: badge

        property alias text: label.text
        property string iconName: ""
        property bool warning: false
        property bool highlighted: false

        readonly property color foreground: warning ? Kirigami.Theme.neutralTextColor
                                                    : highlighted ? Kirigami.Theme.highlightedTextColor : "white"

        implicitWidth: badgeRow.implicitWidth + Kirigami.Units.largeSpacing
        implicitHeight: badgeRow.implicitHeight + Kirigami.Units.smallSpacing
        radius: Kirigami.Units.cornerRadius
        color: warning ? Kirigami.Theme.neutralBackgroundColor
                       : highlighted ? Kirigami.Theme.highlightColor
                                     : Qt.rgba(0, 0, 0, 0.65)

        Row {
            id: badgeRow
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing / 2

            Kirigami.Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: badge.iconName !== ""
                source: badge.iconName
                width: Kirigami.Units.iconSizes.small
                height: width
                isMask: true
                color: badge.foreground
            }
            QQC2.Label {
                id: label
                font: Kirigami.Theme.smallFont
                color: badge.foreground
            }
        }
    }
}
