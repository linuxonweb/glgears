/*
 * glgears: port of Mesa demos glxgears (Brian Paul, public domain / MIT) to
 * surfaceless EGL for the wasm32 Linux guest.
 *
 * The scene is glxgears unchanged: three display-list gears (same geometry,
 * colours, light, frustum, 20/30/0 view rotation, 70 degrees/second), drawn
 * with the fixed-function desktop GL path (EGL_OPENGL_API). There is no GLX in
 * this static Mesa build, so the context is made current without a surface
 * (EGL_KHR_surfaceless_context) and renders into an FBO (RGBA8 + depth24).
 * Every GL entry point is resolved through eglGetProcAddress.
 *
 * Presentation: glReadPixels (BGRA) -> XImage -> XPutImage into an X11 window
 * (CPU copy, as gputest-demo/vkcube-lite.c). -nopresent skips X11 entirely and
 * ends each frame with glFinish, which measures pure rendering.
 *
 * Driver selection is Mesa's own: LIBGL_ALWAYS_SOFTWARE=1 [GALLIUM_DRIVER=softpipe]
 * or MESA_LOADER_DRIVER_OVERRIDE=zink. Output: glxgears-style
 * "N frames in 5.0 seconds = X FPS" lines plus a parseable GLGEARS_FPS line.
 *
 * -window instead renders to an EGL X11 window surface (EGL_PLATFORM_X11_KHR,
 * Mesa built with -Dplatforms=x11): Zink presents through the Vulkan swapchain
 * (kopper), eglSwapInterval 0, no CPU copy.
 *
 * Options: -nopresent  -window  -seconds N (0 = forever)  -size WxH  -info  -interval S
 *          -readrgba (read GL_RGBA, the renderbuffer's own format, and swizzle
 *          on the CPU; Zink otherwise converts to BGRA with a GPU blit)
 * Exit codes: 0 ok, 1 usage, 2 EGL failure, 3 GL failure, 4 X11 failure.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifndef M_PI
#define M_PI 3.14159265
#endif

static void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout);
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---- GL entry points (all through eglGetProcAddress) ---------------------- */
#define GL_FUNCS(X) \
    X(void, glBegin, (GLenum)) X(void, glEnd, (void)) X(void, glVertex3f, (GLfloat, GLfloat, GLfloat)) \
    X(void, glNormal3f, (GLfloat, GLfloat, GLfloat)) X(void, glShadeModel, (GLenum)) \
    X(GLuint, glGenLists, (GLsizei)) X(void, glNewList, (GLuint, GLenum)) X(void, glEndList, (void)) \
    X(void, glCallList, (GLuint)) X(void, glMaterialfv, (GLenum, GLenum, const GLfloat *)) \
    X(void, glLightfv, (GLenum, GLenum, const GLfloat *)) X(void, glEnable, (GLenum)) \
    X(void, glClear, (GLbitfield)) X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glPushMatrix, (void)) X(void, glPopMatrix, (void)) \
    X(void, glRotatef, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, glTranslatef, (GLfloat, GLfloat, GLfloat)) \
    X(void, glMatrixMode, (GLenum)) X(void, glLoadIdentity, (void)) \
    X(void, glFrustum, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) X(void, glFinish, (void)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(const GLubyte *, glGetString, (GLenum)) X(GLenum, glGetError, (void)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum))
#define DECLARE(ret, name, args) static ret (*p_##name) args;
GL_FUNCS(DECLARE)
#undef DECLARE
#define glBegin p_glBegin
#define glEnd p_glEnd
#define glVertex3f p_glVertex3f
#define glNormal3f p_glNormal3f
#define glShadeModel p_glShadeModel
#define glGenLists p_glGenLists
#define glNewList p_glNewList
#define glEndList p_glEndList
#define glCallList p_glCallList
#define glMaterialfv p_glMaterialfv
#define glLightfv p_glLightfv
#define glEnable p_glEnable
#define glClear p_glClear
#define glClearColor p_glClearColor
#define glPushMatrix p_glPushMatrix
#define glPopMatrix p_glPopMatrix
#define glRotatef p_glRotatef
#define glTranslatef p_glTranslatef
#define glMatrixMode p_glMatrixMode
#define glLoadIdentity p_glLoadIdentity
#define glFrustum p_glFrustum
#define glViewport p_glViewport
#define glFinish p_glFinish
#define glReadPixels p_glReadPixels
#define glPixelStorei p_glPixelStorei
#define glGetString p_glGetString
#define glGetError p_glGetError
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glCheckFramebufferStatus p_glCheckFramebufferStatus

static int load_gl(void) {
    int missing = 0;
#define LOAD(ret, name, args) \
    if (!(p_##name = (ret (*) args)eglGetProcAddress(#name))) { say("GLGEARS_ERROR missing GL function %s\n", #name); missing++; }
    GL_FUNCS(LOAD)
#undef LOAD
    return !missing;
}

/* ---- glxgears scene (verbatim geometry) ---------------------------------- */
static GLfloat view_rotx = 20.0, view_roty = 30.0, view_rotz = 0.0;
static GLint gear1, gear2, gear3;
static GLfloat angle = 0.0;

/*
 *  Draw a gear wheel.  You'll probably want to call this function when
 *  building a display list since we do a lot of trig here.
 *
 *  Input:  inner_radius - radius of hole at center
 *          outer_radius - radius at center of teeth
 *          width - width of gear teeth - number of teeth
 *          tooth_depth - depth of tooth
 */
static void gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width, GLint teeth, GLfloat tooth_depth) {
    GLint i;
    GLfloat r0, r1, r2;
    GLfloat angle, da;
    GLfloat u, v, len;

    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2.0;
    r2 = outer_radius + tooth_depth / 2.0;

    da = 2.0 * M_PI / teeth / 4.0;

    glShadeModel(GL_FLAT);

    glNormal3f(0.0, 0.0, 1.0);

    /* draw front face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        if (i < teeth) {
            glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
            glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5);
        }
    }
    glEnd();

    /* draw front sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0 * M_PI / teeth / 4.0;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), width * 0.5);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5);
    }
    glEnd();

    glNormal3f(0.0, 0.0, -1.0);

    /* draw back face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        if (i < teeth) {
            glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -width * 0.5);
            glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        }
    }
    glEnd();

    /* draw back sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0 * M_PI / teeth / 4.0;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
    }
    glEnd();

    /* draw outward faces of teeth */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
        u = r2 * cos(angle + da) - r1 * cos(angle);
        v = r2 * sin(angle + da) - r1 * sin(angle);
        len = sqrt(u * u + v * v);
        u /= len;
        v /= len;
        glNormal3f(v, -u, 0.0);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
        glNormal3f(cos(angle), sin(angle), 0.0);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -width * 0.5);
        u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
        v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
        glNormal3f(v, -u, 0.0);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -width * 0.5);
        glNormal3f(cos(angle), sin(angle), 0.0);
    }

    glVertex3f(r1 * cos(0), r1 * sin(0), width * 0.5);
    glVertex3f(r1 * cos(0), r1 * sin(0), -width * 0.5);

    glEnd();

    glShadeModel(GL_SMOOTH);

    /* draw inside radius cylinder */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glNormal3f(-cos(angle), -sin(angle), 0.0);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
    }
    glEnd();
}

static void draw(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPushMatrix();
    glRotatef(view_rotx, 1.0, 0.0, 0.0);
    glRotatef(view_roty, 0.0, 1.0, 0.0);
    glRotatef(view_rotz, 0.0, 0.0, 1.0);

    glPushMatrix();
    glTranslatef(-3.0, -2.0, 0.0);
    glRotatef(angle, 0.0, 0.0, 1.0);
    glCallList(gear1);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.1, -2.0, 0.0);
    glRotatef(-2.0 * angle - 9.0, 0.0, 0.0, 1.0);
    glCallList(gear2);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1, 4.2, 0.0);
    glRotatef(-2.0 * angle - 25.0, 0.0, 0.0, 1.0);
    glCallList(gear3);
    glPopMatrix();

    glPopMatrix();
}

/* new window size or exposure */
static void reshape(int width, int height) {
    GLfloat h = (GLfloat)height / (GLfloat)width;

    glViewport(0, 0, (GLint)width, (GLint)height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0, 0.0, -40.0);
}

static void init(void) {
    static GLfloat pos[4] = { 5.0, 5.0, 10.0, 0.0 };
    static GLfloat red[4] = { 0.8, 0.1, 0.0, 1.0 };
    static GLfloat green[4] = { 0.0, 0.8, 0.2, 1.0 };
    static GLfloat blue[4] = { 0.2, 0.2, 1.0, 1.0 };

    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);

    /* make the gears */
    gear1 = glGenLists(1);
    glNewList(gear1, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
    gear(1.0, 4.0, 1.0, 20, 0.7);
    glEndList();

    gear2 = glGenLists(1);
    glNewList(gear2, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, green);
    gear(0.5, 2.0, 2.0, 10, 0.7);
    glEndList();

    gear3 = glGenLists(1);
    glNewList(gear3, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, blue);
    gear(1.3, 2.0, 0.5, 10, 0.7);
    glEndList();

    glEnable(GL_NORMALIZE);
}

/* ---- X11 presentation (CPU copy) ----------------------------------------- */
static Display *dpy; static Window win; static GC gc; static XImage *image; static Atom wm_delete;
static int x_error_code;
static int on_x_error(Display *d, XErrorEvent *e) {
    (void)d; x_error_code = e->error_code;
    say("GLGEARS_X11_ERROR code=%u request=%u minor=%u\n", e->error_code, e->request_code, e->minor_code);
    return 0;
}
static int on_x_io_error(Display *d) { (void)d; say("GLGEARS_X11_LOST display connection closed\n"); _exit(4); return 0; }
static int read_rgba; /* -readrgba: pixels are RGBA, swap R/B while copying */
static int check; /* -check: print a colour census of one frame at angle 0 */
static int direct_bgra; /* XImage is 32bpp LSBFirst with 0xff0000/0xff00/0xff masks: copy BGRA rows as-is */
static int egl_window; /* -window: EGL window surface + eglSwapBuffers; no GC/XImage */
/* Optional link-time X round-trip counters (see the kopper link's present guard). */
extern void guest_xcb_stats(unsigned *round_trips, double *seconds) __attribute__((weak));
extern void guest_xcb_report(char *out, unsigned size) __attribute__((weak));

static int x11_open(int w, int h) {
    XSetErrorHandler(on_x_error); XSetIOErrorHandler(on_x_io_error);
    const char *name = getenv("DISPLAY"); if (!name || !*name) name = ":0";
    for (int attempt = 1; attempt <= 60 && !dpy; attempt++) {
        dpy = XOpenDisplay(name);
        if (!dpy) { if (attempt == 1 || attempt % 10 == 0) say("GLGEARS_X11_RETRY display=%s attempt=%d\n", name, attempt); sleep(1); }
    }
    if (!dpy) return 0;
    int screen = DefaultScreen(dpy); Visual *visual = DefaultVisual(dpy, screen);
    if (visual->class != TrueColor) { say("GLGEARS_X11_UNSUPPORTED visual class=%d\n", visual->class); return 0; }
    int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
    int x = sw > w + 64 ? sw - w - 32 : 0, y = sh > h + 96 ? 48 : 0;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), x, y, w, h, 0, 0, BlackPixel(dpy, screen));
    if (!win) return 0;
    XStoreName(dpy, win, "glxgears");
    XClassHint cls = { .res_name = "glgears", .res_class = "Glgears" }; XSetClassHint(dpy, win, &cls);
    XSizeHints *hints = XAllocSizeHints();
    if (hints) { hints->flags = PMinSize | PMaxSize | PPosition | USPosition; hints->x = x; hints->y = y;
        hints->min_width = hints->max_width = w; hints->min_height = hints->max_height = h;
        XSetWMNormalHints(dpy, win, hints); XFree(hints); }
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False); XSetWMProtocols(dpy, win, &wm_delete, 1);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask);
    if (egl_window) {
        XMapRaised(dpy, win); XSync(dpy, False);
        say("GLGEARS_X11_OPEN display=%s screen=%dx%d window=%dx%d+%d+%d depth=%d egl_window=1\n",
            name, sw, sh, w, h, x, y, DefaultDepth(dpy, screen));
        return !x_error_code;
    }
    gc = XCreateGC(dpy, win, 0, NULL); if (!gc) return 0;
    image = XCreateImage(dpy, visual, DefaultDepth(dpy, screen), ZPixmap, 0, NULL, w, h, 32, 0);
    if (!image || image->bytes_per_line <= 0) return 0;
    image->data = calloc((size_t)h, (size_t)image->bytes_per_line); if (!image->data) return 0;
    direct_bgra = image->bits_per_pixel == 32 && image->byte_order == LSBFirst && image->red_mask == 0xff0000 &&
                  image->green_mask == 0xff00 && image->blue_mask == 0xff;
    XMapRaised(dpy, win); XSync(dpy, False);
    say("GLGEARS_X11_OPEN display=%s screen=%dx%d window=%dx%d+%d+%d depth=%d bpp=%d direct_bgra=%d\n",
        name, sw, sh, w, h, x, y, DefaultDepth(dpy, screen), image->bits_per_pixel, direct_bgra);
    return !x_error_code;
}
/* Returns 0 when the window was closed. */
static int x11_events(void) {
    while (XPending(dpy)) {
        XEvent e; XNextEvent(dpy, &e);
        if (e.type == DestroyNotify) return 0;
        if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete) return 0;
        if (e.type == KeyPress) { KeySym k = XLookupKeysym(&e.xkey, 0); if (k == 'q' || k == 0xff1b) return 0; }
    }
    return 1;
}
static unsigned long component(unsigned v, unsigned long mask) {
    unsigned shift = 0; if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; shift++; }
    return (unsigned long)(((uint64_t)v * mask + 127) / 255) << shift;
}
/* bgra: bottom-up rows from glReadPixels(GL_BGRA, GL_UNSIGNED_BYTE), or
 * GL_RGBA with -readrgba. */
static void x11_present(const unsigned char *bgra, int w, int h) {
    for (int y = 0; y < h; y++) {
        const unsigned char *src = bgra + (size_t)(h - 1 - y) * w * 4;
        if (direct_bgra && !read_rgba) { memcpy(image->data + (size_t)y * image->bytes_per_line, src, (size_t)w * 4); continue; }
        if (direct_bgra) {
            unsigned char *dst = (unsigned char *)image->data + (size_t)y * image->bytes_per_line;
            for (int x = 0; x < w * 4; x += 4) { dst[x] = src[x + 2]; dst[x + 1] = src[x + 1]; dst[x + 2] = src[x]; dst[x + 3] = src[x + 3]; }
            continue;
        }
        const int r = read_rgba ? 0 : 2, b = read_rgba ? 2 : 0;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = src + x * 4;
            XPutPixel(image, x, y, component(p[r], image->red_mask) | component(p[1], image->green_mask) | component(p[b], image->blue_mask));
        }
    }
    XPutImage(dpy, win, gc, image, 0, 0, 0, 0, w, h);
    XSync(dpy, False);
}
static void x11_close(void) {
    if (!dpy) return;
    if (image) { XDestroyImage(image); image = NULL; }
    if (gc) XFreeGC(dpy, gc);
    if (win) XDestroyWindow(dpy, win);
    XSync(dpy, False); XCloseDisplay(dpy); dpy = NULL;
}

/* ---- main ---------------------------------------------------------------- */
static const char *driver_name(void) {
    const char *o = getenv("MESA_LOADER_DRIVER_OVERRIDE");
    if (o && *o) return o;
    const char *g = getenv("GALLIUM_DRIVER");
    if (g && *g) return g;
    const char *s = getenv("LIBGL_ALWAYS_SOFTWARE");
    return s && *s && strcmp(s, "0") && strcmp(s, "false") ? "swrast-default" : "default";
}

static void usage(void) {
    say("usage: glgears [-nopresent|-window] [-seconds N] [-size WxH] [-info] [-interval S] [-readrgba] [-check]\n");
}

int main(int argc, char **argv) {
    int present = 1, info = 0, w = 300, h = 300;
    double seconds = 0, interval = 5.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-nopresent")) present = 0;
        else if (!strcmp(argv[i], "-info")) info = 1;
        else if (!strcmp(argv[i], "-window")) egl_window = 1;
        else if (!strcmp(argv[i], "-readrgba")) read_rgba = 1;
        else if (!strcmp(argv[i], "-check")) check = 1;
        else if (!strcmp(argv[i], "-seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-interval") && i + 1 < argc) interval = atof(argv[++i]);
        else if (!strcmp(argv[i], "-size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 4096 || h > 4096) { usage(); return 1; }
        } else { usage(); return 1; }
    }
    if (interval <= 0) interval = 5.0;
    if (egl_window) present = 1;
    const char *driver = driver_name();
    say("GLGEARS_START driver=%s present=%d window=%d size=%dx%d seconds=%g read=%s\n", driver, present, egl_window, w, h, seconds, read_rgba ? "rgba" : "bgra");
    if (egl_window && !x11_open(w, h)) { say("GLGEARS_ERROR X11 open failed\n"); return 4; }

    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = egl_window ? (gpd ? gpd(EGL_PLATFORM_X11_KHR, dpy, NULL) : EGL_NO_DISPLAY) :
        gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
        say("GLGEARS_ERROR eglInitialize failed egl=0x%x driver=%s\n", eglGetError(), driver); return 2;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) { say("GLGEARS_ERROR eglBindAPI(EGL_OPENGL_API) failed egl=0x%x\n", eglGetError()); return 2; }
    EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLint wincfgattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE };
    EGLConfig cfg = EGL_NO_CONFIG_KHR; EGLint n = 0;
    if (!eglChooseConfig(d, egl_window ? wincfgattr : cfgattr, &cfg, 1, &n) || n < 1) cfg = EGL_NO_CONFIG_KHR;
    if (egl_window && cfg == EGL_NO_CONFIG_KHR) { say("GLGEARS_ERROR no EGL window config egl=0x%x\n", eglGetError()); return 2; }
    /* Compatibility profile (fixed function + display lists), as glxgears' legacy context. */
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) { say("GLGEARS_ERROR eglCreateContext failed egl=0x%x configs=%d\n", eglGetError(), n); return 2; }
    EGLSurface surf = EGL_NO_SURFACE;
    if (egl_window) {
        surf = eglCreateWindowSurface(d, cfg, (EGLNativeWindowType)win, NULL);
        if (surf == EGL_NO_SURFACE) { say("GLGEARS_ERROR eglCreateWindowSurface failed egl=0x%x\n", eglGetError()); return 2; }
    }
    if (!eglMakeCurrent(d, surf, surf, ctx)) { say("GLGEARS_ERROR eglMakeCurrent failed egl=0x%x\n", eglGetError()); return 2; }
    if (!load_gl()) return 3;

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    if (!renderer) renderer = "?";
    if (info) {
        say("EGL_VERSION = %d.%d (%s)\n", major, minor, eglQueryString(d, EGL_VENDOR));
        say("GL_RENDERER   = %s\n", renderer);
        say("GL_VERSION    = %s\n", version ? version : "?");
        say("GL_VENDOR     = %s\n", vendor ? vendor : "?");
    }

    if (egl_window) eglSwapInterval(d, 0);
    GLuint fbo = 0, rb[2] = { 0, 0 };
    if (!egl_window) {
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) { say("GLGEARS_ERROR framebuffer incomplete 0x%x\n", status); return 3; }
    }

    init();
    reshape(w, h);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) { say("GLGEARS_ERROR GL error 0x%x after init\n", err); return 3; }

    unsigned char *pixels = NULL;
    if ((present && !egl_window) || check) {
        pixels = malloc((size_t)w * h * 4);
        if (!pixels) { say("GLGEARS_ERROR out of memory\n"); return 4; }
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    if (present && !egl_window && !x11_open(w, h)) { say("GLGEARS_ERROR X11 open failed\n"); return 4; }
    say("GLGEARS_READY driver=%s renderer=\"%s\" version=\"%s\" present=%d size=%dx%d\n",
        driver, renderer, version ? version : "?", present, w, h);

    if (check) {
        /* Census of the angle-0 frame, comparable across drivers: pixels whose
         * dominant channel is red/green/blue (the three gears), black, other. */
        draw();
        glReadPixels(0, 0, w, h, read_rgba ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        long red = 0, green = 0, blue = 0, black = 0, other = 0;
        for (long n = 0; n < (long)w * h; n++) {
            const unsigned char *q = pixels + n * 4;
            int r = q[read_rgba ? 0 : 2], g = q[1], b = q[read_rgba ? 2 : 0];
            if (r < 16 && g < 16 && b < 16) black++;
            else if (r > g + 16 && r > b + 16) red++;
            else if (g > r + 16 && g > b + 16) green++;
            else if (b > r + 16 && b > g + 16) blue++;
            else other++;
        }
        err = glGetError();
        say("GLGEARS_CHECK driver=%s size=%dx%d red=%ld green=%ld blue=%ld black=%ld other=%ld glerr=0x%x\n",
            driver, w, h, red, green, blue, black, other, err);
    }
    double t_start = now_s(), t_last = t_start, t_rot = t_start;
    int frames = 0, total = 0;
    double swap_s = 0;
    for (;;) {
        double t = now_s();
        /* advance rotation for next frame (glxgears: 70 degrees per second) */
        angle += 70.0 * (t - t_rot);
        t_rot = t;
        if (angle > 3600.0) angle -= 3600.0;

        draw();
        if (egl_window) {
            double swap_t = now_s();
            if (!eglSwapBuffers(d, surf)) { say("GLGEARS_ERROR eglSwapBuffers failed egl=0x%x\n", eglGetError()); break; }
            swap_s += now_s() - swap_t;
            if (!x11_events()) break;
        } else if (present) {
            glReadPixels(0, 0, w, h, read_rgba ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE, pixels);
            x11_present(pixels, w, h);
            if (!x11_events()) break;
        } else {
            glFinish();
        }
        frames++; total++;

        t = now_s();
        if (t - t_last >= interval) {
            double secs = t - t_last, fps = frames / secs;
            say("%d frames in %3.1f seconds = %6.3f FPS\n", frames, secs, fps);
            say("GLGEARS_FPS driver=%s renderer=\"%s\" fps=%.3f frames=%d seconds=%.2f present=%d size=%dx%d\n",
                driver, renderer, fps, frames, secs, present, w, h);
            if (egl_window) {
                unsigned trips = 0; double trip_s = 0;
                if (guest_xcb_stats) guest_xcb_stats(&trips, &trip_s);
                say("GLGEARS_SWAP avg_ms=%.2f x_round_trips=%u x_ms=%.1f\n", 1000 * swap_s / frames, trips, 1000 * trip_s);
                swap_s = 0;
                if (guest_xcb_report) { char report[1024]; guest_xcb_report(report, sizeof report); say("GLGEARS_XREQ %s\n", report); }
            }
            t_last = t; frames = 0;
            err = glGetError();
            if (err != GL_NO_ERROR) say("GLGEARS_GL_ERROR 0x%x\n", err);
        }
        if (seconds > 0 && t - t_start >= seconds) break;
    }
    double elapsed = now_s() - t_start;
    say("GLGEARS_DONE driver=%s renderer=\"%s\" frames=%d seconds=%.2f avg_fps=%.3f present=%d size=%dx%d\n",
        driver, renderer, total, elapsed, elapsed > 0 ? total / elapsed : 0.0, present, w, h);
    if (present && !egl_window) x11_close();
    free(pixels);
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(d, surf);
    eglDestroyContext(d, ctx);
    eglTerminate(d);
    if (egl_window) x11_close();
    return 0;
}
