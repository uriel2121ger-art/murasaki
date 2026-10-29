import QtQuick
import QtWebEngine

// A Wallpaper Engine web item. murasaki-surface supplies the profile that serves murasaki-item:// URLs and the
// page API script from buildWebApiScript(); this item only drives it.
WebEngineView {
    id: view

    property url contentUrl
    // Counts this view for the profile's owner, which deletes a retired profile only after its last view (see
    // WebProfileLease). Null: nobody waits for this view.
    property QtObject lease: null
    property string script
    property bool paused: false
    property bool audible: true
    // The user allowed this item to use the internet. Murasaki's request interceptor and Content-Security-Policy
    // enforce it for the network; the page script below closes WebRTC, which neither of them sees.
    property bool networkAllowed: false
    // Credentials for the NetworkGate proxy, only for an item with the permission (empty: the proxy refuses it).
    property string proxyToken: ""
    property real refreshRate: 60
    property var crashTimes: []
    // Property changes that arrived before the page started; merged and applied once it has.
    property var pendingProperties: ({})
    // The newest 128-value spectrum from the host; forwarded to the page while it runs.
    property var audioSpectrum: null
    // True once the page registered an audio listener (polled after load: pages register whenever they like).
    readonly property bool wantsAudio: internal.listening
    signal failed(string reason)
    // The page loaded and its API started: a crossfade to this item can start.
    readonly property bool ready: internal.started

    backgroundColor: "black"
    audioMuted: !audible
    settings.playbackRequiresUserGesture: false
    settings.showScrollBars: false
    settings.errorPageEnabled: false
    settings.focusOnNavigationEnabled: false
    settings.dnsPrefetchEnabled: networkAllowed

    QtObject {
        id: internal
        property bool listening: false
        property bool started: false // the page loaded and __murasaki.start() ran
        property bool requested: false // url was set
        property bool priming: false // the credentials of the proxy are being introduced, the page comes after
        property bool windowDrew: false // the window finished a frame since this item exists
    }

    // Note: a page that loads while its output is paused paints its first frame only after the unpause, because
    // the injected API holds requestAnimationFrame callbacks back while paused.
    // The page loads only once the view has a size and its window has drawn a frame: a page that loads into a 0x0
    // viewport (a Plasma host starts before its first frame) sizes its canvases once and stays black.
    function loadWhenReady() {
        if (internal.requested || !internal.windowDrew || width <= 0 || height <= 0)
            return
        internal.requested = true
        if (proxyToken !== "") {
            // The proxy asks for credentials once; a WebSocket handshake can not answer, so the first request is one that
            // can. The gate answers it itself, then the page loads.
            internal.priming = true
            url = "http://murasaki-gate.invalid/prime"
        } else {
            url = contentUrl
        }
    }
    onWidthChanged: loadWhenReady()
    onHeightChanged: loadWhenReady()
    Connections {
        target: view.Window.window
        enabled: !internal.windowDrew
        function onFrameSwapped() {
            internal.windowDrew = true
            view.loadWhenReady()
        }
    }

    Timer {
        id: listeningPoll
        interval: 1000
        repeat: true
        // Not while paused (the page's timers are throttled then); it resumes on unpause.
        running: internal.started && !view.paused && !internal.listening
        onTriggered: view.runJavaScript("window.__murasaki && window.__murasaki.listening()", function (result) {
            if (result === true) {
                internal.listening = true
            }
        })
    }

    Component.onDestruction: if (lease) lease.viewClosed()

    Component.onCompleted: {
        if (lease)
            lease.viewOpened()
        updateScript(script)
        loadWhenReady()
    }

    onLoadingChanged: function (loadRequest) {
        if (internal.priming) {
            if (loadRequest.status === WebEngineView.LoadSucceededStatus || loadRequest.status === WebEngineView.LoadFailedStatus
                    || loadRequest.status === WebEngineView.LoadStoppedStatus) {
                internal.priming = false
                url = contentUrl
            }
            return
        }
        if (loadRequest.status === WebEngineView.LoadStartedStatus) {
            // A (re)load starts a new page: it has registered nothing yet.
            internal.started = false
            internal.listening = false
        } else if (loadRequest.status === WebEngineView.LoadSucceededStatus) {
            internal.started = true
            runJavaScript("window.__murasaki && window.__murasaki.start(" + refreshRate + ")")
            flushProperties()
            sendSilence()
            applyPaused()
        } else if (loadRequest.status === WebEngineView.LoadFailedStatus) {
            failed("load failed: " + loadRequest.errorString)
        }
    }

    onRenderProcessTerminated: function (terminationStatus, exitCode) {
        const now = Date.now()
        crashTimes = crashTimes.filter(t => now - t < 60000).concat([now])
        if (crashTimes.length > 3)
            failed("the page crashed " + crashTimes.length + " times in 60 s")
        else
            reload()
    }

    onPausedChanged: applyPaused()

    onRefreshRateChanged: {
        if (internal.started)
            runJavaScript("window.__murasaki && window.__murasaki.setFps(" + refreshRate + ")")
    }

    onAudioSpectrumChanged: {
        if (!internal.started || paused || !audioSpectrum)
            return
        // Three decimals are plenty for a visualizer and keep the script text short.
        const text = JSON.stringify(Array.prototype.map.call(audioSpectrum, v => Math.round(v * 1000) / 1000))
        runJavaScript("window.__murasaki && window.__murasaki.audio(" + text + ")")
    }
    onAuthenticationDialogRequested: function (request) {
        request.accepted = true
        if (request.type === 1 /* AuthenticationTypeProxy */ && proxyToken !== "")
            request.dialogAccept("murasaki", proxyToken)
        else
            request.dialogReject()
    }
    onContextMenuRequested: function (request) { request.accepted = true }

    // Replaces the injected script; the running page keeps its own copy, but a reload (e.g. after a renderer crash)
    // picks this one up, so it must carry the newest property values.
    function updateScript(text) {
        script = text
        const scripts = [{
            name: "murasaki-we-api",
            sourceCode: text,
            injectionPoint: WebEngineScript.DocumentCreation,
            worldId: WebEngineScript.MainWorld
        }]
        if (!networkAllowed) {
            // WebRTC opens UDP and TCP sockets on its own (STUN, TURN, ICE checks to any address a page names), which
            // no request interceptor sees. Without the permission the API does not exist, in this page and in every
            // frame it creates, from the first line of script on.
            scripts.push({
                name: "murasaki-no-webrtc",
                sourceCode: noWebRtcScript,
                injectionPoint: WebEngineScript.DocumentCreation,
                worldId: WebEngineScript.MainWorld,
                runsOnSubFrames: true
            })
        }
        userScripts.collection = scripts
    }

    readonly property string noWebRtcScript: "(function () {" +
        "for (const name of ['RTCPeerConnection', 'webkitRTCPeerConnection', 'RTCDataChannel', 'RTCSessionDescription'," +
        " 'RTCIceCandidate', 'WebTransport', 'WebTransportBidirectionalStream', 'WebTransportDatagramDuplexStream'," +
        " 'WebTransportError']) {" +
        "try { Object.defineProperty(window, name, {value: undefined, writable: false, configurable: false}); } catch (e) {}" +
        "}})();"

    // Pushes changed user properties ({key: {value}}) into the running page without reloading it. A page that has
    // not started yet gets them right after start().
    function applyProperties(changed) {
        if (!changed || typeof changed !== "object")
            return
        if (!internal.started) {
            // defineProperty keeps a "__proto__" key an own property (assignment would set the prototype).
            const merged = {}
            for (const source of [pendingProperties, changed]) {
                for (const key of Object.keys(source))
                    Object.defineProperty(merged, key, { value: source[key], enumerable: true, writable: true, configurable: true })
            }
            pendingProperties = merged
            return
        }
        // JSON.parse of a string literal: a "__proto__" key stays an own property, unlike in an object literal.
        const literal = JSON.stringify(JSON.stringify(changed)).replace(/\u2028/g, "\\u2028").replace(/\u2029/g, "\\u2029")
        runJavaScript("window.__murasaki && window.__murasaki.applyProperties(JSON.parse(" + literal + "))")
    }

    function flushProperties() {
        const queued = pendingProperties
        pendingProperties = ({})
        if (Object.keys(queued).length > 0)
            applyProperties(queued)
    }

    // A fresh page, or one coming back from a pause, must not keep showing a stale spectrum until the next push.
    function sendSilence() {
        runJavaScript("window.__murasaki && window.__murasaki.audio(new Array(128).fill(0))")
    }

    // Pausing tells the page (WE's setPaused); the injected API then holds back animation frames. The view is neither
    // hidden nor frozen: hiding shrinks the page's viewport to 0x0, so pages that size their canvases once keep them
    // empty, and a freeze that lands while images are still loading cancels them. Either way the page comes back with
    // only its DOM text (a clock) on black.
    function applyPaused() {
        if (paused) {
            if (loading)
                return // onLoadingChanged calls applyPaused() again after LoadSucceeded
            runJavaScript("window.__murasaki && window.__murasaki.setPaused(true)")
        } else {
            runJavaScript("window.__murasaki && window.__murasaki.setPaused(false)")
            if (internal.started)
                sendSilence()
        }
    }
}
