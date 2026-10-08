#!/bin/bash
# Run repro.qml N times and count crashes.
# Usage: run-repro.sh [runs] [effect: dropshadow|multieffect|layer|none] [panels] [period_ms]
# Honours QML_BIN, QSG_RENDER_LOOP (default threaded) and QSG_RHI_BACKEND (default opengl) from the environment.
runs=${1:-10}; effect=${2:-dropshadow}; panels=${3:-20}; period=${4:-700}
# Qt 6 qml runtime: Fedora /usr/lib64/qt6/bin, Debian/Ubuntu /usr/lib/qt6/bin, Arch qml6; override with QML_BIN
for qml in "${QML_BIN:-}" /usr/lib64/qt6/bin/qml /usr/lib/qt6/bin/qml "$(command -v qml6)"; do [ -x "$qml" ] && break; done
[ -x "$qml" ] || { echo "Qt 6 qml tool not found; set QML_BIN" >&2; exit 1; }
cd "$(dirname "$0")"
crash=0
for i in $(seq 1 "$runs"); do
    QSG_RENDER_LOOP=${QSG_RENDER_LOOP:-threaded} QSG_RHI_BACKEND=${QSG_RHI_BACKEND:-opengl} QT_FORCE_STDERR_LOGGING=1 \
        timeout 120 "$qml" repro.qml -- --REPRO_EFFECT=$effect --REPRO_PANELS=$panels --REPRO_PERIOD=$period > /dev/null 2>run.err
    rc=$?
    if [ $rc -ne 0 ]; then crash=$((crash+1)); echo "run $i: exit $rc $(grep -m1 -E 'REPRO|Segmentation|error' run.err)"; else echo "run $i: ok"; fi
done
echo "RESULT loop=${QSG_RENDER_LOOP:-threaded} backend=${QSG_RHI_BACKEND:-opengl} effect=$effect panels=$panels period=$period: $crash crashes / $runs runs"
