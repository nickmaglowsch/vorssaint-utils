import QtQuick
import QtQuick.Window
import Vorssaint

// Screen 3: the capture overlay. Transparent fullscreen surface, drag a
// rectangle, the selected rect goes to the core and is printed.
Window {
    id: overlay
    visible: true
    visibility: Window.FullScreen
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    CoreModel { id: settings; service: "settings" }

    property real ox: 0
    property real oy: 0
    property real ow: 0
    property real oh: 0
    property bool dragging: false

    // The dimmed backdrop, with the selection punched out of it.
    Rectangle {
        anchors.fill: parent
        color: "#66000000"
        layer.enabled: true
    }
    Rectangle {
        x: overlay.ox; y: overlay.oy; width: overlay.ow; height: overlay.oh
        color: "#22ffffff"
        border.color: "#4a90d9"
        border.width: 2
        visible: overlay.ow > 0
    }
    Rectangle {
        x: overlay.ox; y: overlay.oy - 26
        width: label.width + 12; height: 22
        color: "#4a90d9"
        visible: overlay.ow > 0
        Text {
            id: label
            anchors.centerIn: parent
            color: "white"; font.pixelSize: 12
            text: Math.round(overlay.ox) + "," + Math.round(overlay.oy) + "  "
                  + Math.round(overlay.ow) + "x" + Math.round(overlay.oh)
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 40
        color: "white"; font.pixelSize: 16
        text: "Drag to select  -  core says: " + (settings.state.selection || "none")
    }

    function beginAt(x, y) { overlay.ox = x; overlay.oy = y; overlay.ow = 0; overlay.oh = 0; overlay.dragging = true }
    function extendTo(x, y) {
        if (!overlay.dragging) return;
        overlay.ow = Math.abs(x - overlay.ox); overlay.oh = Math.abs(y - overlay.oy);
    }
    function finish() {
        overlay.dragging = false;
        const rect = [Math.round(ox), Math.round(oy), Math.round(ow), Math.round(oh)].join(",");
        console.log("selected rect: " + rect);
        settings.invoke(JSON.stringify({ set: { key: "selection", value: rect } }));
    }

    MouseArea {
        anchors.fill: parent
        onPressed: mouse => overlay.beginAt(mouse.x, mouse.y)
        onPositionChanged: mouse => overlay.extendTo(mouse.x, mouse.y)
        onReleased: overlay.finish()
    }

    // Headless runs have no pointer; replay one drag through the same
    // functions the MouseArea calls, so the screenshot is of real state.
    Timer {
        interval: 700; running: demoDrag; repeat: false
        onTriggered: { overlay.beginAt(220, 180); overlay.extendTo(760, 520); overlay.finish() }
    }
}
