import QtQuick
import QtQuick.Window
import Vorssaint

// Screen 1: the panel that drops out of the tray item. Live CPU sparkline,
// bound straight to the metrics service's snapshot.
Window {
    id: panel
    visible: true
    width: 340
    height: 200
    title: "Vorssaint panel"
    color: "#1c1f26"

    CoreModel {
        id: metrics
        service: "metrics"
        onStateChanged: spark.requestPaint()
    }

    Text {
        id: heading
        x: 16; y: 14
        text: "CPU"
        color: "#8b93a7"
        font.pixelSize: 12
        font.letterSpacing: 1.5
    }

    Text {
        x: 16; y: 30
        text: (metrics.state.cpu !== undefined ? metrics.state.cpu.toFixed(1) : "--") + " %"
        color: "#e8ecf3"
        font.pixelSize: 30
    }

    Canvas {
        id: spark
        x: 16; y: 84
        width: panel.width - 32
        height: 84
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const h = metrics.state.history || [];
            ctx.strokeStyle = "#2b303b";
            ctx.lineWidth = 1;
            for (let g = 0; g <= 4; g++) {
                const gy = height * g / 4;
                ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(width, gy); ctx.stroke();
            }
            if (h.length < 2)
                return;
            const step = width / (h.length - 1);
            const py = v => height - (v / 100) * height;
            ctx.beginPath();
            ctx.moveTo(0, height);
            for (let i = 0; i < h.length; i++) ctx.lineTo(i * step, py(h[i]));
            ctx.lineTo((h.length - 1) * step, height);
            ctx.closePath();
            ctx.fillStyle = "#1d3a52";
            ctx.fill();
            ctx.beginPath();
            for (let i = 0; i < h.length; i++) {
                const x = i * step, y = py(h[i]);
                i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
            }
            ctx.strokeStyle = "#4a90d9";
            ctx.lineWidth = 2;
            ctx.stroke();
        }
    }

    Text {
        x: 16; y: panel.height - 22
        text: (metrics.state.history || []).length + " samples"
        color: "#6b7488"
        font.pixelSize: 11
    }
}
