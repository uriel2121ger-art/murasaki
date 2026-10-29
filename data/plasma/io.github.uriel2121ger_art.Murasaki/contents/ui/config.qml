import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

// The wallpaper's page in Plasma's settings: there is nothing to configure here, what is shown is chosen in Murasaki.
// The status and the button live in ConfigStatus.qml, loaded so that a missing module shows a message and not an error.
ColumnLayout {
    // Plasma hands the dialog over; unused.
    property var configDialog
    property var wallpaperConfiguration

    Loader {
        Layout.fillWidth: true
        source: "ConfigStatus.qml"
        onStatusChanged: if (status === Loader.Error) fallback.visible = true
    }
    QQC2.Label {
        id: fallback
        visible: false
        text: "Murasaki module not found, run: murasakictl plasma disable"
    }
}
