// Standalone reproducer (Qt only, no KDE): crash in NVIDIA's OpenGL driver (libnvidia-glcore)
// from QSGBatchRenderer::Renderer::unmap/uploadBatch with Qt Quick's threaded render loop on X11
// when several windows render at once.
//
// Mimics plasmashell at login: a "desktop" window shows an animated BusyIndicator under a
// per-frame effect (KDE Plasma's shadow background is a live Qt5Compat DropShadow over the
// whole widget), while "panel" windows (one render thread each) are created and draw their
// first frames.
//
// Run: ./run-repro.sh [runs] [effect], or: qml repro.qml -- --REPRO_EFFECT=<effect> (options below)
//   --REPRO_EFFECT=dropshadow   Qt5Compat DropShadow, as Plasma's shadow background (default)
//                  multieffect  Qt 6 QtQuick.Effects MultiEffect shadow
//                  layer        just render the widget into an offscreen texture (layer.enabled)
//                  none         no effect (control)
//   --REPRO_SHADOW=0            same as --REPRO_EFFECT=none
//   --REPRO_PANELS=N            number of panel create/destroy cycles (default 20)
//   --REPRO_PERIOD=ms           panel cycle period (default 700)
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import Qt5Compat.GraphicalEffects
import QtQuick.Effects

Window {
    id: desktop
    readonly property string effect: env("REPRO_SHADOW", "1") === "0" ? "none" : env("REPRO_EFFECT", "dropshadow")
    readonly property int panels: parseInt(env("REPRO_PANELS", "20"))
    readonly property int period: parseInt(env("REPRO_PERIOD", "700"))
    property int cycle: 0

    function env(name, dflt) {
        // qml runtime has no env access; values are passed in via the wrapper script as --name=value args
        const a = Qt.application.arguments.find(s => s.startsWith("--" + name + "="));
        return a ? a.split("=")[1] : dflt;
    }

    width: 1600; height: 900
    visible: true
    color: "#3a5a40"
    title: "repro desktop (effect=" + effect + ")"

    Item {
        id: widget
        x: 32; y: 32; width: 1008; height: 480
        layer.enabled: desktop.effect === "layer"
        BusyIndicator { anchors.centerIn: parent; running: true }
        Text { x: 10; y: 10; text: "widget"; color: "white"; font.pixelSize: 24 }
    }
    DropShadow {
        anchors.fill: widget
        z: -1
        horizontalOffset: 0; verticalOffset: 1
        radius: 4; samples: 9; spread: 0.35
        color: Qt.rgba(0, 0, 0, 0.5)
        source: desktop.effect === "dropshadow" ? widget : null
        visible: source !== null
    }
    MultiEffect {
        anchors.fill: widget
        z: -1
        visible: desktop.effect === "multieffect"
        source: visible ? widget : null
        shadowEnabled: true
        shadowColor: Qt.rgba(0, 0, 0, 0.5)
        shadowHorizontalOffset: 0; shadowVerticalOffset: 1
        shadowBlur: 0.4
    }

    Component {
        id: panelComponent
        Window {
            width: desktop.width; height: 44
            x: desktop.x; y: desktop.y + desktop.height - height
            flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
            color: "#202326"
            visible: true
            Row {
                anchors.fill: parent; spacing: 8; padding: 6
                Repeater {
                    model: 12
                    Rectangle { width: 32; height: 32; radius: 6; color: Qt.hsla(index / 12, 0.6, 0.5, 1) }
                }
                Text { text: Qt.formatTime(new Date(), "hh:mm:ss.zzz"); color: "white"; font.pixelSize: 18 }
            }
        }
    }

    property var panel: null
    Timer {
        interval: desktop.period; repeat: true; running: true
        onTriggered: {
            if (desktop.panel) { desktop.panel.destroy(); desktop.panel = null; }
            if (desktop.cycle >= desktop.panels) { console.warn("REPRO: done, no crash, cycles", desktop.cycle); Qt.quit(); return; }
            desktop.cycle++;
            desktop.panel = panelComponent.createObject(null);
        }
    }
    Component.onCompleted: console.warn("REPRO: start effect", effect, "panels", panels, "period", period)
}
