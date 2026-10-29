import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, Performance: frame rate, scene engine and render quality.
Kirigami.FormLayout {
    id: page

    // What the daemon reports, with its defaults while it has not answered.
    readonly property int webFpsLimit: Daemon.settings.webFpsLimit ?? SystemControl.defaultWebFpsLimit
    readonly property real sceneRenderScale: Daemon.settings.sceneRenderScale ?? 0

    // Puts the controls back to the daemon's values (after SettingsChanged, or a rejected change).
    function sync() {
        if (!fpsSlider.pressed && !fpsDebounce.running)
            fpsSlider.value = page.webFpsLimit;
        qualityBox.currentIndex = qualityBox.indexOfScale(page.sceneRenderScale);
    }

    Component.onCompleted: sync()

    Connections {
        target: Daemon
        function onSettingsChanged() { page.sync(); }
        function onError() { page.sync(); }
    }

    // Sends the slider's value once it has stopped moving (mouse, keyboard and wheel alike).
    Timer {
        id: fpsDebounce
        interval: 300
        onTriggered: Daemon.setSettings({webFpsLimit: Math.round(fpsSlider.value)})
    }

    // Leaving the page with a change still waiting out its debounce must not lose it.
    Component.onDestruction: {
        if (fpsDebounce.running) {
            fpsDebounce.stop();
            fpsDebounce.triggered();
        }
    }

    RowLayout {
        Kirigami.FormData.label: qsTr("Frame rate limit:")
        enabled: Daemon.available
        spacing: Kirigami.Units.largeSpacing

        QQC2.Slider {
            id: fpsSlider
            Layout.preferredWidth: Kirigami.Units.gridUnit * 14
            from: SystemControl.minWebFpsLimit
            to: SystemControl.maxWebFpsLimit
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: page.webFpsLimit
            // Applied once the value settles, not on every step of a drag.
            onMoved: fpsDebounce.restart()
            onPressedChanged: {
                if (!pressed && fpsDebounce.running) {
                    fpsDebounce.stop();
                    fpsDebounce.triggered();
                }
            }
        }
        QQC2.Label {
            Layout.minimumWidth: Kirigami.Units.gridUnit * 4
            text: qsTr("%1 fps").arg(Math.round(fpsSlider.value))
        }
    }

    Hint {
        text: qsTr("Applies to web wallpapers and scenes, and never exceeds the screen's refresh rate. Videos play at their own rate.")
    }


    QQC2.ComboBox {
        id: qualityBox
        Kirigami.FormData.label: qsTr("Render quality:")
        enabled: Daemon.available
        textRole: "text"
        valueRole: "value"
        model: [
            {text: qsTr("Automatic (adapts to the graphics card, never below 50 %)"), value: 0},
            {text: qsTr("High (full resolution)"), value: 1.0},
            {text: qsTr("Medium (75 %)"), value: 0.75},
            {text: qsTr("Low (50 %, lightest on the graphics card)"), value: 0.5}
        ]
        // The nearest of the three choices to what the daemon holds (a hand-edited settings file may hold another).
        function indexOfScale(scale) {
            if (scale <= 0)
                return 0;
            let best = 1;
            for (let i = 2; i < model.length; ++i) {
                if (Math.abs(model[i].value - scale) < Math.abs(model[best].value - scale))
                    best = i;
            }
            return best;
        }
        currentIndex: indexOfScale(page.sceneRenderScale)
        onActivated: Daemon.setSettings({sceneRenderScale: currentValue})
    }

    Hint {
        text: qsTr("The resolution scenes are drawn at. Automatic lowers it only for scenes that would take more than a quarter of the graphics card and raises it again when there is room; the other choices keep it fixed. Lower values make the picture a little softer; scenes on screen restart when you change it.")
    }
}
