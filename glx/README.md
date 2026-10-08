# GLX-only reproducer (no Qt)

`glx-repro.c` reproduces the same `libnvidia-glcore` crash with plain Xlib/GLX/OpenGL, in a few seconds.

```sh
gcc -O2 -Wall -pthread -o glx-repro glx-repro.c -lGL -lX11 -lm
./glx-repro 12 2000 8 1 0 0 8      # minimal recipe below; exit code 139 = crashed
./glx-repro                        # everything Qt does (defaults), also crashes
```

Arguments: `seconds destroyer_sleep_us buffers_per_frame destroyer_mode separate_display effect qt_like`
(see the comment at the top of the file). Environment: `REPRO_PBUFFER_STEP`, `REPRO_DUMMY`.

## The recipe

- **Thread A** (like Qt's render thread): one window, one context, every frame updates vertex buffers with
  `glBindBuffer` + `glBufferSubData`, draws them, and calls `glXSwapBuffers`.
- **Thread B** (like Qt's GUI thread when a new window is exposed, `QRhiGles2InitParams::newFallbackSurface`),
  in a loop every 2 ms:
  1. `glXCreateContextAttribsARB` (OpenGL 2.0, not shared)
  2. make it current on a 1×1 dummy window, query `GL_VERSION`/flags, release it, destroy the window (`QGLXContext` init)
  3. **`glXCreatePbuffer` (1×1) + `glXDestroyPbuffer`** (`QOffscreenSurface`)
  4. `glXDestroyContext`

Thread A crashes in `glBufferSubData` with the same `libnvidia-glcore` frames as the Qt and plasmashell crashes:
`+0xa2cea1, +0xa2185a, +0xa22efa, +0x72228c, +0x722402` on 595.104.02.

## What matters (RTX 5060 Ti, 595.104.02, 12 s runs)

| Thread B does | Runs crashed |
|---|---|
| everything Qt does (robust-access flag, shared contexts, make-current every frame, pbuffer step) | 6 / 6 (in 2–5 s) |
| temp context + dummy window + **pbuffer create/destroy** (no robust flag, no sharing, no per-frame make-current, no effect) | 4 / 4 |
| temp context + pbuffer create/destroy, no dummy window | 2 / 4 |
| temp context + dummy window, **no pbuffer** | 0 / 3 (+0 in ~10 earlier runs, up to 156k cycles) |
| **only** pbuffer create/destroy, no contexts | 0 / 11 |
| temp context made current on the pbuffer (no dummy window) | 0 / 4 |

Single Qt habits on their own (robust flag, share group, make-current every frame) don't crash;
the blur/shadow pass (`effect=1`) isn't needed either: the pbuffer step is what does it, together with a context
being created/destroyed around it on the second thread.

## Is the program (or Qt) misusing GLX?

We checked the GLX threading rules: each context is only ever current on one thread; the temporary context and the pbuffer
are released before they're destroyed; `XInitThreads()` is called first. And:

| Run | Result |
|---|---|
| NVIDIA, both threads on **separate X connections** (`separate_display=1`) | still crashes, 3 / 3 |
| **All-Mesa stack** (Xvfb + Mesa llvmpipe), same binary, same recipe and full Qt-like mode | 0 / 3 runs, ~2000 cycles each, no X or GL errors |

(Mesa's client GLX can't be tested against NVIDIA's X server: creating a pbuffer through NVIDIA's server-side GLX fails with
`GLXBadPbuffer`, even in pbuffer-only mode, hence Xvfb.) Valid GLX usage, a clean run on another implementation, and a segfault
inside `libnvidia-glcore` point to the NVIDIA driver.
