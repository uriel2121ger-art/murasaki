import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Takes the reference pictures of one region of a screen trigger (docs/pets/automation.md "Screen triggers"): "Capture now" asks the daemon for the picture of the region as the
// screen shows it, the pictures are shown here, and nothing is kept until you press "Keep". They are scaled to 360 pixels at most and stored on this computer only.
KirigamiDialogs.Dialog {
    id: dialog

    property string triggerId: ""
    property int region: 0
    // idle, taking, shown, failed
    property string phase: "idle"
    property string request: ""
    property string problem: ""
    property var pictures: []

    readonly property bool watching: (Daemon.screenWatch?.state ?? "off") === "watching"

    title: qsTr("Reference pictures of region %1").arg(region + 1)
    standardButtons: KirigamiDialogs.Dialog.Close
    preferredWidth: Kirigami.Units.gridUnit * 34
    padding: Kirigami.Units.largeSpacing

    function openFor(id, regionIndex) {
        triggerId = id;
        region = regionIndex;
        phase = "idle";
        request = "";
        problem = "";
        pictures = [];
        open();
    }

    // Only a small PNG of ours is shown: the size the daemon says must be at most 360 pixels a side.
    function showResult(result) {
        if (result.error !== undefined && result.error !== "") {
            problem = String(result.error).slice(0, 200);
            phase = "failed";
            return;
        }
        // (DaemonClient has checked the pictures' headers already; these are the last, cheap checks on what is drawn.)
        const steps = (result.steps ?? []).slice(0, 64).map(s => ({width: Number(s.width), height: Number(s.height), png: s.png})).filter(s => s.width >= 1 && s.width <= 360 && s.height >= 1 && s.height <= 360 && typeof s.png === "string" && s.png.length < 6000000);
        if (steps.length === 0) {
            problem = qsTr("No picture came back.");
            phase = "failed";
            return;
        }
        request = String(result.request ?? "");
        pictures = steps;
        phase = "shown";
    }

    function keep() {
        Daemon.commitScreenReference(request);
        phase = "idle";
        pictures = [];
        close();
    }

    // The daemon is told to drop what it holds (it would forget it after a minute anyway).
    function discard() {
        if (request !== "")
            Daemon.discardScreenReference(request);
        phase = "idle";
        pictures = [];
        request = "";
    }

    onClosed: {
        if (phase === "taking")
            Daemon.cancelScreenCapture();
        else if (phase === "shown" && request !== "")
            Daemon.discardScreenReference(request);
        phase = "idle";
        pictures = [];
        request = "";
    }

    Connections {
        target: Daemon
        // DaemonClient only passes the result of the request this client asked for last (errors too); it must also be for this region.
        function onScreenCaptured(req, result) {
            if (dialog.phase !== "taking")
                return;
            if (result.error === undefined && (result.trigger !== dialog.triggerId || result.region !== dialog.region))
                return;
            dialog.showResult(result);
        }
    }

    Timer {
        id: giveUp
        interval: 30000 + stepsBox.value * 2000
        running: dialog.phase === "taking"
        onTriggered: {
            Daemon.cancelScreenCapture();
            dialog.problem = qsTr("The picture did not come in time.");
            dialog.phase = "failed";
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Show on the screen what should make the trigger fire, then press Capture now. Your own pets over the region are not part of what is compared, so move them away if you can.")
        }

        RowLayout {
            QQC2.Label { text: qsTr("Pictures:") }
            QQC2.SpinBox {
                id: stepsBox
                from: 1
                to: 64
                value: 1
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: qsTr("One for a picture that stays. More for something that plays: they are taken one after another, a moment apart.")
            }
            QQC2.Button {
                icon.name: "camera-photo"
                text: qsTr("Capture now")
                enabled: dialog.watching && dialog.phase !== "taking"
                onClicked: {
                    dialog.pictures = [];
                    dialog.problem = "";
                    dialog.phase = "taking";
                    Daemon.captureScreenReference(dialog.triggerId, dialog.region, stepsBox.value);
                }
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !dialog.watching
            type: Kirigami.MessageType.Information
            text: qsTr("Start watching the screen first (the Watch the screen button on the Triggers page): the desktop asks you to share the screen, and then a picture can be taken.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: dialog.phase === "failed"
            type: Kirigami.MessageType.Warning
            text: qsTr("The picture could not be taken.")
        }

        // The daemon's words are plain text here (the InlineMessage above reads markup).
        QQC2.Label {
            Layout.fillWidth: true
            visible: dialog.phase === "failed" && dialog.problem !== ""
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: dialog.problem
        }

        QQC2.BusyIndicator {
            visible: dialog.phase === "taking"
            running: visible
        }

        Flow {
            Layout.fillWidth: true
            visible: dialog.phase === "shown"
            spacing: Kirigami.Units.smallSpacing
            Repeater {
                model: dialog.pictures
                delegate: Image {
                    required property var modelData
                    required property int index
                    width: Math.min(modelData.width, Kirigami.Units.gridUnit * 14)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    sourceSize: Qt.size(360, 360)
                    source: "data:image/png;base64," + modelData.png
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("Picture %1: %2 x %3").arg(index + 1).arg(modelData.width).arg(modelData.height)
                    QQC2.ToolTip.visible: hoverArea.containsMouse
                    QQC2.ToolTip.text: qsTr("Picture %1: %2 x %3").arg(index + 1).arg(modelData.width).arg(modelData.height)
                    MouseArea { id: hoverArea; anchors.fill: parent; hoverEnabled: true }
                }
            }
        }

        RowLayout {
            visible: dialog.phase === "shown"
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Keep these %n picture(s) as the reference? Nothing is kept until you say so.", "", dialog.pictures.length)
            }
            QQC2.Button {
                icon.name: "dialog-ok"
                text: qsTr("Keep")
                onClicked: dialog.keep()
            }
            QQC2.Button {
                icon.name: "edit-delete"
                text: qsTr("Discard")
                onClicked: dialog.discard()
            }
        }
    }
}
