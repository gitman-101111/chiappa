// bridge.qml — display surface for einkbridge v2 (socket-driven rm2fb bridge)
//
// Renders the shared framebuffer (/dev/shm/swtfb) via the "framebuffer"
// image provider registered by einkbridge.cpp.  NO polling: einkbridge bumps
// the `seq` property once per received swtfb_update message, and the Image
// source binds to it, so the provider is re-queried exactly once per update.
// Between updates the process sleeps (this is what keeps a CPU core free —
// the v1 Timer-poll variant repainted the full panel 10×/s forever).
//
// `waveform` carries the client's requested rm2fb waveform id; used by the
// refresh-mode mapping (fast/partial vs full-quality refresh).
import QtQuick 2.15

Item {
    id: root
    // Sized by einkbridge's QQuickView (SizeRootObjectToView) to the panel
    // geometry (SWTFB_WIDTH/SWTFB_HEIGHT, Move defaults) — no literals here.

    // Set from C++ (SocketThread → GUI thread) per update
    property int seq: 0
    property int waveform: 2

    Image {
        id: fb
        anchors.fill: parent
        cache: false
        fillMode: Image.Stretch
        // seq in the URL busts Qt's per-URL image cache once per update
        source: "image://framebuffer/frame?" + root.seq
        asynchronous: false
    }

    // Boot banner: visible until the first client update arrives, so a
    // freshly booted device shows it's alive instead of a blank panel.
    Column {
        visible: root.seq === 0
        anchors.centerIn: parent
        spacing: 24

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "reMarkable Paper Pro Move"
            font.pixelSize: 48
            font.bold: true
            color: "black"
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Interact via SSH over USB to begin"
            font.pixelSize: 32
            color: "black"
        }
    }
}
