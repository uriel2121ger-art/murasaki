import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtWebEngine
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import Murasaki.App

Kirigami.Page {
    id: page

    readonly property string homeUrl: "https://steamcommunity.com/app/431960/workshop/"
    // Hosts the view may navigate to; anything else opens in the system browser.
    readonly property var allowedDomains: ["steamcommunity.com", "steampowered.com"]

    // Address to show instead of the Workshop home (set by the item page and the drawer).
    property string url: ""

    // The Workshop id of the item page on screen, or "".
    readonly property string itemId: {
        const match = /sharedfiles\/filedetails\/\?.*\bid=(\d+)/.exec(webView.url.toString());
        return match ? match[1] : "";
    }
    // "installed", "downloading" or "none"; the revision term re-evaluates it when Steam's files change.
    readonly property string itemState: itemId !== "" ? (Workshop.revision, Workshop.state(itemId)) : "none"
    property bool loadFailed: false
    // The one profile of the run, from Main.qml.
    readonly property WebEngineProfile profile: applicationWindow().workshopProfile()

    title: qsTr("Workshop")
    padding: 0

    // The state of "Use now" for the item on screen: "" when it is about another item or nothing was started.
    readonly property string useState: UseNow.itemId === "we:" + itemId ? UseNow.state : "idle"
    // The SteamCMD download of the item on screen, if one was asked for: {state, progress, error} or null.
    readonly property var steamCmdJob: (Daemon.steamCmd.jobs ?? []).find(j => j.itemId === itemId) ?? null
    readonly property bool steamCmdReady: Daemon.steamCmdSupported && (Daemon.steamCmd.available ?? false)
    readonly property bool steamCmdBusy: steamCmdJob !== null && (steamCmdJob.state === "queued" || steamCmdJob.state === "running")
    readonly property var knownDesktopIds: Surface.desktops.map(d => d.id)

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "\u2039").replace(/>/g, "\u203a");
    }

    function targetName(a) {
        const screen = a.output === "*" ? qsTr("All screens") : a.output;
        if (a.desktop === "*")
            return screen;
        const desk = Surface.desktops.find(d => d.id === a.desktop);
        return qsTr("%1, %2").arg(screen).arg(desk ? desk.name : a.desktop);
    }

    // "Download with SteamCMD": signed in, it queues the item; otherwise the account dialog comes first and the download
    // follows once the sign-in worked.
    function downloadWithSteamCmd() {
        if ((Daemon.steamCmd.state ?? "") === "logged-in") {
            Daemon.steamCmdDownload("431960", itemId);
            return;
        }
        steamCmdDialog.pendingItem = itemId;
        steamCmdDialog.open();
    }

    SteamCmdDialog {
        id: steamCmdDialog
        property string pendingItem: ""
        onSignedIn: {
            if (pendingItem !== "")
                Daemon.steamCmdDownload("431960", pendingItem);
            pendingItem = "";
        }
    }

    // "Use now": straight away when nothing narrower would hide the item, else ask what to do about those.
    function useNow() {
        const hiding = Daemon.overridingAssignments("*", "*", knownDesktopIds);
        if (hiding.length === 0) {
            UseNow.start("we:" + itemId, true, knownDesktopIds);
            return;
        }
        overrideDialog.names = hiding.map(a => plain(targetName(a))).join(", ");
        overrideDialog.open();
    }

    KirigamiDialogs.PromptDialog {
        id: overrideDialog

        property string names: ""

        title: qsTr("Some screens have their own wallpaper")
        subtitle: qsTr("These keep their own wallpaper, which hides the new one: %1").arg(names)
        standardButtons: KirigamiDialogs.PromptDialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                icon.name: "view-fullscreen"
                text: qsTr("Use on all screens")
                tooltip: qsTr("Replaces the wallpapers of the screens listed above")
                onTriggered: {
                    overrideDialog.close();
                    UseNow.start("we:" + page.itemId, true, page.knownDesktopIds);
                }
            },
            Kirigami.Action {
                icon.name: "dialog-ok-apply"
                text: qsTr("Only where nothing else is set")
                tooltip: qsTr("The screens listed above keep their wallpaper")
                onTriggered: {
                    overrideDialog.close();
                    UseNow.start("we:" + page.itemId, false, page.knownDesktopIds);
                }
            },
            Kirigami.Action {
                icon.name: "configure"
                text: qsTr("More options…")
                onTriggered: {
                    overrideDialog.close();
                    applicationWindow().openItem("we:" + page.itemId);
                }
            }
        ]
    }

    // scheme and host of an address, matched on its text so it does not depend on QUrl's JS bindings.
    function parts(address) {
        const match = /^([a-z][a-z0-9+.-]*):(?:\/\/(?:[^\/\\?#@]*@)?([^\/?#:]*))?/i.exec(address.toString());
        return match ? {scheme: match[1].toLowerCase(), host: (match[2] ?? "").toLowerCase()} : null;
    }

    // May the view itself navigate here? Only the Steam sites over http(s), and about:blank.
    function isAllowed(address) {
        const p = parts(address);
        if (!p)
            return false;
        if (p.scheme === "about")
            return address.toString() === "about:blank";
        if (p.scheme !== "https" && p.scheme !== "http")
            return false;
        return allowedDomains.some(d => p.host === d || p.host.endsWith("." + d));
    }

    // Hands a link the view refused to the system, if it is harmless: web pages, and the two steam:// forms that
    // only open a page in Steam. Everything else (file:, smb:, ssh:, javascript:, steam://run, steam://install,
    // ...) is dropped without a word.
    function openExternal(address) {
        const text = address.toString();
        const p = parts(text);
        if (!p)
            return;
        if (p.scheme === "https" || p.scheme === "http"
                || (p.scheme === "steam" && (text.startsWith("steam://url/") || text.startsWith("steam://openurl/"))))
            Qt.openUrlExternally(address);
    }

    onUrlChanged: if (url !== "") webView.url = url

    actions: [
        Kirigami.Action {
            icon.name: "go-previous"
            text: qsTr("Back")
            enabled: webView.canGoBack
            onTriggered: webView.goBack()
        },
        Kirigami.Action {
            icon.name: "go-next"
            text: qsTr("Forward")
            enabled: webView.canGoForward
            onTriggered: webView.goForward()
        },
        Kirigami.Action {
            icon.name: "view-refresh"
            text: qsTr("Reload")
            onTriggered: webView.reload()
        },
        Kirigami.Action {
            icon.name: "go-home"
            text: qsTr("Workshop home")
            onTriggered: webView.url = page.homeUrl
        }
    ]

    // Nothing is downloaded to disk from this page.
    Connections {
        target: page.profile
        function onDownloadRequested(download) { download.cancel(); }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // State of the item whose page is on screen.
        Rectangle {
            Layout.fillWidth: true
            visible: page.itemId !== ""
            implicitHeight: banner.implicitHeight + Kirigami.Units.largeSpacing * 2
            color: page.itemState === "installed" ? Kirigami.Theme.positiveBackgroundColor
                 : page.itemState === "downloading" ? Kirigami.Theme.neutralBackgroundColor
                 : Kirigami.Theme.backgroundColor

            Kirigami.Separator {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
            }

            RowLayout {
                id: banner
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: Kirigami.Units.largeSpacing
                }
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    visible: page.itemState !== "downloading"
                    source: page.itemState === "installed" ? "dialog-ok-apply" : "dialog-information"
                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                    implicitHeight: implicitWidth
                }
                QQC2.BusyIndicator {
                    visible: page.itemState === "downloading"
                    running: visible
                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                    implicitHeight: implicitWidth
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    QQC2.Label {
                        Layout.fillWidth: true
                        font.bold: true
                        elide: Text.ElideRight
                        text: page.itemState === "installed" ? qsTr("Installed")
                            : page.itemState === "downloading" ? qsTr("Downloading in Steam…")
                            : qsTr("Not installed")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: page.itemState === "installed" && page.useState !== "idle"
                        wrapMode: Text.Wrap
                        font: Kirigami.Theme.smallFont
                        textFormat: Text.PlainText
                        color: page.useState === "failed" ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                        text: page.useState === "waiting" ? qsTr("Waiting for Steam to finish…")
                            : page.useState === "applying" ? qsTr("Applying…")
                            : page.useState === "applied" ? qsTr("Applied to all screens")
                            : UseNow.message
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: page.itemState === "none" && page.steamCmdJob === null
                        wrapMode: Text.Wrap
                        opacity: 0.75
                        font: Kirigami.Theme.smallFont
                        text: page.steamCmdReady
                            ? qsTr("Log in on this page and press its Subscribe button, open the item in Steam, or download it with your Steam account through SteamCMD.")
                            : qsTr("Log in on this page and press its Subscribe button, or open the item in Steam.")
                    }
                    // A SteamCMD download of this item.
                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: page.steamCmdJob !== null && page.itemState !== "installed"
                        wrapMode: Text.Wrap
                        font: Kirigami.Theme.smallFont
                        textFormat: Text.PlainText
                        color: page.steamCmdJob && page.steamCmdJob.state === "failed" ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                        text: !page.steamCmdJob ? ""
                            : page.steamCmdJob.state === "queued" ? qsTr("SteamCMD: waiting for its turn…")
                            : page.steamCmdJob.state === "running" ? qsTr("SteamCMD: downloading… %1%").arg(Math.round(page.steamCmdJob.progress * 100))
                            : page.steamCmdJob.state === "failed" ? qsTr("SteamCMD: %1").arg(page.steamCmdJob.error)
                            : qsTr("SteamCMD: downloaded.")
                    }
                }

                QQC2.Button {
                    visible: page.itemState === "installed"
                    icon.name: "media-playback-start"
                    text: qsTr("Use now")
                    enabled: page.useState !== "waiting" && page.useState !== "applying"
                    onClicked: page.useNow()
                }
                QQC2.Button {
                    visible: page.steamCmdBusy
                    icon.name: "dialog-cancel"
                    text: qsTr("Cancel")
                    onClicked: Daemon.steamCmdCancel(page.itemId)
                }
                QQC2.Button {
                    visible: page.itemState === "none" && page.steamCmdReady && !page.steamCmdBusy
                    icon.name: "download"
                    text: qsTr("Download with SteamCMD")
                    onClicked: page.downloadWithSteamCmd()

                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: qsTr("Downloads with your own Steam account through SteamCMD, Valve's official tool. You must own Wallpaper Engine.")
                }
                QQC2.Button {
                    visible: page.itemState === "none"
                    icon.name: "steam"
                    text: qsTr("Open in Steam")
                    onClicked: Qt.openUrlExternally("steam://url/CommunityFilePage/" + page.itemId)
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            WebEngineView {
                id: webView

                anchors.fill: parent
                profile: page.profile
                url: page.url !== "" ? page.url : page.homeUrl

                onNavigationRequested: request => {
                    // Only the main frame matters: sub-resources (CDN images, scripts) and embedded frames are free.
                    if (request.isMainFrame && !page.isAllowed(request.url)) {
                        request.reject();
                        page.openExternal(request.url);
                    }
                }
                // Popups (Steam's login window, "open in new tab") load in this view.
                onNewWindowRequested: request => request.openIn(webView)
                // The page may not ask for anything: every permission (notifications, location, camera and
                // microphone, clipboard, ...) is denied, as are full screen, file dialogs and protocol handlers.
                onPermissionRequested: permission => permission.deny()
                onFullScreenRequested: request => request.reject()
                onFileDialogRequested: request => {
                    request.accepted = true;
                    request.dialogReject();
                }
                onRegisterProtocolHandlerRequested: request => request.reject()
                onLoadingChanged: loadRequest => {
                    if (loadRequest.status === WebEngineView.LoadSucceededStatus)
                        page.loadFailed = false;
                    // -3 (ERR_ABORTED) is a navigation replaced by another one, not a failure.
                    else if (loadRequest.status === WebEngineView.LoadFailedStatus && loadRequest.errorCode !== -3)
                        page.loadFailed = true;
                }
                // A menu of our own: the default one also offers "view source" and the developer tools.
                onContextMenuRequested: request => {
                    request.accepted = true;
                    contextMenu.request = request;
                    contextMenu.popup();
                }
            }

            QQC2.ProgressBar {
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                }
                visible: webView.loading && !page.loadFailed
                from: 0
                to: 100
                value: webView.loadProgress
            }

            Rectangle {
                anchors.fill: parent
                visible: page.loadFailed
                color: Kirigami.Theme.backgroundColor

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    icon.name: "network-disconnect"
                    text: qsTr("The Workshop could not be loaded")
                    explanation: qsTr("Check your internet connection and try again.")
                    helpfulAction: Kirigami.Action {
                        icon.name: "view-refresh"
                        text: qsTr("Retry")
                        onTriggered: {
                            page.loadFailed = false;
                            webView.reload();
                        }
                    }
                }
            }

            QQC2.Menu {
                id: contextMenu

                property var request: null

                QQC2.MenuItem {
                    text: qsTr("Open link")
                    visible: contextMenu.request && contextMenu.request.linkUrl.toString() !== ""
                    height: visible ? implicitHeight : 0
                    onTriggered: webView.triggerWebAction(WebEngineView.OpenLinkInThisWindow)
                }
                QQC2.MenuItem {
                    text: qsTr("Copy link address")
                    visible: contextMenu.request && contextMenu.request.linkUrl.toString() !== ""
                    height: visible ? implicitHeight : 0
                    onTriggered: webView.triggerWebAction(WebEngineView.CopyLinkToClipboard)
                }
                QQC2.MenuItem {
                    text: qsTr("Cut")
                    visible: contextMenu.request && contextMenu.request.isContentEditable
                    height: visible ? implicitHeight : 0
                    onTriggered: webView.triggerWebAction(WebEngineView.Cut)
                }
                QQC2.MenuItem {
                    text: qsTr("Copy")
                    visible: contextMenu.request && contextMenu.request.selectedText !== ""
                    height: visible ? implicitHeight : 0
                    onTriggered: webView.triggerWebAction(WebEngineView.Copy)
                }
                QQC2.MenuItem {
                    text: qsTr("Paste")
                    visible: contextMenu.request && contextMenu.request.isContentEditable
                    height: visible ? implicitHeight : 0
                    onTriggered: webView.triggerWebAction(WebEngineView.Paste)
                }
                QQC2.MenuItem {
                    text: qsTr("Back")
                    enabled: webView.canGoBack
                    onTriggered: webView.goBack()
                }
                QQC2.MenuItem {
                    text: qsTr("Forward")
                    enabled: webView.canGoForward
                    onTriggered: webView.goForward()
                }
                QQC2.MenuItem {
                    text: qsTr("Reload")
                    onTriggered: webView.reload()
                }
            }
        }
    }
}
