import QtQuick
import QtQuick.Window
import QtQml

// Draw a rectangle on the screen with the mouse (docs/pets/automation.md "Screen triggers"): a dim window over each screen; the rectangle is reported in the desktop's logical
// coordinates (screen position plus pointer). Escape cancels. It shows nothing of what is on the screen: it is only a shade to draw on.
Item {
    id: picker

    property bool active: false
    signal picked(int x, int y, int width, int height)
    signal cancelled()

    function pick() { active = true; }
    function finish() { active = false; }

    // Where a rectangle dragged on a shade is on the desktop: the shade's own origin (the screen's, plus the offset the compositor gave the window when it is mapped, as when it keeps a
    // panel out) plus the pointer's; the corners come in any order and the result is whole pixels. {x, y, w, h}.
    function desktopRect(screenX, screenY, windowX, windowY, x0, y0, x1, y1) {
        const dx = screenX + Math.max(0, windowX - screenX), dy = screenY + Math.max(0, windowY - screenY);
        return {x: Math.round(Math.min(x0, x1)) + dx, y: Math.round(Math.min(y0, y1)) + dy, w: Math.round(Math.abs(x1 - x0)), h: Math.round(Math.abs(y1 - y0))};
    }

    // Escape on any of the shades, even when the compositor never gave the keyboard to one of them.
    Shortcut {
        sequence: "Escape"
        enabled: picker.active
        context: Qt.ApplicationShortcut
        onActivated: { picker.finish(); picker.cancelled(); }
    }

    // (An Instantiator, not a Repeater: the delegates are windows, which a Repeater refuses.)
    Instantiator {
        model: picker.active ? Qt.application.screens : []

        delegate: Window {
            id: shade

            required property var modelData
            property real startX: 0
            property real startY: 0
            property real curX: 0
            property real curY: 0
            property bool dragging: false

            screen: modelData
            x: modelData.virtualX
            y: modelData.virtualY
            width: modelData.width
            height: modelData.height
            visible: true
            flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
            color: "#55000000"
            title: qsTr("Choose a region")

            Component.onCompleted: { requestActivate(); visibility = Window.FullScreen; }

            Rectangle {
                visible: shade.dragging
                x: Math.min(shade.startX, shade.curX)
                y: Math.min(shade.startY, shade.curY)
                width: Math.abs(shade.curX - shade.startX)
                height: Math.abs(shade.curY - shade.startY)
                color: "#3300aaff"
                border.color: "#00aaff"
                border.width: 2
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 24
                color: "white"
                style: Text.Outline
                styleColor: "black"
                font.pixelSize: 18
                text: qsTr("Drag over the region to watch. Escape, or a click without dragging, cancels.")
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.CrossCursor
                onPressed: mouse => { shade.startX = shade.curX = mouse.x; shade.startY = shade.curY = mouse.y; shade.dragging = true; }
                onPositionChanged: mouse => { shade.curX = Math.max(0, Math.min(shade.width, mouse.x)); shade.curY = Math.max(0, Math.min(shade.height, mouse.y)); }
                onReleased: {
                    shade.dragging = false;
                    const r = picker.desktopRect(shade.modelData.virtualX, shade.modelData.virtualY, shade.x, shade.y, shade.startX, shade.startY, shade.curX, shade.curY);
                    // (finish() destroys this window, which is in the middle of this handler: the Instantiator frees it later, so nothing here is used after it.)
                    picker.finish();
                    if (r.w >= 2 && r.h >= 2)
                        picker.picked(r.x, r.y, r.w, r.h);
                    else
                        picker.cancelled();
                }
            }

            Item {
                focus: true
                Keys.onEscapePressed: { picker.finish(); picker.cancelled(); }
            }
        }
    }
}
