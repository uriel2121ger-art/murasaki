import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

// Makes a desktop pet from a sprite sheet, a set of images (files or a folder), a ZIP of frames, a GIF (converted to APNG) or an .mdaa package
// (docs/pets/import.md). The form builds the job description, asks the daemon for a preview of what it would make (headers only), runs the job and
// shows its progress; the new pet's page opens when it is done. Nothing is decoded here.
KirigamiDialogs.Dialog {
    id: dialog

    // sheet | images | zip | gif | mdaa | video
    property string kind: "sheet"
    property var sources: []
    property string jobId: ""
    property var preview: null
    property string failure: ""
    // Values to start the form from (an item's importSpec, "Edit import"); empty for the settings' defaults.
    property var startFrom: ({})

    readonly property var job: Daemon.imports.find(j => j.id === jobId) ?? null
    readonly property bool running: jobId !== "" && (job === null || job.state === "queued" || job.state === "running")
    readonly property var prefs: Daemon.settings
    readonly property bool hasSources: sources.length > 0
    readonly property bool webpWanted: formatBox.currentValue === "webp"
    readonly property bool timed: kind === "images" || kind === "zip"
    readonly property bool keyable: kind !== "mdaa"

    title: ({sheet: qsTr("Import a sprite sheet"), images: qsTr("Import images"), zip: qsTr("Import a ZIP of frames"),
             gif: qsTr("Convert a GIF to APNG"), mdaa: qsTr("Import a package (.mdaa)"), video: qsTr("Import a video")})[kind] ?? qsTr("Import a pet")
    standardButtons: KirigamiDialogs.Dialog.NoButton
    preferredWidth: Kirigami.Units.gridUnit * 30
    padding: Kirigami.Units.largeSpacing

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    function baseName(path) {
        const leaf = String(path).split("/").pop();
        const dot = leaf.lastIndexOf(".");
        return dot > 0 ? leaf.substring(0, dot) : leaf;
    }

    // Opens the form for a kind, asking for the source first.
    function openFor(newKind, from, source) {
        kind = newKind;
        startFrom = from ?? ({});
        sources = source ? [source] : [];
        jobId = "";
        preview = null;
        failure = "";
        open();
    }

    function buildSpec() {
        const spec = {kind: kind, sources: sources, name: nameField.text.trim() !== "" ? nameField.text.trim() : (hasSources ? baseName(sources[0]) : "")};
        if (kind === "sheet") {
            spec.frameWidth = widthBox.value;
            spec.frameHeight = heightBox.value;
            spec.frameCount = countBox.value;
            spec.startFrame = startBox.value;
            spec.paddingLeft = padLeftBox.value;
            spec.paddingTop = padTopBox.value;
            spec.spacingH = spaceHBox.value;
            spec.spacingV = spaceVBox.value;
            spec.frameMs = msBox.value;
        } else if (timed) {
            spec.fps = fpsBox.value;
            spec.sizePolicy = sizeBox.currentValue;
        }
        if (kind === "video") {
            spec.maxSide = sideBox.value;
            spec.maxFps = videoFpsBox.value;
            spec.startMs = trimStartBox.value * 1000;
            spec.durationMs = trimLengthBox.value * 1000;
            if (audioSwitch.checked)
                spec.keepAudio = true;
        }
        if (kind === "mdaa") {
            if (sceneSwitch.checked)
                spec.autoScene = sceneBox.value;
            return spec;
        }
        spec.format = formatBox.currentValue;
        if (webpWanted)
            spec.webpQuality = qualityBox.value;
        if (keySwitch.checked)
            spec.colorKey = {color: keyField.text.replace("#", ""), tolerance: toleranceBox.value, softness: softnessBox.value};
        if (keepSwitch.checked)
            spec.keepSource = true;
        return spec;
    }

    readonly property var spec: buildSpec()
    onSpecChanged: if (opened && hasSources && jobId === "") previewTimer.restart()

    Timer {
        id: previewTimer
        interval: 400
        onTriggered: Daemon.previewImport(dialog.buildSpec())
    }

    // Sets the controls from the settings' defaults, then from `startFrom` (an earlier import of this pet).
    function loadDefaults() {
        const s = startFrom;
        nameField.text = s.name ?? "";
        formatBox.currentIndex = Math.max(0, formatBox.indexOfValue(s.format ?? prefs.importFormat ?? "apng"));
        qualityBox.value = s.webpQuality ?? 100;
        sizeBox.currentIndex = Math.max(0, sizeBox.indexOfValue(s.sizePolicy ?? prefs.importSizePolicy ?? "fit"));
        fpsBox.value = s.fps ?? prefs.importFps ?? 24;
        widthBox.value = s.frameWidth ?? 64;
        heightBox.value = s.frameHeight ?? 64;
        countBox.value = s.frameCount ?? 4;
        startBox.value = s.startFrame ?? 1;
        padLeftBox.value = s.paddingLeft ?? 0;
        padTopBox.value = s.paddingTop ?? 0;
        spaceHBox.value = s.spacingH ?? 0;
        spaceVBox.value = s.spacingV ?? 0;
        msBox.value = s.frameMs ?? 100;
        keySwitch.checked = s.colorKey !== undefined;
        keyField.text = s.colorKey?.color ?? "00ff00";
        toleranceBox.value = s.colorKey?.tolerance ?? 50;
        softnessBox.value = s.colorKey?.softness ?? 10;
        keepSwitch.checked = s.keepSource ?? prefs.importKeepSource ?? false;
        sideBox.value = s.maxSide ?? prefs.importVideoMaxSide ?? 1024;
        videoFpsBox.value = s.maxFps ?? prefs.importVideoMaxFps ?? 30;
        trimStartBox.value = Math.round((s.startMs ?? 0) / 1000);
        trimLengthBox.value = Math.round((s.durationMs ?? 0) / 1000);
        audioSwitch.checked = s.keepAudio ?? prefs.importKeepAudio ?? false;
        sceneSwitch.checked = (s.autoScene ?? -1) >= 0;
        sceneBox.value = Math.max(0, s.autoScene ?? 0);
    }

    function chooseSources() {
        if (kind === "images")
            imageFiles.open();
        else
            oneFile.open();
    }

    onOpened: {
        loadDefaults();
        if (hasSources)
            nameField.placeholderText = baseName(sources[0]);
        else
            chooseSources();
    }
    onClosed: {
        previewTimer.stop();
        if (running)
            Daemon.cancelImport(jobId);
    }

    FileDialog {
        id: oneFile
        title: qsTr("Choose the file")
        nameFilters: ({
            sheet: [qsTr("Images (*.png *.webp *.jpg *.jpeg *.gif)"), qsTr("All files (*)")],
            zip: [qsTr("ZIP archives (*.zip)"), qsTr("All files (*)")],
            gif: [qsTr("GIF animations (*.gif)"), qsTr("All files (*)")],
            mdaa: [qsTr("Packages (*.mdaa *.zip)"), qsTr("All files (*)")],
            video: [qsTr("Videos (*.mp4 *.m4v *.webm *.mkv *.mov *.avi)"), qsTr("All files (*)")]
        })[dialog.kind] ?? [qsTr("All files (*)")]
        onAccepted: {
            dialog.sources = [SystemControl.localFile(selectedFile)];
            if (nameField.text === "")
                nameField.placeholderText = dialog.baseName(dialog.sources[0]);
        }
    }
    FileDialog {
        id: imageFiles
        title: qsTr("Choose the images")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Images (*.png *.webp *.jpg *.jpeg)"), qsTr("All files (*)")]
        onAccepted: {
            dialog.sources = selectedFiles.map(f => SystemControl.localFile(f));
            if (nameField.text === "" && dialog.sources.length > 0)
                nameField.placeholderText = dialog.baseName(dialog.sources[0]);
        }
    }
    FolderDialog {
        id: imageFolder
        title: qsTr("Choose the folder of images")
        onAccepted: {
            dialog.sources = [SystemControl.localFile(selectedFolder)];
            if (nameField.text === "")
                nameField.placeholderText = dialog.baseName(dialog.sources[0]);
        }
    }

    Connections {
        target: Daemon
        function onImportStarted(id) {
            if (dialog.running || dialog.jobId === "")
                dialog.jobId = id;
        }
        function onImportPreviewed(answer) {
            if (dialog.jobId === "")
                dialog.preview = answer;
        }
        function onError(message) {
            if (dialog.jobId === "pending") { // ImportPet was refused
                dialog.failure = message;
                dialog.jobId = "";
            }
        }
        function onImportsChanged() {
            const j = dialog.job;
            if (!j || !dialog.opened)
                return;
            if (j.state === "done" && (j.itemId ?? "") !== "") {
                const id = j.itemId;
                dialog.jobId = "";
                dialog.close();
                applicationWindow().openItem(id);
            } else if (j.state === "failed" || j.state === "cancelled") {
                dialog.failure = j.error ?? "";
                dialog.jobId = "";
            }
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // The source.
        RowLayout {
            Layout.fillWidth: true
            enabled: !dialog.running
            QQC2.Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: !dialog.hasSources ? qsTr("Nothing chosen yet")
                                         : dialog.sources.length === 1 ? dialog.plain(dialog.sources[0].split("/").pop())
                                                                       : qsTr("%n file(s)", "", dialog.sources.length)
            }
            QQC2.Button {
                text: qsTr("Choose…")
                onClicked: dialog.chooseSources()
            }
            QQC2.Button {
                visible: dialog.kind === "images"
                text: qsTr("Choose a folder…")
                onClicked: imageFolder.open()
            }
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            enabled: !dialog.running

            QQC2.TextField {
                id: nameField
                Kirigami.FormData.label: qsTr("Name:")
                maximumLength: 100
            }

            // Sprite sheet geometry (Anima's defaults: 64 × 64 frames, 4 of them, from the first, no margins).
            RowLayout {
                visible: dialog.kind === "sheet"
                Kirigami.FormData.label: qsTr("Frame size:")
                QQC2.SpinBox { id: widthBox; from: 1; to: 8192; editable: true }
                QQC2.Label { text: "×" }
                QQC2.SpinBox { id: heightBox; from: 1; to: 8192; editable: true }
            }
            RowLayout {
                visible: dialog.kind === "sheet"
                Kirigami.FormData.label: qsTr("Frames:")
                QQC2.SpinBox { id: countBox; from: 1; to: 20000; editable: true }
                QQC2.Label { text: qsTr("starting at") }
                QQC2.SpinBox { id: startBox; from: 1; to: 80000; editable: true }
            }
            RowLayout {
                visible: dialog.kind === "sheet"
                Kirigami.FormData.label: qsTr("Margin (left, top):")
                QQC2.SpinBox { id: padLeftBox; from: 0; to: 16384; editable: true }
                QQC2.SpinBox { id: padTopBox; from: 0; to: 16384; editable: true }
            }
            RowLayout {
                visible: dialog.kind === "sheet"
                Kirigami.FormData.label: qsTr("Gap (across, down):")
                QQC2.SpinBox { id: spaceHBox; from: 0; to: 16384; editable: true }
                QQC2.SpinBox { id: spaceVBox; from: 0; to: 16384; editable: true }
            }
            RowLayout {
                visible: dialog.kind === "sheet"
                Kirigami.FormData.label: qsTr("Each frame lasts:")
                QQC2.SpinBox { id: msBox; from: 1; to: 60000; editable: true }
                QQC2.Label { text: qsTr("ms") }
            }

            // Images and ZIPs have no timing of their own.
            RowLayout {
                visible: dialog.timed
                Kirigami.FormData.label: qsTr("Speed:")
                QQC2.SpinBox { id: fpsBox; from: 1; to: 120; editable: true }
                QQC2.Label { text: qsTr("frames per second") }
            }
            QQC2.ComboBox {
                id: sizeBox
                visible: dialog.timed
                Kirigami.FormData.label: qsTr("Images of other sizes:")
                textRole: "text"
                valueRole: "value"
                model: [
                    {text: qsTr("Centre them without stretching"), value: "fit"},
                    {text: qsTr("Scale them to the first image"), value: "scale"},
                    {text: qsTr("Refuse the set"), value: "first"}
                ]
            }

            // A video: the picture is scaled down and thinned out to what a pet needs; the sound is not played.
            RowLayout {
                visible: dialog.kind === "video"
                Kirigami.FormData.label: qsTr("Longest side:")
                QQC2.SpinBox { id: sideBox; from: 16; to: 8192; editable: true }
                QQC2.Label { text: qsTr("px") }
            }
            RowLayout {
                visible: dialog.kind === "video"
                Kirigami.FormData.label: qsTr("At most:")
                QQC2.SpinBox { id: videoFpsBox; from: 1; to: 60; editable: true }
                QQC2.Label { text: qsTr("frames per second") }
            }
            RowLayout {
                visible: dialog.kind === "video"
                Kirigami.FormData.label: qsTr("Start at:")
                QQC2.SpinBox { id: trimStartBox; from: 0; to: 86400; editable: true }
                QQC2.Label { text: qsTr("s, keep") }
                QQC2.SpinBox { id: trimLengthBox; from: 0; to: 86400; editable: true }
                QQC2.Label { text: qsTr("s (0: to the end)") }
            }
            QQC2.CheckBox {
                id: audioSwitch
                visible: dialog.kind === "video"
                Kirigami.FormData.label: qsTr("Sound:")
                text: qsTr("Keep the sound (you can give it to the pet afterwards)")
            }

            // A package starts in no scene unless the user says so.
            RowLayout {
                visible: dialog.kind === "mdaa"
                Kirigami.FormData.label: qsTr("Scene:")
                QQC2.CheckBox { id: sceneSwitch; text: qsTr("Start a new pet in scene") }
                QQC2.SpinBox { id: sceneBox; from: 0; to: 255; enabled: sceneSwitch.checked }
            }

            QQC2.ComboBox {
                id: formatBox
                visible: dialog.keyable
                Kirigami.FormData.label: qsTr("Save as:")
                textRole: "text"
                valueRole: "value"
                model: [{text: qsTr("APNG (lossless)"), value: "apng"}, {text: qsTr("Animated WebP (smaller)"), value: "webp"}]
            }
            RowLayout {
                visible: dialog.keyable && dialog.webpWanted
                Kirigami.FormData.label: qsTr("WebP quality:")
                QQC2.SpinBox { id: qualityBox; from: 1; to: 100; editable: true }
                QQC2.Label { text: qsTr("(100 is lossless)") }
            }

            QQC2.CheckBox {
                id: keySwitch
                visible: dialog.keyable
                Kirigami.FormData.label: qsTr("Background:")
                text: qsTr("Make one colour transparent")
            }
            RowLayout {
                visible: dialog.keyable && keySwitch.checked
                Kirigami.FormData.label: qsTr("Colour (RRGGBB):")
                QQC2.TextField {
                    id: keyField
                    inputMask: ">HHHHHH"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 6
                }
                Rectangle {
                    implicitWidth: Kirigami.Units.gridUnit
                    implicitHeight: Kirigami.Units.gridUnit
                    color: "#" + keyField.text
                    border.color: Kirigami.Theme.textColor
                }
            }
            RowLayout {
                visible: dialog.keyable && keySwitch.checked
                Kirigami.FormData.label: qsTr("Tolerance, softness:")
                QQC2.SpinBox { id: toleranceBox; from: 0; to: 100; editable: true }
                QQC2.SpinBox { id: softnessBox; from: 0; to: 100; editable: true }
                QQC2.Label { text: "%" }
            }
            QQC2.CheckBox {
                id: keepSwitch
                visible: dialog.keyable
                Kirigami.FormData.label: qsTr("Original:")
                text: qsTr("Keep a copy of the source files")
            }
        }

        // What the daemon says it would make, or why it will not.
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: !dialog.running && dialog.hasSources && dialog.failure === ""
            color: dialog.preview && !dialog.preview.ok ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
            text: {
                if (!dialog.preview)
                    return qsTr("Looking at the source…");
                if (!dialog.preview.ok)
                    return dialog.plain(dialog.preview.error);
                const p = dialog.preview.preview;
                const size = qsTr("%1 × %2 px").arg(p.width).arg(p.height);
                const mb = qsTr("up to %1 MB").arg(Math.max(0.1, p.estimatedBytes / 1048576).toFixed(1));
                const time = p.totalDelayMs >= 0 ? qsTr("%1 s").arg((p.totalDelayMs / 1000).toFixed(2)) : "";
                const parts = dialog.kind === "mdaa" ? [qsTr("%n animation(s)", "", p.frames)]
                                                      : [p.frames > 0 ? qsTr("%n frame(s)", "", p.frames) : qsTr("length not known yet"), size, time, p.frames > 0 ? mb : ""];
                return parts.filter(t => t !== "").join(" · ") + (p.degraded.length > 0 ? "\n" + p.degraded.map(d => dialog.plain(d)).join("\n") : "");
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            visible: dialog.failure !== ""
            color: Kirigami.Theme.negativeTextColor
            text: qsTr("The import did not finish: %1").arg(dialog.plain(dialog.failure))
        }

        // Progress of the running job.
        ColumnLayout {
            visible: dialog.running
            Layout.fillWidth: true
            QQC2.ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: dialog.job ? Math.max(0, dialog.job.progress) : 0
                indeterminate: dialog.job === null || dialog.job.state === "queued" || dialog.job.progress < 0
            }
            QQC2.Label {
                text: dialog.job && dialog.job.state === "queued" ? qsTr("Waiting for another import…") : qsTr("Importing…")
                opacity: 0.7
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            QQC2.Button {
                text: dialog.running ? qsTr("Cancel import") : qsTr("Close")
                onClicked: {
                    if (dialog.running)
                        Daemon.cancelImport(dialog.jobId);
                    else
                        dialog.close();
                }
            }
            QQC2.Button {
                visible: !dialog.running
                highlighted: true
                icon.name: "document-import"
                text: qsTr("Import")
                enabled: dialog.hasSources && Daemon.importsSupported && (dialog.preview === null || dialog.preview.ok)
                onClicked: {
                    dialog.failure = "";
                    Daemon.importPet(dialog.buildSpec());
                    dialog.jobId = "pending";
                }
            }
        }
    }
}
