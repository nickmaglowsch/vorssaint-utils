import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Vorssaint

// Screen 2: preferences. Toggle, slider and shortcut recorder, each one a
// command into the settings service; the displayed values come back out of
// the snapshot, so the round trip through the bridge is what is on screen.
Window {
    id: prefs
    visible: true
    width: 420
    height: 260
    title: "Vorssaint preferences"

    CoreModel { id: settings; service: "settings" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        Label { text: "Preferences"; font.pixelSize: 18; font.bold: true }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Launch at login"; Layout.fillWidth: true }
            Switch {
                checked: settings.state.toggle === true
                onToggled: settings.invoke(JSON.stringify({ set: { key: "toggle", value: checked } }))
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Update interval" }
            Slider {
                Layout.fillWidth: true
                from: 0; to: 100
                value: settings.state.slider || 0
                onMoved: settings.invoke(JSON.stringify({ set: { key: "slider", value: Math.round(value) } }))
            }
            Label { text: Math.round(settings.state.slider || 0) + " %"; Layout.minimumWidth: 44 }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Shortcut"; Layout.fillWidth: true }
            Button {
                id: recorder
                property bool recording: false
                focus: recording
                text: recording ? "Press a chord..." : (settings.state.shortcut || "none")
                onClicked: recording = true
                Keys.onPressed: event => {
                    if (!recording) return;
                    const names = { 16777216: "Esc", 16777220: "Return", 32: "Space" };
                    if (event.key === Qt.Key_Shift || event.key === Qt.Key_Control
                        || event.key === Qt.Key_Alt || event.key === Qt.Key_Meta) return;
                    let parts = [];
                    if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl");
                    if (event.modifiers & Qt.AltModifier) parts.push("Alt");
                    if (event.modifiers & Qt.ShiftModifier) parts.push("Shift");
                    if (event.modifiers & Qt.MetaModifier) parts.push("Super");
                    parts.push(names[event.key] || String.fromCharCode(event.key));
                    settings.invoke(JSON.stringify({ recordShortcut: parts.join("+") }));
                    recording = false;
                    event.accepted = true;
                }
            }
        }

        Item { Layout.fillHeight: true }

        Label {
            Layout.fillWidth: true
            text: "snapshot: " + JSON.stringify(settings.state)
            font.pixelSize: 10
            wrapMode: Text.Wrap
            opacity: 0.6
        }
    }

    // Headless runs have no keyboard; drive the recorder once so the
    // screenshot shows a real round trip through vs_command.
    Timer {
        interval: 900; running: true
        onTriggered: settings.invoke(JSON.stringify({ recordShortcut: "Ctrl+Alt+K" }))
    }
}
