import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

Kirigami.ScrollablePage {
    id: page

    title: qsTr("Screens")

    // Bumped when the library changes, so item lookups re-evaluate.
    property int libraryRevision: 0

    // The library entry for an item id (title, preview, ...), or null when the item is gone or the id is empty.
    function libraryItem(itemId) {
        libraryRevision;
        const row = itemId ? Library.indexOf(itemId) : -1;
        return row >= 0 ? Library.get(row) : null;
    }

    function itemTitle(itemId) {
        const item = libraryItem(itemId);
        return item ? item.title : itemId;
    }

    function desktopName(desktop) {
        if (desktop === "*")
            return qsTr("All desktops");
        const found = Surface.desktops.find(d => d.id === desktop);
        return found ? (found.name || qsTr("Desktop %1").arg(found.number ?? "")) : desktop;
    }

    function outputName(output) {
        return output === "*" ? qsTr("All screens") : output;
    }

    // The assignment that decides what an output shows, with the daemon's precedence:
    // (output, desktop), (output, *), (*, desktop), (*, *). Null when nothing is assigned.
    function resolvedAssignment(output) {
        const desktop = output.desktop || "*";
        const keys = [[output.name, desktop], [output.name, "*"], ["*", desktop], ["*", "*"]];
        for (const [o, d] of keys) {
            const found = Daemon.assignments.find(a => a.output === o && a.desktop === d);
            if (found)
                return found;
        }
        return null;
    }

    function stateText(output) {
        if (!output.itemId)
            return qsTr("Nothing assigned");
        // The pause rules of the Settings page come first: they say why everything stopped.
        switch (output.pauseReason ?? "") {
        case "game": return qsTr("Paused — a game is running");
        case "battery": return qsTr("Paused — on battery");
        case "app": return qsTr("Paused — %1 has the focus or is fullscreen").arg(output.pauseDetail ?? "");
        }
        if (output.playing)
            return (output.fpsCap ?? 0) > 0 ? qsTr("Playing — limited to %1 fps on battery").arg(output.fpsCap) : qsTr("Playing");
        return output.occluded ? qsTr("Paused — covered") : qsTr("Paused");
    }

    // Mute toggles the very assignment entry that currently decides what this output shows, keeping its other
    // options (fill mode, properties). If that is a wildcard entry (e.g. "All screens"), every output that
    // uses it is muted or unmuted together: the entry is the unit that carries the option.
    function setMuted(entry, muted) {
        const options = {muted: muted};
        if (entry.fill)
            options.fill = entry.fill;
        if (entry.span)
            options.span = true;
        if (entry.properties && Object.keys(entry.properties).length > 0)
            options.properties = entry.properties;
        Daemon.setAssignment(entry.output, entry.desktop, entry.itemId, options);
    }

    // The page's live state is only worth polling while it is on screen.
    Binding {
        target: Surface
        property: "poll"
        value: true
        when: page.visible
        restoreMode: Binding.RestoreValue
    }

    Connections {
        target: Library
        function onCountChanged() { page.libraryRevision++; }
        function onDataChanged() { page.libraryRevision++; }
        function onModelReset() { page.libraryRevision++; }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        visible: Surface.available

        GridLayout {
            Layout.fillWidth: true
            visible: Surface.outputs.length > 0
            columns: Math.max(1, Math.min(3, Math.floor(width / (Kirigami.Units.gridUnit * 16))))
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing
            uniformCellWidths: true

            Repeater {
                model: Surface.outputs

                Kirigami.AbstractCard {
                    id: card

                    required property var modelData
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    readonly property var entry: page.resolvedAssignment(modelData)
                    readonly property var item: page.libraryItem(modelData.itemId)

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        RowLayout {
                            Layout.fillWidth: true

                            Kirigami.Icon {
                                source: "video-display"
                                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                                implicitHeight: implicitWidth
                            }
                            Kirigami.Heading {
                                Layout.fillWidth: true
                                level: 3
                                text: card.modelData.name
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: width * 9 / 16
                            color: Kirigami.Theme.alternateBackgroundColor
                            radius: Kirigami.Units.cornerRadius
                            clip: true

                            Image {
                                anchors.fill: parent
                                source: card.item ? card.item.preview : ""
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                sourceSize.width: 480
                                sourceSize.height: 270
                            }
                            Kirigami.Icon {
                                anchors.centerIn: parent
                                width: Kirigami.Units.iconSizes.large
                                height: width
                                source: "image-missing"
                                visible: !card.item || card.item.preview.toString() === ""
                            }
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            visible: card.modelData.itemId !== ""
                            text: page.itemTitle(card.modelData.itemId)
                            textFormat: Text.PlainText
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        EngineFailureMessage {
                            output: card.modelData.name
                            failure: card.modelData.failure
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: page.stateText(card.modelData)
                            textFormat: Text.PlainText
                            opacity: 0.7
                        }

                        QQC2.Switch {
                            id: muteSwitch
                            Layout.fillWidth: true
                            text: qsTr("Mute")
                            enabled: card.entry !== null
                            checked: card.entry ? card.entry.muted === true : false
                            onToggled: page.setMuted(card.entry, checked)
                            // Fall back to the real state if the change did not go through.
                            Connections {
                                target: Daemon
                                function onAssignmentsChanged() { muteSwitch.checked = card.entry ? card.entry.muted === true : false; }
                                function onError() { muteSwitch.checked = card.entry ? card.entry.muted === true : false; }
                            }
                        }
                    }
                }
            }
        }

        Kirigami.Heading {
            Layout.fillWidth: true
            level: 2
            visible: Daemon.available
            text: qsTr("Assignments")
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: Daemon.available && Daemon.assignments.length === 0
            opacity: 0.7
            wrapMode: Text.Wrap
            text: qsTr("No wallpaper is assigned yet. Pick one in the Library.")
        }

        Repeater {
            model: Daemon.available ? Daemon.assignments : []

            QQC2.ItemDelegate {
                id: row

                required property var modelData

                Layout.fillWidth: true
                hoverEnabled: false
                focusPolicy: Qt.NoFocus

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Rectangle {
                        implicitWidth: Kirigami.Units.gridUnit * 5
                        implicitHeight: implicitWidth * 9 / 16
                        color: Kirigami.Theme.alternateBackgroundColor
                        radius: Kirigami.Units.cornerRadius
                        clip: true

                        Image {
                            anchors.fill: parent
                            source: page.libraryItem(row.modelData.itemId)?.preview ?? ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize.width: 160
                            sourceSize.height: 90
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: page.itemTitle(row.modelData.itemId)
                            textFormat: Text.PlainText
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: qsTr("%1, %2").arg(page.outputName(row.modelData.output)).arg(page.desktopName(row.modelData.desktop))
                            textFormat: Text.PlainText
                            opacity: 0.7
                            elide: Text.ElideRight
                        }
                    }

                    QQC2.Button {
                        icon.name: "edit-clear"
                        text: qsTr("Clear")
                        onClicked: Daemon.clearAssignment(row.modelData.output, row.modelData.desktop)
                    }
                }
            }
        }
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: !Surface.available
        icon.name: "video-display"
        text: qsTr("The wallpaper is not running")
        explanation: qsTr("Start it to see your screens here.")
        helpfulAction: Kirigami.Action {
            icon.name: "media-playback-start"
            text: qsTr("Start")
            onTriggered: SystemControl.startSurface()
        }
    }
}
