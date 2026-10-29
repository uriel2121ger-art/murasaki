import QtQuick

// Root of one output's surface. murasaki-surface sets `content` (SurfaceController::contentMap) and the playback
// flags; a new content key crossfades to a new delegate, and a failing delegate falls back to the preview.
Rectangle {
    id: root

    property var content: null
    property bool paused: false
    property bool audible: false
    property real refreshRate: 60
    // Animated wallpapers (web pages, scenes) draw at most this often (WE's FPS setting); they otherwise follow
    // the display rate.
    property real frameRateLimit: 30
    // Power saving (on battery): nothing, videos included, goes above this many frames a second; 0 for no cap.
    property real fpsCap: 0
    readonly property real effectiveFrameRateLimit: fpsCap > 0 ? Math.min(frameRateLimit, fpsCap) : frameRateLimit
    // The output this root covers (the scene engine names its screen after it).
    property string outputName: ""
    property var webProfile: null
    property var audioSpectrum: null
    // Whether the page on screen listens for audio; the outgoing page of a crossfade does not count.
    readonly property bool wantsAudio: internal.currentWeb ? internal.currentWeb.wantsAudio : false
    property string currentKey: ""
    property Item currentSlot: null // the newest delegate's slot: what the playback flags and a video seek talk to
    // What is on screen. A switch keeps it fully visible, animating, until the new slot's delegate has a first real frame
    // (or switchTimeout passes), then crossfades; the preview stands in only when nothing was shown before.
    property Item visibleSlot: null
    property Item pendingSlot: null // created and running, not yet visible
    property int switchTimeout: 10000

    // Failure the user can see: nothing was drawn (no engine would start it, it crashed for good, or no first frame came
    // within noFrameTimeout). The preview stays, dimmed, with a small mark in the corner (no text on the wallpaper); the
    // details go to the surface through health() and the GUI says them.
    property int noFrameTimeout: content && content.noFrameTimeoutMs > 0 ? content.noFrameTimeoutMs : 15000
    property bool failed: false
    property string failureKind: "" // "noframe" clears itself when the frame comes late, anything else stays
    // event: "failed" (reason, engine: what was running) or "frame" (the content drew its first frame).
    signal health(string key, string event, string reason, string engine)

    color: "black"
    clip: true // a spanned page or preview is laid out on the whole canvas and shifted; what is beyond this output is cut

    // Spanning: this output shows a rectangle of a canvas that spreads over several outputs, {canvasW, canvasH, x, y,
    // w, h} in logical pixels; empty otherwise. Videos and scenes crop themselves (mpv's pan and zoom, the engine's
    // SpanView); web pages and previews fill a canvas-sized slot that is moved so that this output's rectangle
    // lies on the item.
    readonly property var span: content && content.span && content.span.canvasW > 0 ? content.span : ({})
    readonly property bool spanning: span.canvasW !== undefined

    // The position in seconds of the video on screen, or -1; and a seek of it (spanned videos are kept in step).
    function videoPosition() {
        const slot = currentSlot
        if (!slot)
            return -1
        for (let i = 0; i < slot.children.length; ++i) {
            if (slot.children[i].position !== undefined)
                return slot.children[i].position()
        }
        return -1
    }
    function seekVideo(seconds) {
        const slot = currentSlot
        if (!slot)
            return
        for (let i = 0; i < slot.children.length; ++i) {
            if (slot.children[i].seekTo !== undefined)
                slot.children[i].seekTo(seconds)
        }
    }

    QtObject {
        id: internal
        property Item currentWeb: null // the web delegate on screen, if any
        property Item currentScene: null // the scene delegate on screen, if any
    }

    Component {
        id: slotComponent
        Item {
            // Pages and previews have no crop of their own: the slot is the canvas, shifted by this output's corner.
            property bool onCanvas: false
            readonly property bool shifted: onCanvas && root.spanning
            x: shifted ? -root.span.x : 0
            y: shifted ? -root.span.y : 0
            width: shifted ? root.span.canvasW : root.width
            height: shifted ? root.span.canvasH : root.height
            opacity: 0
            Behavior on opacity { NumberAnimation { duration: 400 } }
            // set by the delegate's first-frame signal; `revealed` once it is on screen (its audio is off until then)
            property bool ready: false
            property bool revealed: false
            property bool waiting: false
            property Item delegate: null
            // Armed while a video, page or scene has not drawn its first frame.
            property bool frameWait: false
            Timer {
                interval: root.switchTimeout
                running: parent.waiting
                onTriggered: root.reveal(parent)
            }
            Timer {
                // (not while paused: a scene started under a game or a covering window draws nothing until it plays, and was taken
                // for broken; when the pause ends the wait starts again in full)
                interval: root.noFrameTimeout
                running: parent.frameWait && !root.paused
                onTriggered: root.noFrame(parent)
            }
        }
    }
    Component { id: videoComponent; VideoItem { anchors.fill: parent; span: root.span; frameRateLimit: root.fpsCap } }
    Component { id: webComponent; WebItem { anchors.fill: parent } }
    Component { id: sceneComponent; SceneItem { anchors.fill: parent; span: root.span } }
    Component { id: previewComponent; PreviewItem { anchors.fill: parent } }

    onContentChanged: {
        const key = content ? content.key : ""
        if (key === currentKey) {
            // Same item and options: only user properties (and mute) can differ. A web page takes them live and
            // a reload (crash) must see the newest values; nothing else needs the content.
            const web = internal.currentWeb
            if (web && content.kind === "web") {
                if (web.script !== content.script)
                    web.updateScript(content.script)
                if (content.changedProperties && Object.keys(content.changedProperties).length > 0)
                    web.applyProperties(content.changedProperties)
            }
            const scene = internal.currentScene
            if (scene && content.kind === "scene")
                { scene.properties = content.properties; scene.desktop = content.desktop ?? "" } // the item sends only the keys that changed
            return
        }
        currentKey = key
        show(content ? content.kind : "none")
    }

    // Nothing came on screen within noFrameTimeout. Whatever is running stays (a slow scene may still arrive).
    function noFrame(slot) {
        slot.frameWait = false
        if (currentSlot !== slot || !content)
            return
        const reason = "no first frame within " + Math.round(noFrameTimeout / 1000) + " s"
        console.warn("murasaki: " + content.itemId + ": " + reason)
        failed = true
        failureKind = "noframe"
        health(content.key, "failed", reason, slot.delegate && slot.delegate.engineLabel !== undefined ? slot.delegate.engineLabel : "")
    }

    // The delegate of the current slot gave up: its preview takes its place, dimmed and marked.
    function giveUp(slot, reason) {
        const engine = slot.delegate && slot.delegate.engineLabel !== undefined ? slot.delegate.engineLabel : ""
        const key = content ? content.key : ""
        console.warn("murasaki: " + (content ? content.itemId : "?") + ": " + reason + "; showing its preview")
        show("preview") // clears currentWeb/currentScene
        failed = true
        failureKind = "failed"
        health(key, "failed", reason, engine)
    }

    // A watcher for the first real frame of a delegate: `callback` once, now if it is already there.
    function watchFirstFrame(kind, delegate, callback) {
        if (kind === "scene") {
            if (delegate.hasFrame)
                callback()
            else
                delegate.hasFrameChanged.connect(() => { if (delegate.hasFrame) callback() })
        } else if (kind === "video") {
            delegate.started.connect(callback)
        } else if (delegate.ready !== undefined) { // web and preview
            if (delegate.ready)
                callback()
            else
                delegate.readyChanged.connect(() => { if (delegate.ready) callback() })
        } else {
            callback()
        }
    }

    // Brings the pending slot on screen and retires what was shown: the old delegate is silenced, fades out under the new
    // one and is destroyed after the fade.
    function reveal(slot) {
        if (!slot || slot !== pendingSlot)
            return
        pendingSlot = null
        slot.waiting = false
        slot.revealed = true
        const old = visibleSlot
        visibleSlot = slot
        slot.opacity = 1
        if (old)
            retire(old)
    }

    function retire(old) {
        // Assigning breaks the binding; keeps the outgoing audio from overlapping the fade.
        for (let i = 0; i < old.children.length; ++i)
            old.children[i].audible = false
        for (let i = 0; i < old.children.length; ++i) {
            if (old.children[i].audioSpectrum !== undefined)
                old.children[i].audioSpectrum = null // breaks the binding: the outgoing page gets no more audio
        }
        old.destroy(450) // after the new slot has faded in
    }

    // A switch that never reached the screen (the choice changed again, or it failed) goes without a trace.
    function discardPending() {
        const p = pendingSlot
        if (!p)
            return
        pendingSlot = null
        p.waiting = false
        if (currentSlot === p)
            currentSlot = visibleSlot
        for (let i = 0; i < p.children.length; ++i)
            p.children[i].audible = false
        p.destroy()
    }

    function show(kind) {
        failed = false
        failureKind = ""
        discardPending()
        const shown = visibleSlot
        currentSlot = null
        internal.currentWeb = null
        internal.currentScene = null
        if (kind === "none") {
            if (shown) {
                visibleSlot = null
                retire(shown)
            }
            return
        }
        // Nothing before: the preview may stand in for a scene's first frame. Something before: it stays as it is.
        const nothingBefore = !shown
        const slot = slotComponent.createObject(root, { onCanvas: kind === "web" || kind === "preview" })
        let delegate = null
        if (kind === "video")
            delegate = videoComponent.createObject(slot, {
                source: content.entryFile, fill: content.fill, paused: root.paused, audible: false })
        else if (kind === "web" && content.webProfile)
            delegate = webComponent.createObject(slot, {
                profile: content.webProfile, lease: content.webLease ?? null, script: content.script, contentUrl: content.contentUrl,
                networkAllowed: content.networkAllowed === true, proxyToken: content.proxyToken ?? "",
                paused: root.paused, audible: false })
        else if (kind === "scene") {
            // (Not across outputs: it would show the whole preview on each of them.)
            const placeholder = nothingBefore && !root.spanning
                ? previewComponent.createObject(slot, { source: content.previewUrl, fill: content.fill }) : null
            delegate = sceneComponent.createObject(slot, {
                itemDir: content.itemDir, assetsDir: content.assetsDir, properties: content.properties,
                sceneEngine: content.sceneEngine, sandbox: content.sandboxEngines !== false, requireSandbox: content.requireSandbox === true, renderScale: content.sceneRenderScale ?? 0, fill: content.fill, outputName: root.outputName, desktop: content.desktop ?? "", volume: 1.0,
                paused: root.paused, audible: false })
            if (placeholder && delegate)
                placeholder.visible = Qt.binding(() => !delegate.hasFrame)
        } else
            delegate = previewComponent.createObject(slot, { source: content.previewUrl, fill: content.fill })
        if (!delegate) {
            console.warn("murasaki: cannot create a " + kind + " delegate for " + content.itemId)
            slot.destroy()
            currentSlot = shown
            return
        }
        delegate.paused = Qt.binding(() => root.paused)
        delegate.audible = Qt.binding(() => root.audible && slot.revealed)
        if (kind === "web") {
            delegate.refreshRate = Qt.binding(() => Math.min(root.refreshRate, root.effectiveFrameRateLimit))
            delegate.audioSpectrum = Qt.binding(() => root.audioSpectrum)
            internal.currentWeb = delegate
        }
        if (kind === "scene") {
            delegate.fps = Qt.binding(() => Math.max(1, Math.round(Math.min(root.refreshRate, root.effectiveFrameRateLimit))))
            internal.currentScene = delegate
        }
        if (kind === "video" || kind === "web" || kind === "scene") {
            const id = content.itemId
            if (kind === "scene") {
                // The item started before its check was back and the verdict says the Vulkan engine can draw it in full:
                // a second delegate (it picks the engine from the verdict) crossfades in over this one, once.
                delegate.engineUpgradeAvailable.connect(() => {
                    if (currentSlot === slot && root.content && root.content.kind === "scene")
                        show("scene")
                })
            }
            delegate.failed.connect(reason => {
                if (currentSlot === slot)
                    giveUp(slot, reason)
            })
        }
        slot.delegate = delegate
        currentSlot = slot
        pendingSlot = slot
        // Something is on screen: it stays until this one has its first frame, whatever the kind says it is
        const becameReady = () => { slot.ready = true; root.reveal(slot) }
        if (kind === "video" || kind === "web" || kind === "scene") {
            slot.frameWait = true
            const key = content.key
            watchFirstFrame(kind, delegate, () => {
                slot.frameWait = false
                if (failed && failureKind === "noframe" && currentSlot === slot) {
                    failed = false // it was only slow
                    failureKind = ""
                }
                health(key, "frame", "", "")
            })
        }
        if (nothingBefore) {
            reveal(slot) // the first content: there is nothing to keep on screen
            return
        }
        watchFirstFrame(kind, delegate, becameReady)
        if (pendingSlot === slot)
            slot.waiting = true // the timeout: a delegate that never gets there is shown anyway after switchTimeout
    }

    // The failure mark: the picture under it is dimmed and a small circle with an exclamation mark sits in a corner.
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: root.failed ? 0.5 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
    }
    Item {
        id: failureMark
        width: 28
        height: 28
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        opacity: root.failed ? 0.75 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.color: "white"
            border.width: 2
        }
        Rectangle { x: 13; y: 6; width: 2; height: 10; radius: 1; color: "white" }
        Rectangle { x: 13; y: 19; width: 2; height: 3; radius: 1; color: "white" }
    }
}
