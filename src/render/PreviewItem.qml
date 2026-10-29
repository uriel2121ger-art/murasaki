import QtQuick

// An item's preview image (animated GIFs play). Used for scenes until subproject 3, and when a video or web item
// fails.
Item {
    id: preview

    property url source
    property string fill: "fill"
    property bool paused: false
    property bool audible: false // unused; keeps the delegate interface uniform
    signal failed(string reason)
    // The picture is loaded (or could not be): a crossfade to this item can start.
    readonly property bool ready: image.status !== Image.Loading

    AnimatedImage {
        id: image
        anchors.fill: parent
        source: preview.source
        paused: preview.paused
        asynchronous: true
        cache: false
        fillMode: preview.fill === "fit" ? Image.PreserveAspectFit
                : preview.fill === "stretch" ? Image.Stretch
                : Image.PreserveAspectCrop
    }
}
