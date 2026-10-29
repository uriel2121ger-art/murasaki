import QtQuick
import "Murasaki/Bridge"

// The wallpaper proper. Loaded by main.qml, which fills in the host program, the output name and the visibility.
Item {
    id: view

    property string program
    property string outputName
    property bool hidden: false

    // Placeholder until the first frame of the host arrives (and whenever there is none): the item's preview.
    MurasakiOutputStatus {
        id: status
        output: view.outputName
    }

    Image {
        anchors.fill: parent
        source: status.preview
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        visible: !bridge.hasFrame && source != ""
    }

    BridgeItem {
        id: bridge
        anchors.fill: parent
        // Nothing starts before the screen is known; changing either restarts the host.
        program: view.outputName !== "" ? view.program : ""
        arguments: ["--host", view.outputName]
        paused: view.hidden
        property bool gaveUp: false
        onFailed: gaveUp = true
        onHasFrameChanged: if (hasFrame) gaveUp = false
    }

    Text {
        anchors.centerIn: parent
        visible: bridge.gaveUp && !bridge.hasFrame
        color: "white"
        style: Text.Outline
        styleColor: "black"
        text: "Murasaki is not running"
    }
}
