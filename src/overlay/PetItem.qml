import QtQuick
import Murasaki.Render
import Murasaki.Sprite
import Murasaki.Puppet

// One desktop pet: the animation, plus the controls of the arrange mode.
//
// Normally the window is as large as the pet and the pet fills it. In the arrange mode (`expanded`) the window
// covers the whole output and the pet is drawn at (petX, petY) with the size petW x petH; the pointer positions
// that are reported are then positions on the output, which PetWindow turns into the pet's new place and size.
Item {
    id: root

    property string source: ""
    property bool paused: false
    property bool arrange: false
    property real petOpacity: 1
    property bool expanded: false
    property real petX: 0
    property real petY: 0
    property real petW: 0
    property real petH: 0
    // Rotation (degrees, clockwise, about the pet's centre) and the flips of the picture: transforms of the scene graph, so nothing is decoded again.
    // The window is the footprint of the turned pet (PetWindow), the pet is centred in it; the arrange controls turn with the pet.
    property real petRotation: 0
    property bool flipH: false
    property bool flipV: false
    // The GPU frame cache: when spriteEnabled, the animation is decoded once at spriteW x spriteH pixels (each time
    // spriteKey changes, within spriteBudget bytes) and drawn from textures. The video plays until then, and again
    // if the frames cannot be made.
    property bool spriteEnabled: false
    property int spriteW: 0
    property int spriteH: 0
    property string spriteKey: ""
    property real spriteBudget: 0
    // The picture of the state likely to come next, decoded ahead into a second set of frames (the back sprite) so that the change is a swap, not a decode: when
    // spriteKey becomes the key that was preloaded the sets change places. Empty preloadKey: no preload (the back set is dropped).
    property string preloadSource: ""
    property int preloadW: 0
    property int preloadH: 0
    property string preloadKey: ""
    property real preloadBudget: 0
    // Another value: the picture on screen plays from its first frame again.
    property int restartToken: 0
    property bool frontIsB: false
    property string frontKey: ""
    property string backKey: ""
    readonly property var frontSprite: frontIsB ? spriteB : spriteA
    readonly property var backSprite: frontIsB ? spriteA : spriteB
    // The bytes the hit masks of the frames may take (made while the frames are decoded); 0: none.
    property real maskBudget: 0
    // Whether the frame on screen is reported (frameChanged): only a pet that has a use for its hit mask needs it.
    property bool reportFrames: false
    // How the frames are timed (a fixed rate, or a speed multiplier of the file's delays; both neutral: the file's own
    // timing) and the most frames per second the pet may change at (0: no limit). Applied live.
    property int timingFps: 0
    property real timingSpeed: 1
    property int fpsCap: 0
    // The same limit for the libmpv path, set only when the animation steps faster than it (mpv's fps filter would also repeat frames).
    property int videoFpsCap: 0
    // mpv's playback speed (1: the file's own timing).
    property real speed: 1

    // A puppet pet (docs/pets/puppets.md): the picture is drawn by murasaki-puppet, an engine process behind the sandbox, not played from a file. The sprites and the video are off then.
    property bool puppetMode: false
    property string puppetDir: ""
    property string puppetFile: ""
    property bool puppetSandbox: true
    property bool puppetRequireSandbox: true
    property bool puppetBlocked: false
    property bool puppetLook: false
    property string puppetExpression: ""
    property int puppetFps: 60
    property real puppetMaxPixels: 4194304
    // (The window reaches the PuppetItem itself - what it reports and the commands it takes - through the object, not through signals of this root.)

    signal dragStarted(real x, real y)
    signal dragMoved(real x, real y)
    signal resizeStarted(real x, real y)
    signal resizeMoved(real x, real y)
    signal editEnded()
    signal scaleAt(real factor, real x, real y)
    signal closeRequested()
    signal playbackFailed(string reason)
    signal playbackStarted()
    signal spriteDone(bool ok, real bytes, string reason)
    // The decode of the preloaded picture ended: the frames are ready (ok, bytes on the GPU side) or could not be made.
    signal preloadDone(bool ok, real bytes, string key, string reason)
    // The frame on screen (the index of its hit mask).
    signal frameChanged(int frame)
    // The animation wraps at `ns` (CLOCK_MONOTONIC) — announced ahead — for a pet that plays through libmpv; a pet from the frame cache reports it from its sprite item.
    signal wrapped(real ns, real loop)
    signal wrapCancelled()

    onRestartTokenChanged: {
        if (frontSprite.hasFrames)
            frontSprite.restart()
        else if (videoLoader.item)
            videoLoader.item.seekTo(0)
    }
    function dropSprites() {
        spriteA.clear()
        spriteB.clear()
        frontKey = ""
        backKey = ""
    }
    onSpriteEnabledChanged: if (!spriteEnabled) dropSprites()
    onSpriteKeyChanged: {
        if (!spriteEnabled || spriteKey === "")
            return
        if (backKey === spriteKey && backSprite.hasFrames) {
            // The picture was prepared: the sets change places. The old front is dropped (the controller asks for the next preload in the same apply).
            const bytes = backSprite.cacheBytes
            frontIsB = !frontIsB
            frontKey = backKey
            backKey = ""
            backSprite.clear()
            Qt.callLater(() => root.spriteDone(true, bytes, ""))
        } else {
            frontSprite.request(source, spriteW, spriteH, spriteBudget, 0, maskBudget)
            frontKey = spriteKey
            if (backKey === spriteKey) { // (it was still being decoded: that decode is cancelled and the front decodes it again; a rare change of state before it finished)
                backSprite.clear()
                backKey = ""
            }
        }
    }
    onPreloadKeyChanged: {
        if (preloadKey === "" || !spriteEnabled || preloadKey === frontKey) {
            if (backKey !== "") {
                backSprite.clear()
                backKey = ""
            }
            return
        }
        if (preloadKey === backKey)
            return
        backSprite.request(preloadSource, preloadW, preloadH, preloadBudget, 0, maskBudget)
        backKey = preloadKey
    }

    Item {
        id: pet
        // Expanded (arrange, GNOME): the window is the output and the pet sits at its place. Otherwise the window is the footprint of the turned pet
        // and the pet is in its middle (without a size yet, it fills the window).
        x: root.expanded ? root.petX : (root.petW > 0 ? (root.width - root.petW) / 2 : 0)
        y: root.expanded ? root.petY : (root.petH > 0 ? (root.height - root.petH) / 2 : 0)
        width: root.petW > 0 ? root.petW : root.width
        height: root.petH > 0 ? root.petH : root.height
        transformOrigin: Item.Center
        rotation: root.petRotation

        // The picture, mirrored in its own frame; the rotation of `pet` then turns the mirrored picture (petgeometry.h).
        Item {
            id: art
            anchors.fill: parent
            transform: Scale {
                origin.x: art.width / 2
                origin.y: art.height / 2
                xScale: root.flipH ? -1 : 1
                yScale: root.flipV ? -1 : 1
            }

        SpriteItem {
            id: spriteA
            anchors.fill: parent
            // Always visible: a hidden item is not synced, and its textures would not be released. Without frames
            // it draws nothing. The back set (the preloaded picture) is invisible and paused until it is the front.
            paused: root.paused || root.frontSprite !== spriteA
            timingFps: root.timingFps
            timingSpeed: root.timingSpeed
            fpsCap: root.fpsCap
            opacity: root.frontSprite === spriteA ? root.petOpacity : 0
            drawn: root.frontSprite === spriteA
            onCurrentFrameChanged: if (root.reportFrames && root.frontSprite === spriteA) root.frameChanged(currentFrame)
            onLoaded: bytes => { if (root.frontSprite === spriteA) root.spriteDone(true, bytes, ""); else root.preloadDone(true, bytes, root.backKey, "") }
            onFailed: reason => { if (root.frontSprite === spriteA) root.spriteDone(false, 0, reason); else root.preloadDone(false, 0, root.backKey, reason) }
            // The video is gone: what it announced is withdrawn; the sprite announces its own loops.
            onHasFramesChanged: if (hasFrames && root.frontSprite === spriteA) root.wrapCancelled()
        }

        SpriteItem {
            id: spriteB
            anchors.fill: parent
            paused: root.paused || root.frontSprite !== spriteB
            timingFps: root.timingFps
            timingSpeed: root.timingSpeed
            fpsCap: root.fpsCap
            opacity: root.frontSprite === spriteB ? root.petOpacity : 0
            drawn: root.frontSprite === spriteB
            onCurrentFrameChanged: if (root.reportFrames && root.frontSprite === spriteB) root.frameChanged(currentFrame)
            onLoaded: bytes => { if (root.frontSprite === spriteB) root.spriteDone(true, bytes, ""); else root.preloadDone(true, bytes, root.backKey, "") }
            onFailed: reason => { if (root.frontSprite === spriteB) root.spriteDone(false, 0, reason); else root.preloadDone(false, 0, root.backKey, reason) }
            onHasFramesChanged: if (hasFrames && root.frontSprite === spriteB) root.wrapCancelled()
        }

        Loader {
            id: puppetLoader
            anchors.fill: parent
            active: root.puppetMode
            sourceComponent: PuppetItem {
                enabled: false // the window takes the pointer (drag, click, the input region); the engine hears it from PetWindow, only to look
                itemDir: root.puppetDir
                modelFile: root.puppetFile
                sandbox: root.puppetSandbox
                requireSandbox: root.puppetRequireSandbox
                blocked: root.puppetBlocked
                look: root.puppetLook
                expression: root.puppetExpression
                paused: root.paused
                fps: root.puppetFps
                maxPixels: root.puppetMaxPixels
                fill: "stretch"
                opacity: root.petOpacity
                onFailed: reason => root.playbackFailed(reason)
                onPictureShown: root.playbackStarted()
            }
        }

        // Once the frames are on the GPU the video is gone altogether: no decoding, no mpv instance, no memory.
        Loader {
            id: videoLoader
            anchors.fill: parent
            active: !root.puppetMode && !root.frontSprite.hasFrames
            sourceComponent: VideoItem {
                transparent: true
                audible: false // pets are always muted
                fill: "stretch" // the pet's rectangle already has the animation's aspect ratio
                paused: root.paused
                speed: root.speed
                frameRateLimit: root.videoFpsCap
                source: root.source
                opacity: root.petOpacity
                onFailed: reason => root.playbackFailed(reason)
                onStarted: root.playbackStarted()
                onWrapped: (ns, loop) => {
                    // The sprite that replaces this video must not announce the loop 0 again: its sound has been scheduled or played.
                    if (loop === 0)
                        root.frontSprite.skipFirstWrap()
                    root.wrapped(ns, loop)
                }
                onWrapCancelled: root.wrapCancelled()
            }
        }
        }

        Item {
            anchors.fill: parent
            visible: root.arrange

            Rectangle {
                anchors.fill: parent
                color: "#1f7c4dff"
                border.color: "#e07c4dff"
                border.width: 2
                radius: 3
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                acceptedButtons: Qt.LeftButton
                // Positions are taken on the output: the window covers it while arranging.
                onPressed: mouse => { const p = mapToItem(root, mouse.x, mouse.y); root.dragStarted(p.x, p.y) }
                onPositionChanged: mouse => {
                    if (pressed) {
                        const p = mapToItem(root, mouse.x, mouse.y)
                        root.dragMoved(p.x, p.y)
                    }
                }
                onReleased: root.editEnded()
                onCanceled: root.editEnded()
                onWheel: wheel => {
                    const p = mapToItem(root, wheel.x, wheel.y)
                    root.scaleAt(Math.pow(1.1, wheel.angleDelta.y / 120), p.x, p.y)
                    wheel.accepted = true
                }
            }

            // Bottom-right corner: resize with the aspect ratio kept.
            Rectangle {
                width: 20
                height: 20
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: cornerArea.pressed ? "#ff7c4dff" : "#c07c4dff"
                radius: 3
                MouseArea {
                    id: cornerArea
                    anchors.fill: parent
                    cursorShape: Qt.SizeFDiagCursor
                    acceptedButtons: Qt.LeftButton
                    onPressed: mouse => { const p = mapToItem(root, mouse.x, mouse.y); root.resizeStarted(p.x, p.y) }
                    onPositionChanged: mouse => {
                        if (pressed) {
                            const p = mapToItem(root, mouse.x, mouse.y)
                            root.resizeMoved(p.x, p.y)
                        }
                    }
                    onReleased: root.editEnded()
                    onCanceled: root.editEnded()
                }
            }

            // Top-right corner: remove the pet.
            Rectangle {
                width: 22
                height: 22
                anchors.right: parent.right
                anchors.top: parent.top
                color: closeArea.containsMouse ? "#ffd04040" : "#c0d04040"
                radius: 11
                Text {
                    anchors.centerIn: parent
                    rotation: -root.petRotation // the controls turn with the pet; the glyph stays upright
                    text: "×"
                    color: "white"
                    font.pixelSize: 16
                    font.bold: true
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    acceptedButtons: Qt.LeftButton
                    onClicked: root.closeRequested()
                }
            }
        }
    }
}
