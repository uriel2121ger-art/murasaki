import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App

// Settings, Import: how pets made from files are stored.
Kirigami.FormLayout {
    id: page

    QQC2.ComboBox {
        id: importFormatBox
        Kirigami.FormData.label: qsTr("Save imports as:")
        enabled: Daemon.available && Daemon.importsSupported
        textRole: "text"
        valueRole: "value"
        model: [{text: qsTr("APNG (lossless)"), value: "apng"}, {text: qsTr("Animated WebP (smaller)"), value: "webp"}]
        currentIndex: Math.max(0, indexOfValue(Daemon.settings.importFormat ?? "apng"))
        onActivated: Daemon.setSettings({importFormat: currentValue})
    }

    QQC2.ComboBox {
        id: importSizeBox
        Kirigami.FormData.label: qsTr("Images of other sizes:")
        enabled: Daemon.available && Daemon.importsSupported
        textRole: "text"
        valueRole: "value"
        model: [
            {text: qsTr("Centre them without stretching"), value: "fit"},
            {text: qsTr("Scale them to the first image"), value: "scale"},
            {text: qsTr("Refuse the set"), value: "first"}
        ]
        currentIndex: Math.max(0, indexOfValue(Daemon.settings.importSizePolicy ?? "fit"))
        onActivated: Daemon.setSettings({importSizePolicy: currentValue})
    }

    QQC2.SpinBox {
        id: importFpsBox
        Kirigami.FormData.label: qsTr("Speed without timing (frames per second):")
        enabled: Daemon.available && Daemon.importsSupported
        from: 1
        to: 120
        editable: true
        value: Daemon.settings.importFps ?? 24
        onValueModified: Daemon.setSettings({importFps: value})
    }

    QQC2.Switch {
        id: importKeepSwitch
        enabled: Daemon.available && Daemon.importsSupported
        text: qsTr("Keep a copy of the original files of an import")
        checked: Daemon.settings.importKeepSource ?? false
        onToggled: Daemon.setSettings({importKeepSource: checked})
    }

    QQC2.Switch {
        id: importRequireSwitch
        enabled: Daemon.available && Daemon.importsSupported
        text: qsTr("Import only inside the sandbox")
        checked: Daemon.settings.importRequireSandbox ?? true
        onToggled: Daemon.setSettings({importRequireSandbox: checked})
    }

    Hint {
        text: qsTr("Importing reads files that may come from anywhere. Without the sandbox (no Landlock) imports are refused; turn this off to import anyway, and each such import says it was not sandboxed.")
    }

    QQC2.Switch {
        id: importGifSwitch
        enabled: Daemon.available && Daemon.importsSupported
        text: qsTr("Also convert a GIF to APNG when it is added as a pet")
        checked: Daemon.settings.convertGifOnImport ?? false
        onToggled: Daemon.setSettings({convertGifOnImport: checked})
    }

    QQC2.ComboBox {
        id: workshopFilesBox
        Kirigami.FormData.label: qsTr("A ZIP or video in a Workshop item:")
        enabled: Daemon.available && Daemon.importsSupported
        textRole: "text"
        valueRole: "value"
        model: [{text: qsTr("Ask me (Import this file)"), value: "ask"}, {text: qsTr("Import it automatically"), value: "auto"}]
        currentIndex: Math.max(0, indexOfValue(Daemon.settings.importWorkshopFiles ?? "ask"))
        onActivated: Daemon.setSettings({importWorkshopFiles: currentValue})
    }

    QQC2.ComboBox {
        id: workshopMdaaBox
        Kirigami.FormData.label: qsTr("A package (.mdaa) in a Workshop item:")
        enabled: Daemon.available && Daemon.importsSupported
        textRole: "text"
        valueRole: "value"
        model: [{text: qsTr("Ask me (Import this package)"), value: "ask"}, {text: qsTr("Import it automatically"), value: "auto"}]
        currentIndex: Math.max(0, indexOfValue(Daemon.settings.importWorkshopMdaa ?? "ask"))
        onActivated: Daemon.setSettings({importWorkshopMdaa: currentValue})
    }

    Hint {
        text: qsTr("A package's program (core.exe) is never extracted or run, whichever you choose.")
    }

    QQC2.SpinBox {
        id: importMaxBox
        Kirigami.FormData.label: qsTr("Largest import (MB):")
        enabled: Daemon.available && Daemon.importsSupported
        from: 16
        to: 1024
        stepSize: 16
        editable: true
        value: Daemon.settings.importMaxOutputMiB ?? 1024
        onValueModified: Daemon.setSettings({importMaxOutputMiB: value})
    }
}
