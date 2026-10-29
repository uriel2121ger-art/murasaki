import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// The start and stop banner of the screen triggers (docs/pets/automation.md "Screen triggers"): whether Murasaki is looking at the screen right now, and the one button that starts
// it. Watching is never on by itself: it needs the setting, a screen trigger that is on, and this button; the desktop then asks which screens to share and shows its own sign.
ColumnLayout {
    id: banner

    readonly property var watch: Daemon.screenWatch ?? ({})
    readonly property string watchState: watch.state ?? "off"
    readonly property bool settingOn: Daemon.settings.screenTriggersEnabled ?? false
    readonly property int screenTriggerCount: (Daemon.triggers ?? []).filter(t => t.enabled && t.source?.kind === "screen").length
    readonly property bool running: watchState === "asking" || watchState === "watching"

    spacing: Kirigami.Units.smallSpacing
    visible: Daemon.screenSupported && (settingOn || screenTriggerCount > 0 || watchState !== "off")

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).slice(0, 200);
    }

    // The daemon's reasons are English sentences from a closed list; the known ones are translated here. Anything else (it can carry names from a pack or the desktop's own words) is
    // never put in the InlineMessage, whose label reads markup: it goes to the plain text label under it (odd).
    function reasonText(reason) {
        switch (reason) {
        case "": return "";
        case "screen: permission refused": return qsTr("You did not allow it: the desktop's question was answered with No.");
        case "screen: stopped by the desktop": return qsTr("The desktop stopped sharing the screen.");
        case "screen: portal not available": return qsTr("This desktop has no screen sharing service Murasaki can ask.");
        case "screen: portal error": return qsTr("The desktop's screen sharing service answered with an error.");
        case "screen: portal did not answer": return qsTr("The desktop did not answer in time.");
        case "screen: no monitor shared": return qsTr("No screen was shared.");
        case "screen: helper crashed": return qsTr("The program that looks at the screen stopped unexpectedly.");
        case "screen: helper keeps crashing, turned off": return qsTr("The program that looks at the screen keeps stopping, so screen triggers are turned off. Turn them on again in Settings to try once more.");
        case "screen: no PipeWire": return qsTr("This build cannot read the screen (no PipeWire).");
        case "sandbox unavailable": return qsTr("The sandbox that confines the program is not available here, so it was not started.");
        case "rescue": return qsTr("Stopped by the rescue.");
        case "settings": return qsTr("Stopped because screen triggers were turned off.");
        case "locked": return qsTr("Paused while the session is locked.");
        default: return "";
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: true
        showCloseButton: false
        type: banner.watchState === "watching" ? Kirigami.MessageType.Positive
            : (banner.watchState === "refused" || banner.watchState === "failed") ? Kirigami.MessageType.Warning
            : Kirigami.MessageType.Information
        text: {
            if (!banner.settingOn && banner.watchState === "off")
                return qsTr("Screen triggers are off. Turn them on to let a trigger look at regions of your screen.");
            switch (banner.watchState) {
            case "asking": return qsTr("The desktop is asking which screens Murasaki may look at. Answer its question.");
            case "watching": {
                const n = banner.watch.regions ?? 0;
                const paused = banner.watch.paused ? " " + qsTr("Paused for now.") : "";
                return qsTr("Watching %n region(s) of your screen, %1 times a second. The desktop shows its own sign while this is on.", "", n).arg(banner.watch.ticksPerSecond ?? 0) + paused;
            }
            case "stopped": return banner.reasonText(banner.watch.reason ?? "") || qsTr("Not watching.");
            case "refused": return banner.reasonText(banner.watch.reason ?? "") || qsTr("Not allowed.");
            case "failed": return banner.reasonText(banner.watch.reason ?? "") || qsTr("Could not start.");
            default:
                return banner.screenTriggerCount > 0
                    ? qsTr("Not watching. Press Watch the screen: the desktop will ask which screens to share.")
                    : qsTr("Not watching. Make a screen trigger and turn it on first.");
            }
        }
        actions: [
            Kirigami.Action {
                icon.name: "view-preview"
                text: qsTr("Watch the screen")
                visible: banner.settingOn && !banner.running
                enabled: banner.screenTriggerCount > 0
                onTriggered: Daemon.startScreenWatch()
            },
            Kirigami.Action {
                icon.name: "media-playback-stop"
                text: qsTr("Stop")
                visible: banner.running
                onTriggered: Daemon.stopScreenWatch()
            },
            Kirigami.Action {
                icon.name: "dialog-ok"
                text: qsTr("Turn on")
                visible: !banner.settingOn
                onTriggered: Daemon.setSettings({screenTriggersEnabled: true})
            }
        ]
    }

    QQC2.Label {
        id: oddReason
        readonly property string odd: banner.reasonText(banner.watch.reason ?? "") === "" ? banner.plain(banner.watch.reason) : ""
        Layout.fillWidth: true
        visible: odd !== "" && (banner.watchState === "stopped" || banner.watchState === "refused" || banner.watchState === "failed")
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: odd
    }

    Repeater {
        model: banner.watch.notes ?? []
        delegate: QQC2.Label {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            textFormat: Text.PlainText
            text: banner.plain(modelData)
        }
    }
}
