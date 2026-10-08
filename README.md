# NVIDIA OpenGL crash with Qt Quick's threaded render loop (multiple windows)

A small, Qt-only reproducer for a segfault inside NVIDIA's OpenGL driver (`libnvidia-glcore.so`) that hits
Qt 6 Quick applications using more than one window with Qt's default **threaded** render loop on **X11**.
In practice it crashes **KDE Plasma's `plasmashell` at login**: the desktop window and the panel window each
have their own render thread, and a desktop widget animating under a shadow effect while the panel starts up
crashes the shell on roughly 40% of logins.

- Reproduces on **RTX 5060 Ti, RTX 5060 (Blackwell, GB206)** and **RTX 4060 Laptop (Ada, AD107)**
- Reproduces on driver branches **580 (580.178.04), 595 (595.104.02) and 615 (615.71.09)**
- Does **not** reproduce with a single render thread, with Qt's Vulkan backend, or with Mesa's OpenGL running
  on NVIDIA's Vulkan driver (zink) on the same GPU

## Crash signature

Crashing thread: Qt's per-window render thread.

```
#0 libnvidia-glcore.so.595.104.02 + 0xa2cea1
#1 libnvidia-glcore.so.595.104.02 + 0xa2185a
#2 libnvidia-glcore.so.595.104.02 + 0xa22efa
#3 libnvidia-glcore.so.595.104.02 + 0x72228c
#4 libnvidia-glcore.so.595.104.02 + 0x722402
#5 QSGBatchRenderer::Renderer::unmap(QSGBatchRenderer::Buffer*, bool)   (libQt6Quick.so.6)
#6 QSGBatchRenderer::Renderer::uploadBatch(QSGBatchRenderer::Batch*)     (libQt6Quick.so.6)
#7 QSGBatchRenderer::Renderer::prepareRenderPass(...)
#8 QSGBatchRenderer::Renderer::render()
#9 QSGRenderer::renderScene()
#10 QQuickWindowPrivate::renderSceneGraph()
#11 QSGRenderThread::syncAndRender()
#12 QSGRenderThread::run()
```

Same path on the other drivers:
- 615.71.09: `+0xa2cb11, +0xa21e5a, +0xa234fa, +0x72242c, +0x7225a2`
- 580.178.04: `+0xa31ad1, +0xa31f6a, +0xa35b1a, +0x705c7c, +0x705df2`

Every crash we collected (100+ across three machines) has this signature.

### What the main thread is doing at that moment: `glXDestroyContext`

Full all-thread backtraces (in [`backtraces/`](backtraces/), 5 reproducer crashes plus a real plasmashell login crash)
show the same picture every time. While the render thread is updating a dynamic vertex buffer on **its** context (`QSGBatchRenderer::Renderer::unmap`,
qsgbatchrenderer.cpp:1133 → `QGles2Buffer::fullDynamicBufferUpdateForCurrentFrame` → `glBindBuffer` + `glBufferSubData`;
the Qt frame in between is a tail call, so it doesn't show in the stack), the GUI thread is **destroying a different, temporary GLX context**:

```
GUI thread:
#0 glXDestroyContext ()                                   (libGLX_nvidia.so.0 + 0x72f7b)
#1 QGLXContext::~QGLXContext                              (qxcb-glx-integration: qglxintegration.cpp:458)
#2 QOpenGLContext::destroy / ~QOpenGLContext              (qopenglcontext.cpp:461/488)
#3 QRhiGles2InitParams::newFallbackSurface(QSurfaceFormat) (qrhigles2.cpp:642)
#4 QSGRhiSupport::maybeCreateOffscreenSurface(QWindow*)   (qsgrhisupport.cpp:901)
#5 QSGThreadedRenderLoop::handleExposure(QQuickWindow*)
#6 QWindow::event (Expose)                                <- the new window being shown
```

When a new window is exposed, Qt's threaded render loop creates a short-lived GL context on the GUI thread
(`newFallbackSurface` "resolves the format" for the window's offscreen fallback surface). Qt's GLX layer makes that
context current on a temporary 1×1 dummy window to query the GL version, releases it, destroys the dummy window,
and the context is destroyed right after.
So the race is:

- **Thread A (render thread of window 1):** `glBufferSubData` on context 1, which is current on that thread
- **Thread B (GUI thread):** `glXDestroyContext` on a temporary context 2, which is not shared with context 1

Two unrelated contexts on two threads; destroying one corrupts the other's buffer unmap. This is also exactly when
it happens in KDE Plasma: the panel window is exposed at login while the desktop window is animating. In the
plasmashell dump the GUI thread is in the same `glXDestroyContext ← newFallbackSurface ← handleExposure` path.

## Even smaller: GLX only, no Qt

[`glx/`](glx/) has a ~300-line C program that reproduces the same crash in a few seconds with plain Xlib/GLX/OpenGL:
one thread updates vertex buffers (`glBufferSubData`) and draws; a second thread does what Qt does when a window is
exposed: create a temporary context, make it current on a dummy window, **create and destroy a 1×1 pbuffer**, destroy
the context. Without the pbuffer step it never crashes; with it, every run crashes. Details and the switch matrix are in
[`glx/README.md`](glx/README.md).

## What the Qt reproducer does

`repro.qml` runs in Qt's `qml` tool, with no C++ and no KDE:

- **"Desktop" window** (1600×900): a `BusyIndicator` (continuous animation) in a 1008×480 item, optionally under a
  per-frame effect:
  - `dropshadow`: Qt5Compat `DropShadow`, with the same parameters KDE Plasma uses for desktop widgets with a
    shadow background
  - `multieffect`: Qt 6 `MultiEffect` shadow
  - `layer`: plain `layer.enabled` offscreen texture, no shader
  - `none`
- **"Panel" window** (frameless, 44 px high): created, rendered, and destroyed every 700 ms, 20 times per run.
  Each new window gets a new render thread that renders its first frames while the desktop window keeps animating.

`run-repro.sh` runs it N times and counts segfaults (exit code 139).

## Running it

Requirements: an X11 session and Qt 6 with Qt Quick Controls and Qt5Compat. On Fedora:
`sudo dnf install qt6-qtdeclarative-devel qt6-qt5compat` (the `qml` tool is in the `-devel` package). The script looks for `qml` in the usual Qt 6 locations; set `QML_BIN` if yours is elsewhere.

```sh
./run-repro.sh 10 multieffect     # threaded loop + OpenGL (the defaults)
./run-repro.sh 10 dropshadow
./run-repro.sh 10 layer
./run-repro.sh 10 none

# controls
QSG_RENDER_LOOP=basic  ./run-repro.sh 10 multieffect     # one render thread
QSG_RHI_BACKEND=vulkan ./run-repro.sh 10 multieffect     # Qt's Vulkan backend
__GLX_VENDOR_LIBRARY_NAME=mesa MESA_LOADER_DRIVER_OVERRIDE=zink ./run-repro.sh 10 multieffect   # Mesa GL on NVIDIA Vulkan
```

Each run takes about 15 s. Windows open and close on screen while it runs. Crash dumps land in `coredumpctl`
(executable `qml`).

## Results

Crashes per 10 runs (each run = 20 panel-window cycles). Raw output is in [`results/`](results/).

| Configuration | RTX 5060 Ti, 595.104.02 | RTX 4060 Laptop, 595.104.02 | RTX 4060 Laptop, 580.178.04 | RTX 5060, 615.71.09 |
|---|---|---|---|---|
| threaded GL, no effect | 1 | 0 | 1 | 0 |
| threaded GL, offscreen layer | 1 | 1 | 2 | 0 |
| threaded GL, Qt5Compat DropShadow | 3 | 8 | **10** | 4 |
| threaded GL, Qt 6 MultiEffect | 6 | **10** | **10** | 4 |
| **basic** loop (one render thread), GL | 0 | 0 | 0 | 0 |
| threaded, **Vulkan** backend | 0 | 0 | 0 | 0 |
| threaded, **zink** (Mesa GL on NVIDIA Vulkan) | 0 | – | – | 0 |

How to read it:
- The bug needs **two or more GL contexts on different threads** rendering at the same time in NVIDIA's OpenGL
  implementation. Even without any effect it crashes occasionally, given enough window creations.
- A per-frame blur/shadow shader in the animating window makes the crash far more likely. Qt 6's `MultiEffect` is
  no better than Qt5Compat's `DropShadow`.
- The same scene, threads and GPU with Mesa's OpenGL (zink over NVIDIA's Vulkan driver) never crashed, which points
  at the GL implementation, not the hardware or kernel driver.

## KDE Plasma impact (field data)

On the RTX 5060 Ti machine, plasmashell at login with a desktop widget showing an animated spinner:

| Desktop widget background | Logins | plasmashell crashes |
|---|---|---|
| Shadow background (live `DropShadow` over the widget) | 23 | 9 |
| Standard background (no per-frame effect) | 29+ | 0 |

The crash signature is identical to the reproducer. `QSG_RENDER_LOOP=basic` or `QSG_RHI_BACKEND=vulkan` for
plasmashell also avoided it.

## Test systems

| | Machine A | Machine B | Machine C |
|---|---|---|---|
| GPU | RTX 5060 Ti (GB206, 10de:2d04), VBIOS 98.06.39.40.B2 | RTX 4060 Laptop (AD107, 10de:28e0), VBIOS 95.07.40.40.40, NVIDIA-only (hybrid off) | RTX 5060 (GB206, 10de:2d05), VBIOS 98.06.39.80.2E |
| Driver | 595.104.02 (open modules) | 595.104.02 and 580.178.04 | 615.71.09 (open modules) |
| CPU / board | Ryzen 9 9950X, ASUS ROG Crosshair X870E Dark Hero | Alienware m16 R2, Core Ultra 9 185H | Ryzen 9 5950X, ASUS Pro WS X570-ACE |
| OS | Fedora 44, kernel 7.2.8, Xorg 21.1.24 | same | same (near-stock install) |
| Qt / desktop | Qt 6.11.2, KDE Plasma 6.7.5 (X11) | same | same |

## Workarounds

- For an affected Qt app: `QSG_RENDER_LOOP=basic` (one render thread) or `QSG_RHI_BACKEND=vulkan`.
  For plasmashell both have side effects: the variable is inherited by apps launched from the shell, and the basic
  loop had GL context-loss problems on device hot-plug in our tests.
- Avoid continuous animations under per-frame effects in the window that's already running when other windows start.

## License

MIT, see [LICENSE](LICENSE).
