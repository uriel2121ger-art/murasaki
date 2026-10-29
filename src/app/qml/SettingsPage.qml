import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Murasaki.App
import "settings"

// Settings in categories: a list on the left on wide windows, a drop-down on top on narrow ones. Each category is
// a form of its own (settings/*.qml) that reads and writes the daemon's settings.
Kirigami.Page {
    id: page

    title: qsTr("Settings")
    padding: 0

    readonly property var categories: [
        {key: "general", name: qsTr("General"), icon: "preferences-desktop", hint: qsTr("Start with the session, Plasma"), component: generalComponent},
        {key: "performance", name: qsTr("Performance"), icon: "speedometer", hint: qsTr("Frame rate, scene engine, quality"), component: performanceComponent},
        {key: "pause", name: qsTr("Pause"), icon: "media-playback-pause", hint: qsTr("Covered screens, games, battery, apps"), component: pauseComponent},
        {key: "pets", name: qsTr("Pets"), icon: "user-identity", hint: qsTr("Speed, sound, fullscreen, puppets, keys"), component: petsComponent},
        {key: "automation", name: qsTr("Automation"), icon: "preferences-system-time", hint: qsTr("Triggers and screen triggers"), component: automationComponent},
        {key: "import", name: qsTr("Importing pets"), icon: "document-import", hint: qsTr("Format, sizes, limits"), component: importComponent},
        {key: "steam", name: qsTr("Steam account"), icon: "internet-services", hint: qsTr("Downloads with SteamCMD"), component: steamComponent},
        {key: "privacy", name: qsTr("Privacy and security"), icon: "security-high", hint: qsTr("Sandbox, media, launching apps"), component: privacyComponent},
        {key: "about", name: qsTr("About"), icon: "help-about", hint: qsTr("Version and license"), component: aboutComponent}
    ]

    property string current: "general"
    readonly property int currentIndex: Math.max(0, categories.findIndex(c => c.key === current))
    readonly property bool wide: width > Kirigami.Units.gridUnit * 36

    // The last category seen is where the page opens next time.
    Settings {
        category: "settings"
        property alias lastCategory: page.current
    }

    // For the smoke test: shows every category once, so that a binding error in any of them shows.
    function smoke() {
        const was = current;
        for (const c of categories)
            current = c.key; // the loader builds the category at once
        current = was;
    }

    Component { id: generalComponent; GeneralSettings {} }
    Component { id: performanceComponent; PerformanceSettings {} }
    Component { id: pauseComponent; PauseSettings {} }
    Component { id: petsComponent; PetsSettings {} }
    Component { id: automationComponent; AutomationSettings {} }
    Component { id: importComponent; ImportSettings {} }
    Component { id: steamComponent; SteamSettings {} }
    Component { id: privacyComponent; PrivacySettings {} }
    Component { id: aboutComponent; AboutSettings {} }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // The categories, on wide windows.
        QQC2.ScrollView {
            Layout.fillHeight: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 14
            visible: page.wide
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            ListView {
                id: categoryList
                model: page.categories
                currentIndex: page.currentIndex
                activeFocusOnTab: true
                keyNavigationEnabled: true
                topMargin: Kirigami.Units.smallSpacing
                onCurrentIndexChanged: if (currentIndex >= 0) page.current = page.categories[currentIndex].key

                delegate: QQC2.ItemDelegate {
                    id: categoryRow

                    required property var modelData
                    required property int index

                    width: ListView.view.width
                    highlighted: ListView.isCurrentItem
                    onClicked: page.current = modelData.key
                    Accessible.name: modelData.name

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.Icon {
                            source: categoryRow.modelData.icon
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            selected: categoryRow.highlighted
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: categoryRow.modelData.name
                                elide: Text.ElideRight
                                color: categoryRow.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: categoryRow.modelData.hint
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                font: Kirigami.Theme.smallFont
                                opacity: 0.7
                                color: categoryRow.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                            }
                        }
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
            visible: page.wide
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The categories, on narrow windows.
            QQC2.ComboBox {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                visible: !page.wide
                model: page.categories
                textRole: "name"
                currentIndex: page.currentIndex
                onActivated: index => page.current = page.categories[index].key
            }

            QQC2.ScrollView {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff
                contentWidth: availableWidth

                ColumnLayout {
                    id: content

                    width: scroller.availableWidth
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.largeSpacing * 2
                        Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                        Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                        level: 2
                        text: page.categories[page.currentIndex].name
                    }

                    Loader {
                        id: categoryLoader
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        Layout.bottomMargin: Kirigami.Units.largeSpacing * 2
                        sourceComponent: page.categories[page.currentIndex].component
                        // A new category starts at the top.
                        onLoaded: scroller.QQC2.ScrollBar.vertical.position = 0
                    }
                }
            }
        }
    }
}
