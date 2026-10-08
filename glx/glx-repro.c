/*
 * Minimal GLX reproducer (no Qt): NVIDIA libnvidia-glcore crash when one thread updates a
 * vertex buffer (glBufferSubData) and draws on its current context, while another thread
 * creates and destroys an unrelated GLX context that is never made current.
 *
 * Mirrors what Qt Quick does: the render thread of an animating window updates dynamic
 * vertex buffers every frame (QGles2Buffer::fullDynamicBufferUpdateForCurrentFrame), and
 * when another window is exposed the GUI thread creates a temporary QOpenGLContext only to
 * resolve the surface format and destroys it again (QRhiGles2InitParams::newFallbackSurface).
 * Qt's QGLXContext creation makes that context current on a temporary dummy X window to query
 * the GL version/format, restores the previous (none) context, destroys the dummy window, and
 * the context itself is destroyed right after (glXDestroyContext). destroyer_mode 1 does this;
 * mode 0 only creates/destroys the context; mode 2 additionally maps a real 1600x44 "panel" window
 * each cycle, renders 10 frames into it on its own thread with its own context, and destroys both
 * (the full window lifecycle of the QML reproducer). Use destroyer_sleep_us=700000 to match it.
 * mode 3: the minimal trigger: the second thread only creates and destroys a 1x1 GLX pbuffer.
 *
 * Build: gcc -O2 -Wall -pthread -o glx-repro glx-repro.c -lGL -lX11
 * Run:   ./glx-repro [seconds=30] [destroyer_sleep_us=2000] [buffers_per_frame=8] [destroyer_mode=1] [separate_display=0] [effect=1] [qt_like=15]
 *        qt_like = bit mask, see QT_* below: 1 robust-access contexts, 2 share group, 4 make-current every
 *        frame, 8 pbuffer step (15 = all, what Qt does).
 *        effect=1: the animated content is rendered into an offscreen texture (FBO) every frame, then a
 *        blur "shadow" shader pass and a copy pass draw it to the window (like Qt5Compat DropShadow /
 *        MultiEffect over a live layer). effect=0: content drawn straight to the window.
 * Exit:  0 = no crash within the time limit; a crash shows up as SIGSEGV (exit code 139).
 */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef GLXContext (*CreateContextAttribs)(Display *, GLXFBConfig, GLXContext, Bool, const int *);

static Display *dpy_render, *dpy_destroy;
static GLXFBConfig fbconfig;
static CreateContextAttribs createContextAttribs;
static atomic_int running = 1;
static atomic_long frames, cycles;
static int destroyer_sleep_us = 2000, buffers_per_frame = 8, destroyer_mode = 1, effect = 1;
static XVisualInfo *visual;

#define VERTS 4096 /* per buffer: 4096 vec2 = 32 KiB */

/* qt_like bit mask (default 15 = all, like Qt): */
#define QT_ROBUST 1     /* GLX_CONTEXT_ROBUST_ACCESS_BIT_ARB on every context */
#define QT_SHARE 2      /* render contexts in one share group with a hidden share context */
#define QT_MAKECURRENT 4 /* glXMakeCurrent at the start of every frame */
#define QT_PBUFFER 8    /* temp context also made current on a 1x1 pbuffer for extension queries */
static int qt_like = 15;
/* REPRO_PBUFFER_STEP (env): 1 create/destroy pbuffer only, 2 + make temp context current on it, 3 (default) + queries */
static int pbuffer_step = 3;
/* REPRO_DUMMY (env): 1 (default) temp context made current on a 1x1 dummy window first, 0 skip */
static int dummy_step = 1;
static GLXContext share_ctx; /* Qt's hidden share context: all render contexts are created shared with it */

static GLXContext new_context_shared(Display *dpy, GLXContext share)
{
    /* Qt's default format: OpenGL 2.0; Qt's QRhi also asks for robust access */
    const int robust[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 2, GLX_CONTEXT_MINOR_VERSION_ARB, 0,
                           GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_ROBUST_ACCESS_BIT_ARB, None };
    const int plain[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 2, GLX_CONTEXT_MINOR_VERSION_ARB, 0, None };
    return createContextAttribs(dpy, fbconfig, share, True, (qt_like & QT_ROBUST) ? robust : plain);
}

static GLXContext new_context(Display *dpy) /* render contexts: in the share group when qt_like */
{
    return new_context_shared(dpy, (qt_like & QT_SHARE) ? share_ctx : NULL);
}

static const char *vs_src =
    "#version 110\n"
    "attribute vec2 pos; varying vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
/* shadow pass: 5x5 blur of the source alpha, offset by one pixel, drawn in translucent black (like DropShadow) */
static const char *blur_fs_src =
    "#version 110\n"
    "uniform sampler2D src; uniform vec2 texel; varying vec2 uv;\n"
    "void main() {\n"
    "  float a = 0.0;\n"
    "  for (int x = -2; x <= 2; x++) for (int y = -2; y <= 2; y++)\n"
    "    a += texture2D(src, uv + vec2(float(x), float(y) + 1.0) * texel * 2.0).a;\n"
    "  gl_FragColor = vec4(0.0, 0.0, 0.0, 0.5 * a / 25.0);\n"
    "}\n";
static const char *copy_fs_src =
    "#version 110\n"
    "uniform sampler2D src; varying vec2 uv;\n"
    "void main() { gl_FragColor = texture2D(src, uv); }\n";

static GLuint program(const char *fs_src)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "render: shader link failed\n"); exit(2); }
    return p;
}

#define W 1008
#define H 480

static void *render_thread(void *arg)
{
    Window win = *(Window *)arg;
    GLXContext ctx = new_context(dpy_render);
    if (!ctx || !glXMakeCurrent(dpy_render, win, ctx)) {
        fprintf(stderr, "render: context/makeCurrent failed\n");
        exit(2);
    }
    GLuint vbo[64];
    glGenBuffers(buffers_per_frame, vbo);
    static float data[VERTS * 2];
    for (int i = 0; i < buffers_per_frame; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, sizeof data, NULL, GL_DYNAMIC_DRAW);
    }

    /* effect resources: offscreen texture + FBO (the "layer"), full-screen quad, two shader programs */
    GLuint tex = 0, fbo = 0, quad = 0, blur = 0, copy = 0;
    if (effect) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "render: FBO incomplete\n"); exit(2); }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        static const float q[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        glGenBuffers(1, &quad);
        glBindBuffer(GL_ARRAY_BUFFER, quad);
        glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
        blur = program(blur_fs_src);
        copy = program(copy_fs_src);
        glUseProgram(blur);
        glUniform1i(glGetUniformLocation(blur, "src"), 0);
        glUniform2f(glGetUniformLocation(blur, "texel"), 1.0f / W, 1.0f / H);
        glUseProgram(copy);
        glUniform1i(glGetUniformLocation(copy, "src"), 0);
        glUseProgram(0);
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    float t = 0;
    while (running) {
        if (qt_like & QT_MAKECURRENT) /* QRhiGles2::ensureContext() at every beginFrame */
            glXMakeCurrent(dpy_render, win, ctx);
        /* 1. the "widget": animated content, rendered into the offscreen texture when the effect is on */
        if (effect) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glClearColor(0, 0, 0, 0);
        } else {
            glClearColor(0.2f, 0.3f, 0.25f, 1);
        }
        glViewport(0, 0, W, H);
        glClear(GL_COLOR_BUFFER_BIT);
        glColor4f(0.24f, 0.68f, 0.91f, 1);
        for (int i = 0; i < buffers_per_frame; i++) {
            for (int v = 0; v < VERTS; v++) { /* a spinning ring of points */
                float a = (float)v / VERTS * 6.2832f + t + i;
                data[2 * v] = 0.8f * __builtin_cosf(a) * (0.3f + 0.08f * i);
                data[2 * v + 1] = 0.8f * __builtin_sinf(a) * (0.3f + 0.08f * i);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo[i]);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof data, data); /* the call that crashes */
            glVertexPointer(2, GL_FLOAT, 0, 0);
            glDrawArrays(GL_POINTS, 0, VERTS);
        }
        /* 2. the "shadow": blur pass over the texture, then the texture itself on top */
        if (effect) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.2f, 0.3f, 0.25f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_2D, tex);
            glBindBuffer(GL_ARRAY_BUFFER, quad);
            glDisableClientState(GL_VERTEX_ARRAY);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glUseProgram(blur);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glUseProgram(copy);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glUseProgram(0);
            glDisableVertexAttribArray(0);
            glEnableClientState(GL_VERTEX_ARRAY);
            glDisable(GL_BLEND);
        }
        glXSwapBuffers(dpy_render, win);
        t += 0.05f;
        frames++;
    }
    glXMakeCurrent(dpy_render, None, NULL);
    glXDestroyContext(dpy_render, ctx);
    return NULL;
}

/* Qt's createDummyWindow(): 1x1 unmapped window with the config's visual */
static Window dummy_window(Display *dpy)
{
    XSetWindowAttributes a = { 0 };
    a.colormap = XCreateColormap(dpy, RootWindow(dpy, visual->screen), visual->visual, AllocNone);
    Window w = XCreateWindow(dpy, RootWindow(dpy, visual->screen), 0, 0, 1, 1, 0, visual->depth, InputOutput,
                             visual->visual, CWColormap, &a);
    XFreeColormap(dpy, a.colormap);
    return w;
}

/* mode 2: a "panel" window like the QML reproducer's: mapped, own context on its own render thread,
   a few frames, then context and window destroyed */
struct panel { Window win; };

static void *panel_render_thread(void *arg)
{
    struct panel *p = arg;
    GLXContext c = new_context(dpy_destroy);
    if (!c || !glXMakeCurrent(dpy_destroy, p->win, c))
        return NULL;
    for (int f = 0; f < 10 && running; f++) {
        if (qt_like & QT_MAKECURRENT)
            glXMakeCurrent(dpy_destroy, p->win, c);
        glViewport(0, 0, 1600, 44);
        glClearColor(0.12f, 0.14f, 0.15f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy_destroy, p->win);
    }
    glXMakeCurrent(dpy_destroy, None, NULL);
    glXDestroyContext(dpy_destroy, c);
    return NULL;
}

static void *destroyer_thread(void *arg)
{
    (void)arg;
    while (running) {
        struct panel panel = { 0 };
        if (destroyer_mode == 2) { /* new top-level window, mapped (the "expose") */
            XSetWindowAttributes a = { 0 };
            a.colormap = XCreateColormap(dpy_destroy, RootWindow(dpy_destroy, visual->screen), visual->visual, AllocNone);
            a.override_redirect = True;
            panel.win = XCreateWindow(dpy_destroy, RootWindow(dpy_destroy, visual->screen), 0, 436, 1600, 44, 0, visual->depth,
                                      InputOutput, visual->visual, CWColormap | CWOverrideRedirect, &a);
            XMapWindow(dpy_destroy, panel.win);
            XSync(dpy_destroy, False);
        }
        if (destroyer_mode == 3) { /* minimal: only create and destroy a 1x1 pbuffer, no contexts at all */
            const int pb_attribs[] = { GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, GLX_LARGEST_PBUFFER, False,
                                       GLX_PRESERVED_CONTENTS, False, None };
            GLXPbuffer pb = glXCreatePbuffer(dpy_destroy, fbconfig, pb_attribs);
            glXDestroyPbuffer(dpy_destroy, pb);
            cycles++;
            if (destroyer_sleep_us)
                usleep(destroyer_sleep_us);
            continue;
        }
        GLXContext tmp = new_context_shared(dpy_destroy, NULL); /* Qt's temp context is never shared */
        if (tmp && destroyer_mode >= 1) {
            GLint v[4];
            if (dummy_step) { /* QGLXContext::init(): query the format on a dummy window */
            Window w = dummy_window(dpy_destroy);
            glXMakeCurrent(dpy_destroy, w, tmp);
            (void)glGetString(GL_VERSION);
            glGetIntegerv(GL_MAJOR_VERSION, &v[0]);
            glGetIntegerv(GL_MINOR_VERSION, &v[1]);
            glGetIntegerv(GL_CONTEXT_FLAGS, &v[2]);
            glXMakeCurrent(dpy_destroy, None, NULL); /* restore "no context" */
            XDestroyWindow(dpy_destroy, w);
            }
            if (qt_like & QT_PBUFFER) { /* QOffscreenSurface: 1x1 pbuffer, temp context current on it, extension queries */
                const int pb_attribs[] = { GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, GLX_LARGEST_PBUFFER, False,
                                           GLX_PRESERVED_CONTENTS, False, None };
                GLXPbuffer pb = glXCreatePbuffer(dpy_destroy, fbconfig, pb_attribs);
                if (pbuffer_step >= 2) {
                    glXMakeContextCurrent(dpy_destroy, pb, pb, tmp);
                    if (pbuffer_step >= 3) {
                        GLint next = 0;
                        glGetIntegerv(GL_NUM_EXTENSIONS, &next);
                        for (GLint e = 0; e < next; e++)
                            (void)glGetStringi(GL_EXTENSIONS, e);
                        glGetIntegerv(GL_MAX_VARYING_COMPONENTS, v);
                    }
                    glXMakeContextCurrent(dpy_destroy, None, None, NULL);
                }
                glXDestroyPbuffer(dpy_destroy, pb);
            }
        }
        if (tmp)
            glXDestroyContext(dpy_destroy, tmp); /* ~QGLXContext */
        if (destroyer_mode == 2) { /* the panel's own render thread, then tear everything down */
            pthread_t pr;
            pthread_create(&pr, NULL, panel_render_thread, &panel);
            pthread_join(pr, NULL);
            XDestroyWindow(dpy_destroy, panel.win);
            XSync(dpy_destroy, False);
        }
        cycles++;
        if (destroyer_sleep_us)
            usleep(destroyer_sleep_us);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    destroyer_sleep_us = argc > 2 ? atoi(argv[2]) : 2000;
    buffers_per_frame = argc > 3 ? atoi(argv[3]) : 8;
    destroyer_mode = argc > 4 ? atoi(argv[4]) : 1;
    int separate_display = argc > 5 ? atoi(argv[5]) : 0;
    effect = argc > 6 ? atoi(argv[6]) : 1;
    qt_like = argc > 7 ? atoi(argv[7]) : 15;
    if (getenv("REPRO_PBUFFER_STEP")) pbuffer_step = atoi(getenv("REPRO_PBUFFER_STEP"));
    if (getenv("REPRO_DUMMY")) dummy_step = atoi(getenv("REPRO_DUMMY"));
    if (buffers_per_frame < 1 || buffers_per_frame > 64) buffers_per_frame = 8;

    XInitThreads(); /* Qt (xcb + Xlib) does this too */
    dpy_render = XOpenDisplay(NULL);
    if (!dpy_render) { fprintf(stderr, "cannot open display\n"); return 2; }
    dpy_destroy = separate_display ? XOpenDisplay(NULL) : dpy_render;

    createContextAttribs = (CreateContextAttribs)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    if (!createContextAttribs) { fprintf(stderr, "no glXCreateContextAttribsARB\n"); return 2; }

    const int fbattribs[] = { GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                              GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
                              GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8, GLX_DOUBLEBUFFER, True, None };
    int n = 0;
    GLXFBConfig *cfgs = glXChooseFBConfig(dpy_render, DefaultScreen(dpy_render), fbattribs, &n);
    if (!cfgs || n == 0) { fprintf(stderr, "no fbconfig\n"); return 2; }
    fbconfig = cfgs[0];
    XVisualInfo *vi = visual = glXGetVisualFromFBConfig(dpy_render, fbconfig);

    XSetWindowAttributes swa = { 0 };
    swa.colormap = XCreateColormap(dpy_render, RootWindow(dpy_render, vi->screen), vi->visual, AllocNone);
    Window win = XCreateWindow(dpy_render, RootWindow(dpy_render, vi->screen), 0, 0, 1008, 480, 0, vi->depth,
                               InputOutput, vi->visual, CWColormap, &swa);
    XStoreName(dpy_render, win, "glx-repro");
    if (qt_like & QT_SHARE) { /* the hidden share context, created first and kept alive (never current again) */
        share_ctx = new_context_shared(dpy_render, NULL);
        if (!share_ctx) { fprintf(stderr, "share context failed\n"); return 2; }
    }
    XMapWindow(dpy_render, win);
    XSync(dpy_render, False);

    printf("glx-repro: %s | %s | %d s, destroyer sleep %d us, %d buffers/frame, destroyer mode %d, %s display, effect %d, qt-like %d\n",
           glXGetClientString(dpy_render, GLX_VENDOR), glXGetClientString(dpy_render, GLX_VERSION),
           seconds, destroyer_sleep_us, buffers_per_frame, destroyer_mode, separate_display ? "separate" : "shared", effect, qt_like);
    fflush(stdout);

    pthread_t r, d;
    pthread_create(&r, NULL, render_thread, &win);
    usleep(200000);
    pthread_create(&d, NULL, destroyer_thread, NULL);

    for (int s = 0; s < seconds; s++)
        sleep(1);
    running = 0;
    pthread_join(d, NULL);
    pthread_join(r, NULL);
    printf("glx-repro: no crash: %ld frames, %ld create/destroy cycles\n", (long)frames, (long)cycles);
    return 0;
}
