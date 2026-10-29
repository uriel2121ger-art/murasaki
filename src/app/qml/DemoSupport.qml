import QtQuick
import Murasaki.App

// The "Try it" of a page (src/core/demos.h, docs/pets/automation.md "Examples"): asks the daemon to make the example of `kind` out of the user's pets, with names in the
// language of the interface and the size of this screen, and tells the page what came of it. Nothing it makes is real data: it is all marked, and "Remove examples" deletes it.
Item {
    id: root

    property string kind: ""
    property string petId: ""   // tracking: the pet that follows the cursor (empty: the first that can walk)

    // The daemon made the example; `result` is what demoDone says for the kind (Daemon.runDemo).
    signal done(var result)
    // There is no pet to make it from.
    signal needsPet()

    readonly property var names: ({
        timeline: qsTr("Example: pets across the screen"),
        trigger: qsTr("Example: a key that plays the timeline"),
        profile: qsTr("Example: my desktop"),
        tracking: qsTr("Example: follow the cursor"),
        screen: qsTr("Example: hide the pets while a picture is on the screen")
    })

    visible: false

    function run() {
        Daemon.runDemo(kind, {names: names, screenWidth: Screen.width, screenHeight: Screen.height, petId: petId});
    }

    Connections {
        target: Daemon
        function onDemoDone(demoKind, result) {
            if (demoKind === root.kind)
                root.done(result);
        }
        function onDemoNeedsPet(demoKind) {
            if (demoKind === root.kind)
                root.needsPet();
        }
    }
}
