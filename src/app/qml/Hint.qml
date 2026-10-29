import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A short explanation under a setting: smaller and dimmer than the setting, wrapped to a readable width.
QQC2.Label {
    Layout.fillWidth: true
    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
    wrapMode: Text.Wrap
    textFormat: Text.PlainText
    opacity: 0.7
    font: Kirigami.Theme.smallFont
}
