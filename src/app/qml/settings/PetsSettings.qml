import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, Pets: their speed, sound, size over fullscreen windows, puppets and the arrange keys.
Kirigami.FormLayout {
    id: page

    // What the daemon reports, with its defaults while it has not answered.
    readonly property int petMaxFps: Daemon.settings.petMaxFps ?? 0
    readonly property bool petSaveEnergy: Daemon.settings.petSaveEnergy ?? true
    readonly property int petsVolume: Daemon.settings.petsVolume ?? 100
    readonly property bool petsMuted: Daemon.settings.petsMuted ?? false
    readonly property bool petsMuteWhenFullscreen: Daemon.settings.petsMuteWhenFullscreen ?? true
    readonly property bool stageHideOverFullscreen: Daemon.settings.stageHideOverFullscreen ?? true
    readonly property real stageMaxPixels: Daemon.settings.stageMaxPixels ?? 8294400
    readonly property real puppetMaxPixels: Daemon.settings.puppetMaxPixels ?? 4194304
    readonly property string puppetCorePath: Daemon.settings.puppetCorePath ?? ""

    // Puts the controls back to the daemon's values (after SettingsChanged, or a rejected change).
    function sync() {
        petFpsBox.currentIndex = petFpsBox.indexOfValue(page.petMaxFps);
        petEnergySwitch.checked = page.petSaveEnergy;
        if (!petsVolumeSlider.pressed && !petsVolumeDebounce.running)
            petsVolumeSlider.value = page.petsVolume;
        petsMuteSwitch.checked = page.petsMuted;
        petsFullscreenSwitch.checked = page.petsMuteWhenFullscreen;
        stageHideSwitch.checked = page.stageHideOverFullscreen;
        stagePixelsBox.currentIndex = stagePixelsBox.indexOfPixels(page.stageMaxPixels);
        puppetPixelsBox.currentIndex = puppetPixelsBox.indexOfPixels(page.puppetMaxPixels);
        if (!puppetCoreField.activeFocus)
            puppetCoreField.text = page.puppetCorePath;
    }

    Connections {
        target: Daemon
        function onSettingsChanged() { page.sync(); }
        function onError() { page.sync(); }
    }

    // How the arrange shortcut of the pets is bound, from the overlay's ShortcutBackend property.
    readonly property string shortcutText: {
        if (!Daemon.petsAvailable)
            return qsTr("Desktop pets need a newer Murasaki service.");
        if (!Overlay.available)
            return qsTr("Starts with the first pet: %1").arg(Overlay.defaultShortcut);
        if (Overlay.shortcutState === "starting")
            return qsTr("Registering…");
        // the key as the compositor bound it (the user may have changed it), else the one asked for
        const bound = Overlay.shortcutTriggers["toggle-arrange"] ?? "";
        const key = bound !== "" ? bound : Overlay.defaultShortcut;
        switch (Overlay.shortcutBackend) {
        case "kglobalaccel": return qsTr("%1 (KDE shortcut)").arg(key);
        case "portal": return qsTr("%1 (desktop shortcut)").arg(key);
        default: return qsTr("Unavailable: use the Arrange pets toggle");
        }
    }

    // The key of one of the other global actions ("toggle-hidden", "rescue"), or why it has none.
    function actionShortcutText(action) {
        if (!Daemon.petsAvailable || !Daemon.petBehavioursAvailable)
            return qsTr("Needs a newer Murasaki service.");
        if (!Overlay.available)
            return qsTr("Starts with the first pet");
        if (Overlay.shortcutState === "starting")
            return qsTr("Registering…");
        if (Overlay.shortcutState === "failed" || Overlay.shortcutState === "off")
            return qsTr("Unavailable: use murasakictl pets ...");
        const key = Overlay.shortcutTriggers[action] ?? "";
        if (key !== "")
            return key;
        return Overlay.shortcutBackend === "kglobalaccel"
            ? qsTr("No key: the usual one is taken by another application. Set one in System Settings, Shortcuts, Murasaki")
            : qsTr("No key: the usual one is taken by another application. Set one in the desktop's shortcut settings");
    }

    Component.onCompleted: {
        Overlay.refresh();
        sync();
    }

    Timer {
        id: petsVolumeDebounce
        interval: 300
        onTriggered: Daemon.setSettings({petsVolume: Math.round(petsVolumeSlider.value)})
    }

    // Leaving the page with a change still waiting out its debounce must not lose it.
    Component.onDestruction: {
        if (petsVolumeDebounce.running) {
            petsVolumeDebounce.stop();
            petsVolumeDebounce.triggered();
        }
    }

    QQC2.ComboBox {
        id: petFpsBox
        Kirigami.FormData.label: qsTr("Frame rate:")
        enabled: Daemon.available && Daemon.apiVersion >= 11
        QQC2.ToolTip.visible: hovered && !enabled
        QQC2.ToolTip.text: qsTr("The running daemon is too old for this; restart it after updating.")
        textRole: "text"
        valueRole: "value"
        model: [
            {text: qsTr("No limit"), value: 0},
            {text: qsTr("At most %1 frames per second").arg(24), value: 24},
            {text: qsTr("At most %1 frames per second").arg(30), value: 30},
            {text: qsTr("At most %1 frames per second").arg(60), value: 60},
            {text: qsTr("At most %1 frames per second").arg(120), value: 120}
        ]
        // A limit set from outside the list (45, from a script) is shown as it is, not as "No limit".
        currentIndex: indexOfValue(page.petMaxFps)
        displayText: currentIndex >= 0 ? currentText : qsTr("At most %1 frames per second").arg(page.petMaxFps)
        onActivated: Daemon.setSettings({petMaxFps: currentValue})
    }

    QQC2.Switch {
        id: petEnergySwitch
        enabled: Daemon.available && Daemon.apiVersion >= 11
        text: qsTr("On battery or in the power-saver profile, limit them to 15 frames per second")
        checked: page.petSaveEnergy
        onToggled: Daemon.setSettings({petSaveEnergy: checked})
    }

    Hint {
        text: qsTr("Frames above the limit are skipped; the animation keeps its speed. The pets also lower the limit by themselves when the computer cannot keep up.")
    }

    Kirigami.Separator {
        Kirigami.FormData.isSection: true
        Kirigami.FormData.label: qsTr("Sounds")
    }

    RowLayout {
        Kirigami.FormData.label: qsTr("Volume:")
        spacing: Kirigami.Units.largeSpacing
        enabled: Daemon.petAudioSupported
        QQC2.ToolTip.visible: hovered && !enabled
        QQC2.ToolTip.text: qsTr("The running daemon is too old for this; restart it after updating.")

        QQC2.Slider {
            id: petsVolumeSlider
            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
            from: 0
            to: 100
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: page.petsVolume
            onMoved: petsVolumeDebounce.restart()
            onPressedChanged: {
                if (!pressed && petsVolumeDebounce.running) {
                    petsVolumeDebounce.stop();
                    petsVolumeDebounce.triggered();
                }
            }
        }
        QQC2.Label {
            Layout.minimumWidth: Kirigami.Units.gridUnit * 3
            text: qsTr("%1%").arg(Math.round(petsVolumeSlider.value))
        }
    }

    QQC2.Switch {
        id: petsMuteSwitch
        enabled: Daemon.petAudioSupported
        text: qsTr("Mute the pets")
        checked: page.petsMuted
        onToggled: Daemon.setSettings({petsMuted: checked})
    }

    QQC2.Switch {
        id: petsFullscreenSwitch
        enabled: Daemon.petAudioSupported
        text: qsTr("Mute them while a window is fullscreen or a listed application is open")
        checked: page.petsMuteWhenFullscreen
        onToggled: Daemon.setSettings({petsMuteWhenFullscreen: checked})
    }

    Hint {
        text: qsTr("A pet makes no sound until you give it one on its page. This volume applies to all of them, on top of each pet's own.")
    }

    Kirigami.Separator {
        Kirigami.FormData.isSection: true
        Kirigami.FormData.label: qsTr("Fullscreen")
    }

    QQC2.Switch {
        id: stageHideSwitch
        Kirigami.FormData.label: qsTr("Hide:")
        enabled: Daemon.petStageSupported
        text: qsTr("Hide them on a screen while a window is fullscreen there")
        checked: page.stageHideOverFullscreen
        onToggled: Daemon.setSettings({stageHideOverFullscreen: checked})
        QQC2.ToolTip.visible: hovered && !enabled
        QQC2.ToolTip.text: qsTr("The running daemon is too old for this; restart it after updating.")
    }

    QQC2.ComboBox {
        id: stagePixelsBox
        Kirigami.FormData.label: qsTr("Size:")
        enabled: Daemon.petStageSupported
        textRole: "text"
        valueRole: "value"
        readonly property var presets: [
            {text: qsTr("Up to 1 megapixel (light)"), value: 1000000},
            {text: qsTr("Up to Full HD (1920 x 1080)"), value: 2073600},
            {text: qsTr("Up to 2560 x 1440"), value: 3686400},
            {text: qsTr("Up to 4K (3840 x 2160)"), value: 8294400},
            {text: qsTr("Up to 8K (7680 x 4320)"), value: 33177600}
        ]
        model: presets.concat(presets.some(p => p.value === page.stageMaxPixels) ? [] : [{text: qsTr("%1 pixels (set by hand)").arg(Math.round(page.stageMaxPixels)), value: page.stageMaxPixels}])
        function indexOfPixels(pixels) {
            const i = presets.findIndex(p => p.value === pixels);
            return i >= 0 ? i : presets.length;
        }
        onActivated: Daemon.setSettings({stageMaxPixels: currentValue})
        currentIndex: indexOfPixels(page.stageMaxPixels)
    }

    Hint {
        text: qsTr("A pet shown across the screen costs what a big animation costs. Above this size, all of them are drawn from smaller copies of the animation, enlarged by the graphics card (an animation that is small to begin with is not reduced).")
    }

    Kirigami.Separator {
        Kirigami.FormData.isSection: true
        Kirigami.FormData.label: qsTr("Puppets")
    }

    QQC2.ComboBox {
        id: puppetPixelsBox
        Kirigami.FormData.label: qsTr("Picture size:")
        enabled: Daemon.puppetsSupported
        textRole: "text"
        valueRole: "value"
        readonly property var presets: [
            {text: qsTr("Up to 1 megapixel (light)"), value: 1000000},
            {text: qsTr("Up to 2 megapixels"), value: 2097152},
            {text: qsTr("Up to 4 megapixels (default)"), value: 4194304},
            {text: qsTr("Up to 8 megapixels"), value: 8388608},
            {text: qsTr("Up to 16 megapixels"), value: 16000000}
        ]
        model: presets.concat(presets.some(p => p.value === page.puppetMaxPixels) ? [] : [{text: qsTr("%1 pixels (set by hand)").arg(Math.round(page.puppetMaxPixels)), value: page.puppetMaxPixels}])
        function indexOfPixels(pixels) {
            const i = presets.findIndex(p => p.value === pixels);
            return i >= 0 ? i : presets.length;
        }
        onActivated: Daemon.setSettings({puppetMaxPixels: currentValue})
        currentIndex: indexOfPixels(page.puppetMaxPixels)
        QQC2.ToolTip.visible: hovered && !enabled
        QQC2.ToolTip.text: qsTr("The running daemon is too old for this; restart it after updating.")
    }

    Hint {
        text: qsTr("A puppet is drawn live. Above this size its picture is drawn smaller and enlarged by the graphics card; a puppet that is drawn smaller says so on its page.")
    }

    QQC2.TextField {
        id: puppetCoreField
        Kirigami.FormData.label: qsTr("Cubism runtime:")
        Layout.preferredWidth: Kirigami.Units.gridUnit * 24
        enabled: Daemon.puppetsSupported
        placeholderText: qsTr("Path to a file you downloaded yourself")
        onEditingFinished: {
            if (text.trim() === page.puppetCorePath)
                return;
            Daemon.setSettings({puppetCorePath: text.trim()});
            // a rejected path must not stay in the field as if it were stored
            text = Qt.binding(() => page.puppetCorePath);
        }
    }

    Hint {
        text: qsTr("Murasaki does not ship the Cubism runtime. This path is only stored and checked (it must be a plain file you own, not writable by others); nothing loads it yet, and Cubism models are not drawn.")
    }

    Kirigami.Separator {
        Kirigami.FormData.isSection: true
        Kirigami.FormData.label: qsTr("Arrange")
    }

    QQC2.Switch {
        id: arrangeSwitch
        Kirigami.FormData.label: qsTr("Arrange:")
        text: qsTr("Arrange pets")
        enabled: Daemon.petsAvailable
        checked: Daemon.petsArrange
        onToggled: Daemon.setPetsArrange(checked)
        Connections {
            target: Daemon
            function onPetsArrangeChanged() { arrangeSwitch.checked = Daemon.petsArrange; }
            function onError() { arrangeSwitch.checked = Daemon.petsArrange; }
        }
    }

    QQC2.Switch {
        id: hideAllSwitch
        Kirigami.FormData.label: qsTr("Hide pets:")
        text: qsTr("Hide every pet (not remembered after a restart)")
        enabled: Daemon.petsAvailable && Daemon.petBehavioursAvailable
        checked: Daemon.petsHidden
        onToggled: Daemon.setPetsHidden(checked)
        Connections {
            target: Daemon
            function onPetsHiddenChanged() { hideAllSwitch.checked = Daemon.petsHidden; }
            function onError() { hideAllSwitch.checked = Daemon.petsHidden; }
        }
    }

    QQC2.Button {
        Kirigami.FormData.label: qsTr("Rescue:")
        enabled: Daemon.petsAvailable && Daemon.petBehavioursAvailable
        icon.name: "edit-undo"
        text: qsTr("Rescue the pets")
        onClicked: Daemon.rescuePets()
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.text: qsTr("Shows every pet and makes it solid and grabbable again: click-through off, keep above on, hover hide, fade, dodge and lock off, opacity 1 when below 50%.")
    }

    QQC2.Label {
        Kirigami.FormData.label: qsTr("Shortcut:")
        textFormat: Text.PlainText
        text: page.shortcutText
    }

    QQC2.Label {
        Kirigami.FormData.label: qsTr("Hide key:")
        textFormat: Text.PlainText
        text: page.actionShortcutText("toggle-hidden")
    }

    QQC2.Label {
        Kirigami.FormData.label: qsTr("Rescue key:")
        textFormat: Text.PlainText
        text: page.actionShortcutText("rescue")
    }

    Hint {
        text: qsTr("Rescue makes every pet visible, solid and grabbable again. If a pet cannot be reached with the pointer, the shortcut or murasakictl pets rescue is the way out. The keys are set in the desktop's own shortcut settings; one that another application holds stays unbound.")
    }

    Hint {
        text: qsTr("While arranging, drag a pet to move it, scroll to resize it and use its close button to remove it. Press the shortcut again to finish.")
    }
}
