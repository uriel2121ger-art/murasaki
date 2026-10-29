import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// The sound of one pet on the desktop (docs/pets/audio.md): the sound file, when it plays, how loud, and a test button. A sound is off until a file is chosen
// and a mode other than "Never" is set. The file is converted once by the Murasaki service in a sandbox; nothing here decodes it.
ColumnLayout {
    id: root

    required property string petId
    required property var pet            // the pet's entry of ListPets
    required property bool settled       // no edit of this pet is waiting to be sent
    required property bool fromVideo     // the pet was imported (its video's sound may have been kept)
    required property string itemId
    property var overlayWindow: undefined // the overlay's Windows() entry of the pet

    readonly property bool supported: Daemon.petAudioSupported
    readonly property var sound: pet?.audio ?? ({})
    readonly property string asset: sound.asset ?? ""
    readonly property string mode: sound.mode ?? "none"
    readonly property var assetInfo: Daemon.petAudioAssets.find(a => a.asset === asset) ?? null
    readonly property var degraded: overlayWindow?.audio?.degraded ?? []
    property string busyJob: ""
    property string problem: ""

    spacing: Kirigami.Units.smallSpacing
    enabled: supported

    function setSound(changes) {
        Daemon.updatePet(petId, {audio: changes});
    }
    function seconds(ms) {
        return qsTr("%1 s").arg((ms / 1000).toFixed(1));
    }
    function degradedText(name) {
        switch (name) {
        case "audio unavailable": return qsTr("PipeWire is not available, so this pet cannot make a sound.");
        case "audio failed": return qsTr("The sound player stopped and could not be started again. It will try again in a moment.");
        case "audio asset missing": return qsTr("The chosen sound is no longer stored. Choose it again.");
        case "audio asset invalid": return qsTr("The stored sound is damaged. Choose the file again.");
        case "audio: animation does not loop": return qsTr("This animation is a single picture, so “Each time it loops” never starts.");
        case "audio: click needs a clickable pet": return qsTr("Click sounds need the pet to take clicks. Turn off “Click through the pet”.");
        default: return name;
        }
    }
    function importError(error) {
        const name = String(error).split(": ").slice(0, 2).join(": ");
        switch (name) {
        case "audio: not audio": return qsTr("This file is not a sound Murasaki can read.");
        case "audio: container not allowed": return qsTr("This kind of file is not accepted as a sound.");
        case "audio: no audio stream": return qsTr("This file has no sound.");
        case "audio: decoder not available": return qsTr("Murasaki cannot decode this sound.");
        case "audio: too long": return qsTr("The sound is longer than 10 minutes.");
        case "audio: too short": return qsTr("The sound is too short.");
        case "audio: over the size limit": return qsTr("The file is too big.");
        case "audio: time limit reached": return qsTr("Reading the sound took too long.");
        case "sandbox unavailable": return qsTr("The sandbox is required for imports and is not available.");
        default: return qsTr("The sound could not be imported: %1").arg(error);
        }
    }

    Connections {
        target: Daemon
        function onPetAudioImportFinished(jobId, ok, assetId, error) {
            if (jobId !== root.busyJob)
                return;
            root.busyJob = "";
            if (ok) {
                root.problem = "";
                root.setSound({asset: assetId});
            } else if (error !== "cancelled") {
                root.problem = root.importError(error);
            }
        }
        // The answers of the call this card made, told by its ticket: another card's import (or the same card's, long ago) is not this one's.
        function onPetAudioImportRefused(ticket, message) {
            if (ticket === root.askedTicket) {
                root.problem = message;
                root.askedTicket = 0;
            }
        }
        function onPetAudioImportStarted(jobId, ticket) {
            if (ticket === root.askedTicket) {
                root.busyJob = jobId;
                root.askedTicket = 0;
            }
        }
    }
    property int askedTicket: 0

    FileDialog {
        id: soundFile
        title: qsTr("Choose a sound")
        nameFilters: [
            qsTr("Sounds (*.mp3 *.wav *.ogg *.oga *.opus *.flac *.m4a *.aac *.wma)"),
            qsTr("Videos with a sound (*.mp4 *.m4v *.webm *.mkv *.mov)"),
            qsTr("All files (*)")
        ]
        onAccepted: {
            root.problem = "";
            root.askedTicket = Daemon.importPetAudio(SystemControl.localFile(selectedFile));
        }
    }

    RowLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            textFormat: Text.PlainText // (the name of a sound comes from a file: never rich text)
            text: root.busyJob !== "" ? qsTr("Reading the sound…")
                : root.asset === "" ? qsTr("No sound")
                : root.assetInfo ? qsTr("%1 · %2").arg(root.assetInfo.name ?? root.asset.slice(0, 8)).arg(root.seconds(root.assetInfo.durationMs ?? 0))
                : qsTr("A sound that is no longer stored")
        }
        QQC2.BusyIndicator {
            visible: root.busyJob !== ""
            running: visible
            Layout.preferredHeight: Kirigami.Units.gridUnit * 1.5
            Layout.preferredWidth: Kirigami.Units.gridUnit * 1.5
        }
        QQC2.Button {
            text: root.busyJob !== "" ? qsTr("Cancel") : qsTr("Choose a sound…")
            onClicked: root.busyJob !== "" ? Daemon.cancelPetAudioImport(root.busyJob) : soundFile.open()
        }
        QQC2.Button {
            visible: root.fromVideo
            enabled: root.busyJob === ""
            text: qsTr("Use this video's sound")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: qsTr("Only for a video imported with “Keep the sound”.")
            onClicked: {
                root.problem = "";
                root.askedTicket = Daemon.importItemAudio(root.itemId);
            }
        }
        QQC2.Button {
            visible: root.asset !== ""
            icon.name: "edit-delete"
            text: qsTr("Remove")
            onClicked: Daemon.updatePet(root.petId, {audio: null})
        }
    }

    QQC2.Label {
        visible: root.problem !== ""
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        wrapMode: Text.Wrap
        color: Kirigami.Theme.negativeTextColor
        textFormat: Text.PlainText // (the text can hold what the daemon was told: a file name, an error of a library)
        text: root.problem
    }

    RowLayout {
        enabled: root.asset !== ""
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label { text: qsTr("Plays:") }
        QQC2.ComboBox {
            id: modeBox
            textRole: "text"
            valueRole: "value"
            model: [
                {text: qsTr("Never"), value: "none"},
                {text: qsTr("Once, when the pet appears"), value: "once"},
                {text: qsTr("Each time the animation loops"), value: "animation_loop"},
                {text: qsTr("Over and over"), value: "continuous"},
                {text: qsTr("When the pet is clicked"), value: "click"}
            ]
            onActivated: root.setSound({mode: currentValue})
            Binding { target: modeBox; property: "currentIndex"; value: modeBox.indexOfValue(root.mode); restoreMode: Binding.RestoreNone }
            Connections { target: Daemon; function onError() { modeBox.currentIndex = modeBox.indexOfValue(root.mode); } }
        }
    }

    RowLayout {
        enabled: root.asset !== ""
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label { text: qsTr("Volume:") }
        QQC2.Slider {
            id: volumeSlider
            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
            from: 0
            to: 100
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            onMoved: Daemon.queuePetUpdate(root.petId, {audio: {volume: value}})
            Binding {
                target: volumeSlider; property: "value"; value: root.sound.volume ?? 75
                when: root.settled && !volumeSlider.pressed; restoreMode: Binding.RestoreNone
            }
        }
        QQC2.Label {
            Layout.minimumWidth: Kirigami.Units.gridUnit * 2.5
            text: qsTr("%1%").arg(Math.round(volumeSlider.value))
        }
    }

    RowLayout {
        enabled: root.asset !== ""
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label { text: qsTr("Delay:") }
        QQC2.SpinBox {
            id: offsetBox
            from: -5000
            to: 5000
            stepSize: 50
            editable: true
            textFromValue: (value, locale) => qsTr("%1 ms").arg(value)
            valueFromText: (text, locale) => parseInt(text) || 0
            onValueModified: Daemon.queuePetUpdate(root.petId, {audio: {offsetMs: value}})
            Binding {
                target: offsetBox; property: "value"; value: root.sound.offsetMs ?? 0
                when: root.settled && !offsetBox.activeFocus; restoreMode: Binding.RestoreNone
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 14
            wrapMode: Text.Wrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            text: qsTr("Negative starts the sound that far into the file.")
        }
    }

    RowLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Button {
            icon.name: "media-playback-start"
            text: qsTr("Test")
            enabled: root.asset !== "" && Overlay.available
            QQC2.ToolTip.visible: hovered && !Overlay.available
            QQC2.ToolTip.text: qsTr("The desktop pets are not running. Show the pet on the desktop first.")
            onClicked: Overlay.playPetSound(root.petId)
        }
        QQC2.Button {
            icon.name: "media-playback-stop"
            text: qsTr("Stop")
            enabled: Overlay.available
            onClicked: Overlay.stopPetSounds()
        }
    }

    // Click sounds need a pet that takes clicks.
    RowLayout {
        visible: root.mode === "click" && (root.pet?.clickThrough ?? true)
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: root.degradedText("audio: click needs a clickable pet")
        }
        QQC2.Button {
            text: qsTr("Turn it off")
            onClicked: Daemon.updatePet(root.petId, {clickThrough: false})
        }
    }

    // What the overlay says it cannot do (the click warning above is shown by its own row).
    Repeater {
        model: root.degraded.filter(n => n !== "audio: click needs a clickable pet")
        QQC2.Label {
            required property string modelData
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            opacity: 0.8
            textFormat: Text.PlainText // (the default of degradedText is the overlay's own text)
            text: root.degradedText(modelData)
        }
    }

    QQC2.Label {
        visible: !root.supported
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        wrapMode: Text.Wrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: qsTr("The running daemon is too old for sounds; restart it after updating.")
    }
}
