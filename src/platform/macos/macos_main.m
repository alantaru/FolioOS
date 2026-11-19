/* FOLIO v4 — macOS simulator (development/tests). Same core as the UEFI app; a port of
 * src/platform/win32/win32_main.c, function by function.
 *   folio_sim [--root DIR] [--vol2 DIR] [--size WxH] [--script FILE --out DIR]
 * With --script it runs headless (no window, no NSApplication) for the automated tests.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <os/lock.h>

/* folio.h names that collide with the system headers: MacTypes.h has its own Rect (the core's
 * typedef is renamed in this file only; typedef names have no linkage), Foundation defines MIN/MAX,
 * and with _FORTIFY_SOURCE <string.h> turns memcpy/memmove/memset into macros (restored after) */
#undef MIN
#undef MAX
#undef RGB
#define Rect FolioRect
#pragma push_macro("memcpy")
#pragma push_macro("memmove")
#pragma push_macro("memset")
#undef memcpy
#undef memmove
#undef memset
#include "../../core/folio.h"
#pragma pop_macro("memcpy")
#pragma pop_macro("memmove")
#pragma pop_macro("memset")

static Surface screen;
static char roots[4][PATH_MAX]; static int nroots = 1;
static Event q[2048]; static int qh, qt;
static bool headless; static u64 fake_ms;
static struct timespec t0;

static void push(Event *e) { int n = (qt + 1) % 2048; if (n == qh) return; q[qt] = *e; qt = n; }

/* GUI state */
@class FolioView;
static NSWindow *win;
static FolioView *view;
static CGFloat bscale = 1;
static bool loop_started;           /* app_init done: the core is in app_run's loop */
static bool live_render;            /* sync_size is drawing a frame itself (app_frame + pal_present) */
static bool presenting;             /* present_now is handing the image to AppKit (may run layout) */
static CGColorSpaceRef srgb;

/* ------------------------------------------------------------ PAL: screen/time */
static void present_now(void);
static void sync_size(void);
Surface *pal_screen(void) { return &screen; }
void pal_present(Rect r) { (void)r; if (!headless) present_now(); }
static double mono_ms(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}
u64 pal_ms(void) {
    if (headless) return fake_ms;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)mono_ms(&t0, &t);
}
void pal_now(DateTime *t) {
    time_t now = time(0); struct tm s; localtime_r(&now, &s);
    t->year = s.tm_year + 1900; t->month = s.tm_mon + 1; t->day = s.tm_mday; t->hour = s.tm_hour; t->min = s.tm_min; t->sec = s.tm_sec;
}
const char *pal_name(void) { return "macOS simulator"; }
/* app.c sets its own quit flag right after this call; EV_QUIT is the analogue of PostQuitMessage */
void pal_power(int a) { (void)a; Event e = { 0 }; e.type = EV_QUIT; push(&e); }
void pal_set_pointer_speed(float s) { (void)s; }
void pal_set_touch_mode(int m) { (void)m; }
void pal_audio_set_gpio(int on) { (void)on; }
void pal_audio_probe(void) {}
void pal_video_fast(bool on) { (void)on; }
void pal_set_rotation(int pref) { (void)pref; }
int  pal_rotation(void) { return 0; }
void pal_set_video_mode(const char *pref) { (void)pref; }
void pal_set_aspect(int w, int h) { (void)w; (void)h; }
void pal_video_apply(void) {}
void pal_video_mode(int *w, int *h) { *w = screen.w; *h = screen.h; }
int  pal_video_modes(int *ws, int *hs, int max) { if (max < 1) return 0; ws[0] = screen.w; hs[0] = screen.h; return 1; }
/* the surface is in backing pixels: on Retina report the dpi that makes auto_scale() (110 dpi = 100%)
 * give the same apparent size per point as a 1x screen */
float pal_display_dpi(void) { if (headless || bscale <= 1) return 0; return 110.0f * (float)bscale; }
void pal_log(const char *s) { fputs(s, stderr); }
void pal_diag(Buf *o) { buf_printf(o, "macOS simulator (no direct hardware access)\r\nAudio: %s\r\n", pal_audio_info()); }
static int ns_mods(NSEventModifierFlags f) {
    int m = 0;
    if (f & NSEventModifierFlagShift) m |= MOD_SHIFT;
    if (f & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) m |= MOD_CTRL;   /* Cmd+S works like Ctrl+S */
    if (f & NSEventModifierFlagOption) m |= MOD_ALT;
    return m;
}
int pal_mods(void) {
    if (headless) return 0;
    return ns_mods([NSEvent modifierFlags]);
}

bool pal_poll(Event *e) {
    if (!headless) {
        loop_started = true;
        /* safety net: a geometry change that reached no hook (or was deferred because it arrived
         * while a frame was being drawn/presented) is picked up here, between frames */
        sync_size();
        @autoreleasepool {
            NSEvent *ev;
            while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode dequeue:YES]))
                [NSApp sendEvent:ev];
        }
    }
    if (qh == qt) return false;
    *e = q[qh]; qh = (qh + 1) % 2048;
    return true;
}
void pal_wait(int ms) {
    if (headless) { fake_ms += (u64)(ms > 0 ? ms : 1); return; }   /* roteiro: o tempo falso anda (a espera do seletor de boot termina) */
    @autoreleasepool {
        NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:ms / 1000.0]
                                            inMode:NSDefaultRunLoopMode dequeue:YES];
        if (ev) [NSApp sendEvent:ev];
    }
}

/* ------------------------------------------------------------ PAL: files (folders as volumes) */
static bool to_p(int vol, const char *path, char *out, size_t cap) {
    if (vol < 0 || vol >= nroots) return false;
    snprintf(out, cap, "%s%s", roots[vol], path);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    return true;
}
/* discos: o simulador nunca toca nos discos do Mac */
int pal_disk_count(void) { return 0; }
bool pal_disk_info(int k, DiskInfo *o) { (void)k; (void)o; return false; }
bool pal_disk_io(int k, u64 lba, u32 n, void *buf, bool w) { (void)k; (void)lba; (void)n; (void)buf; (void)w; return false; }
bool pal_disk_flush(int k) { (void)k; return false; }
bool pal_boot_entry_add(const char *desc, int part_num, u64 part_lba, u64 part_count, const u8 part_guid[16], const char *path) {
    (void)desc; (void)part_num; (void)part_lba; (void)part_count; (void)part_guid; (void)path; return false;
}
bool pal_boot_entry_remove(const u8 part_guid[16]) { (void)part_guid; return false; }
bool pal_chain_mbr(const u8 *code) { (void)code; return false; }
bool pal_boot_other(char *name, int cap) { (void)name; (void)cap; return false; }
void pal_boot_other_start(void) {}

int pal_vol_count(void) { return nroots; }
bool pal_vol_info(int v, VolInfo *o) {
    if (v < 0 || v >= nroots) return false;
    memset(o, 0, sizeof *o);
    o->boot = v == 0;
    snprintf(o->label, sizeof o->label, v == 0 ? "FOLIO" : "BACKUP");
    o->removable = v != 0;                           /* no simulador, o segundo volume faz o papel do pendrive */
    return true;
}
static void tm_to_dt(time_t t, DateTime *d) {
    struct tm s; localtime_r(&t, &s);
    d->year = s.tm_year + 1900; d->month = s.tm_mon + 1; d->day = s.tm_mday; d->hour = s.tm_hour; d->min = s.tm_min; d->sec = s.tm_sec;
}
int pal_list(int vol, const char *path, FileEntry **out) {
    char p[PATH_MAX];
    *out = 0;
    if (!to_p(vol, path, p, sizeof p)) return -1;
    DIR *d = opendir(p);
    if (!d) return -1;
    int n = 0, cap = 64;
    FileEntry *l = mem_alloc(sizeof(FileEntry) * (usize)cap);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n >= cap) { cap *= 2; l = mem_realloc(l, sizeof(FileEntry) * (usize)cap); }
        FileEntry *e = &l[n];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char fp[PATH_MAX]; struct stat st;
        snprintf(fp, sizeof fp, "%s/%s", p, de->d_name);
        if (stat(fp, &st) == 0) {
            e->dir = S_ISDIR(st.st_mode);
            e->size = e->dir ? 0 : (u64)st.st_size;
            tm_to_dt(st.st_mtime, &e->mtime);
        } else e->dir = de->d_type == DT_DIR;
        n++;
    }
    closedir(d);
    *out = l;
    return n;
}
bool pal_read(int vol, const char *path, u8 **data, usize *len) {
    char p[PATH_MAX]; if (!to_p(vol, path, p, sizeof p)) return false;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { close(fd); return false; }
    usize sz = (usize)st.st_size, rd = 0;
    u8 *b = mem_alloc(sz + 1);
    while (rd < sz) { ssize_t k = read(fd, b + rd, sz - rd); if (k <= 0) break; rd += (usize)k; }
    close(fd);
    b[rd] = 0; *data = b; *len = rd;
    return true;
}
bool pal_write(int vol, const char *path, const void *data, usize len) {
    char p[PATH_MAX], t[PATH_MAX + 8];
    if (!to_p(vol, path, p, sizeof p)) return false;
    snprintf(t, sizeof t, "%s.tmp", p);
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const u8 *s = data; usize wr = 0;
    while (wr < len) { ssize_t k = write(fd, s + wr, len - wr); if (k <= 0) break; wr += (usize)k; }
    fsync(fd); close(fd);
    if (wr != len) { unlink(t); return false; }
    if (rename(t, p) != 0) { unlink(t); return false; }
    return true;
}
bool pal_mkdir(int vol, const char *path) { char p[PATH_MAX]; return to_p(vol, path, p, sizeof p) && mkdir(p, 0755) == 0; }
bool pal_remove(int vol, const char *path) {
    char p[PATH_MAX]; struct stat st;
    if (!to_p(vol, path, p, sizeof p) || lstat(p, &st) != 0) return false;
    return S_ISDIR(st.st_mode) ? rmdir(p) == 0 : unlink(p) == 0;
}
bool pal_rename(int vol, const char *path, const char *nn) {
    char np[1024]; snprintf(np, sizeof np, "%s", path);
    char *slash = strrchr(np, '/'); if (slash) slash[1] = 0; else np[0] = 0;
    strncat(np, nn, sizeof np - strlen(np) - 1);
    char a[PATH_MAX], b[PATH_MAX]; struct stat sa, sb;
    if (!to_p(vol, path, a, sizeof a) || !to_p(vol, np, b, sizeof b)) return false;
    if (lstat(a, &sa) != 0) return false;
    /* like MoveFileW: fails if the target exists, unless it is the same file (case-only rename
     * on a case-insensitive volume) */
    if (lstat(b, &sb) == 0 && !(sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino)) return false;
    return rename(a, b) == 0;
}
bool pal_exists(int vol, const char *path, bool *is_dir) {
    char p[PATH_MAX]; struct stat st;
    if (!to_p(vol, path, p, sizeof p) || stat(p, &st) != 0) return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
}
struct PalFile { int fd; };
PalFile *pal_fopen(int vol, const char *path, u64 *size) {
    char p[PATH_MAX]; if (!to_p(vol, path, p, sizeof p)) return 0;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { close(fd); return 0; }
    *size = (u64)st.st_size;
    PalFile *f = mem_alloc(sizeof *f); f->fd = fd; return f;
}
int pal_fread(PalFile *f, void *b, int n) { ssize_t r = read(f->fd, b, (size_t)n); return r < 0 ? -1 : (int)r; }
bool pal_fseek(PalFile *f, u64 p) { return lseek(f->fd, (off_t)p, SEEK_SET) >= 0; }
void pal_fclose(PalFile *f) { if (f) { close(f->fd); mem_free(f); } }

/* ------------------------------------------------------------ PAL: audio (AudioQueue) */
#define NBUF 8
#define BUFF 2048
#define RING (NBUF * BUFF)          /* same capacity as the win32 waveOut buffers */
#define AQ_NBUF 3
#define AQ_FRAMES 1024
static AudioQueueRef aq; static AudioQueueBufferRef aqb[AQ_NBUF];
static i16 ring[RING * 2]; static int rpos, rfill, arate;
static os_unfair_lock alock = OS_UNFAIR_LOCK_INIT;

static void aq_fill(void *ud, AudioQueueRef queue, AudioQueueBufferRef b) {
    (void)ud;
    int want = (int)(b->mAudioDataBytesCapacity / 4), got = 0;
    i16 *dst = b->mAudioData;
    os_unfair_lock_lock(&alock);
    while (got < want && rfill > 0) {
        int k = MIN(want - got, MIN(rfill, RING - rpos));
        memcpy(dst + got * 2, ring + rpos * 2, (size_t)k * 4);
        got += k; rfill -= k; rpos = (rpos + k) % RING;
    }
    os_unfair_lock_unlock(&alock);
    if (got < want) memset(dst + got * 2, 0, (size_t)(want - got) * 4);   /* underrun: silence */
    b->mAudioDataByteSize = (UInt32)want * 4;
    AudioQueueEnqueueBuffer(queue, b, 0, 0);
}
bool pal_audio_start(int rate) {
    if (headless) return false;
    pal_audio_stop();
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBytesPerPacket = 4; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 16;
    os_unfair_lock_lock(&alock); rpos = rfill = 0; os_unfair_lock_unlock(&alock);
    if (AudioQueueNewOutput(&f, aq_fill, 0, 0, 0, 0, &aq) != noErr) { aq = 0; return false; }
    for (int i = 0; i < AQ_NBUF; i++) {
        if (AudioQueueAllocateBuffer(aq, AQ_FRAMES * 4, &aqb[i]) != noErr) { AudioQueueDispose(aq, true); aq = 0; return false; }
        aq_fill(0, aq, aqb[i]);
    }
    if (AudioQueueStart(aq, 0) != noErr) { AudioQueueDispose(aq, true); aq = 0; return false; }
    arate = rate;
    return true;
}
int pal_audio_rate(void) { return arate; }
int pal_audio_space(void) {
    if (!aq) return 0;
    os_unfair_lock_lock(&alock); int n = RING - rfill; os_unfair_lock_unlock(&alock);
    return n;
}
void pal_audio_write(const i16 *fr, int n) {
    if (!aq || n <= 0) return;
    os_unfair_lock_lock(&alock);
    n = MIN(n, RING - rfill);
    while (n > 0) {
        int w = (rpos + rfill) % RING;
        int k = MIN(n, RING - w);
        memcpy(ring + w * 2, fr, (size_t)k * 4);
        rfill += k; fr += k * 2; n -= k;
    }
    os_unfair_lock_unlock(&alock);
}
void pal_audio_stop(void) {
    if (!aq) return;
    AudioQueueStop(aq, true);
    AudioQueueDispose(aq, true);
    aq = 0;
    os_unfair_lock_lock(&alock); rpos = rfill = 0; os_unfair_lock_unlock(&alock);
}
const char *pal_audio_info(void) { return headless ? "no audio (test mode)" : "macOS AudioQueue (simulator)"; }

/* ------------------------------------------------------------ window */
static int map_keycode(unsigned short kc) {
    switch (kc) {
    case 0x35: return KEY_ESC; case 0x24: case 0x4C: return KEY_ENTER; case 0x30: return KEY_TAB;
    case 0x33: return KEY_BACKSPACE; case 0x75: return KEY_DELETE; case 0x72: return KEY_INSERT;
    case 0x7B: return KEY_LEFT; case 0x7C: return KEY_RIGHT; case 0x7E: return KEY_UP; case 0x7D: return KEY_DOWN;
    case 0x73: return KEY_HOME; case 0x77: return KEY_END; case 0x74: return KEY_PGUP; case 0x79: return KEY_PGDN;
    case 0x7A: return KEY_F1; case 0x78: return KEY_F2; case 0x63: return KEY_F3; case 0x76: return KEY_F4;
    case 0x60: return KEY_F5; case 0x61: return KEY_F6; case 0x62: return KEY_F7; case 0x64: return KEY_F8;
    case 0x65: return KEY_F9; case 0x6D: return KEY_F10; case 0x67: return KEY_F11; case 0x6F: return KEY_F12;
    default: return 0;
    }
}
/* digit row by position (kVK_ANSI_1…9: same keys on ANSI, ISO and JIS boards), 0 if not 1…9 */
static int digit_keycode(unsigned short kc) {
    static const unsigned short d[9] = { 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19 };
    for (int i = 0; i < 9; i++) if (kc == d[i]) return '1' + i;
    return 0;
}
static void resize_surface(int w, int h) {
    free(screen.px);
    screen.w = w; screen.h = h; screen.stride = w;
    screen.px = calloc((size_t)w * h, 4);
}

static unsigned held_btns;          /* buttons held (bit 0 left, 1 right, 2 middle) */
static void release_held(void) {
    for (int b = 0; b < 3; b++) if (held_btns & (1u << b)) { Event u = { 0 }; u.type = EV_UP; u.button = b; push(&u); }
    held_btns = 0;
}

/* pixel size of the view (points × backing scale) changed: same rule as WM_SIZE. Called from the
 * view's own geometry changes, the window notifications and every pal_poll, so the Surface always
 * matches the view (entering fullscreen with a full-height window does not change the window frame
 * and posts no windowDidResize: only the content view grows by the title bar height). */
static void sync_size(void) {
    if (!view || !win) return;
    /* never free the surface under a frame being drawn or presented (AppKit can lay out, and so
     * resize the view, from inside displayIfNeeded): the next pal_poll compares again */
    if (live_render || presenting) return;
    bscale = win.backingScaleFactor > 0 ? win.backingScaleFactor : 1;
    NSSize sz = ((NSView *)view).bounds.size;
    int w = (int)(sz.width * bscale + 0.5), h = (int)(sz.height * bscale + 0.5);
    if (w > 64 && h > 64 && (w != screen.w || h != screen.h)) {
        resize_surface(w, h); app_request_redraw();
        /* AppKit's live resize runs its own loop: draw from here so the content follows the edge */
        if (loop_started) {
            live_render = true;
            app_frame();
            pal_present(rect(0, 0, screen.w, screen.h));
            live_render = false;
        }
    }
}

@interface FolioView : NSView <NSTextInputClient> {
@public
    CGImageRef image;
    NSCursor *blank;
    NSString *marked;
}
@end

@implementation FolioView
- (instancetype)initWithFrame:(NSRect)r {
    if ((self = [super initWithFrame:r])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
        self.layerContentsPlacement = NSViewLayerContentsPlacementScaleAxesIndependently;
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:0 pixelsWide:1 pixelsHigh:1 bitsPerSample:8
                                    samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:4 bitsPerPixel:32];
        memset(rep.bitmapData, 0, 4);
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        [img addRepresentation:rep];
        blank = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
        [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
            options:NSTrackingCursorUpdate | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil]];
    }
    return self;
}
- (void)dealloc { if (image) CGImageRelease(image); }
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
/* the image always covers the view exactly (resize gravity: no anchor corner, so nothing depends on
 * whether the layer geometry is flipped). sync_size keeps the Surface at the view's pixel size, so
 * this is 1:1; during the instant a resize is still pending it stretches, and ev_pos maps the
 * pointer with the same proportion, so pointer and content never drift apart. Everything is set
 * here, together with the contents. */
- (void)updateLayer {
    CALayer *l = self.layer;
    l.contentsGravity = kCAGravityResize;
    l.magnificationFilter = kCAFilterNearest;
    l.minificationFilter = kCAFilterNearest;
    l.contentsScale = bscale;
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    l.contents = (__bridge id)image;
}
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; sync_size(); }
- (void)viewDidMoveToWindow { [super viewDidMoveToWindow]; sync_size(); }
- (void)setFrameSize:(NSSize)s {
    [super setFrameSize:s];
    [self.window invalidateCursorRectsForView:self];   /* the hidden-cursor rect follows the new bounds */
    sync_size();
}

/* the core draws its own pointer: the system cursor is hidden over the content (WM_SETCURSOR) */
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:blank]; }
- (void)cursorUpdate:(NSEvent *)e { [blank set]; }

/* ---- pointer: surface pixels, y from the top (flipped view). Scaled by the view's bounds at event
 * time, not a cached backing scale: the image is stretched over the bounds, so this matches it
 * even before a pending resize has been applied */
static void ev_pos(NSView *v, NSEvent *ev, Event *e) {
    NSPoint p = [v convertPoint:ev.locationInWindow fromView:nil];
    NSSize b = v.bounds.size;
    e->x = b.width > 0 ? (int)floor(p.x * screen.w / b.width) : 0;
    e->y = b.height > 0 ? (int)floor(p.y * screen.h / b.height) : 0;
}
- (void)mouseMoved:(NSEvent *)ev {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    if (!NSPointInRect(p, self.bounds)) return;     /* like WM_MOUSEMOVE: client area only */
    Event e = { 0 }; e.type = EV_MOVE; ev_pos(self, ev, &e); push(&e);
}
- (void)dragged:(NSEvent *)ev { Event e = { 0 }; e.type = EV_MOVE; ev_pos(self, ev, &e); push(&e); }
- (void)mouseDragged:(NSEvent *)ev { [self dragged:ev]; }
- (void)rightMouseDragged:(NSEvent *)ev { [self dragged:ev]; }
- (void)otherMouseDragged:(NSEvent *)ev { [self dragged:ev]; }
- (void)button:(NSEvent *)ev down:(bool)down which:(int)b {
    Event e = { 0 };
    e.button = b;
    if (down) held_btns |= 1u << b; else held_btns &= ~(1u << b);
    e.type = down ? EV_DOWN : EV_UP; ev_pos(self, ev, &e); push(&e);
}
- (void)mouseDown:(NSEvent *)ev { [self button:ev down:true which:0]; }
- (void)mouseUp:(NSEvent *)ev { [self button:ev down:false which:0]; }
- (void)rightMouseDown:(NSEvent *)ev { [self button:ev down:true which:1]; }
- (void)rightMouseUp:(NSEvent *)ev { [self button:ev down:false which:1]; }
- (void)otherMouseDown:(NSEvent *)ev { [self button:ev down:true which:2]; }
- (void)otherMouseUp:(NSEvent *)ev { [self button:ev down:false which:2]; }

/* ---- wheel: same accumulator as WM_MOUSEWHEEL. Trackpads send fractions of a notch (and inertia after
 * the finger leaves); a gesture's remainder expires after 400 ms so it does not become a notch later. */
- (void)scrollWheel:(NSEvent *)ev {
    static double acc, last_t;
    double d, notch;
    if (ev.hasPreciseScrollingDeltas) { d = ev.scrollingDeltaY; notch = 40.0; }
    else { d = ev.deltaY; notch = 1.0; if (d != 0 && fabs(d) < 1.0) d = d > 0 ? 1.0 : -1.0; }
    if (d == 0) return;
    double t = ev.timestamp;
    if (t - last_t > 0.4) acc = 0;
    last_t = t;
    if ((d > 0) != (acc > 0)) acc = 0;
    acc += d;
    while (acc >= notch || acc <= -notch) {
        Event e = { 0 }; e.type = EV_WHEEL; e.wheel = acc > 0 ? 1 : -1; push(&e);
        acc -= e.wheel * notch;
    }
}

/* ---- keyboard. Special keys and Ctrl/Cmd chords (WM_KEYDOWN); returns NO if not consumed */
static BOOL handle_key(NSEvent *ev) {
    NSEventModifierFlags f = ev.modifierFlags;
    int mods = ns_mods(f);
    Event e = { 0 };
    int k = map_keycode(ev.keyCode);
    if (k) { e.type = EV_KEY; e.key = k; e.mods = mods; push(&e); return YES; }
    if (f & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) {
        u32 ch = 0;
        for (int pass = 0; pass < 2 && !ch; pass++) {
            NSString *s = pass == 0 ? ev.charactersIgnoringModifiers : [ev charactersByApplyingModifiers:0];
            u32 c = s.length ? [s characterAtIndex:0] : 0;
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            if (c == '+') c = '=';
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '=' || c == '-') ch = c;
        }
        if (ch == 'q' && (f & NSEventModifierFlagCommand)) { e.type = EV_QUIT; push(&e); return YES; }   /* like Alt+F4 */
        /* Cmd+Option+1…9 = Alt+1…9 (go to tab N): the digit row by position, whatever the layout */
        int dig = digit_keycode(ev.keyCode);
        if (dig && (f & NSEventModifierFlagCommand) && (f & NSEventModifierFlagOption) && !(f & NSEventModifierFlagControl)) {
            e.type = EV_KEY; e.ch = (u32)dig; e.mods = mods & ~MOD_CTRL; push(&e); return YES;
        }
        if (ch) { e.type = EV_KEY; e.ch = ch; e.mods = mods; push(&e); return YES; }
        return NO;
    }
    /* Alt+letter/digit shortcuts (win32 pushes them from WM_SYSKEYDOWN). On a Mac, Option is also the
     * key that types characters: Option+digit gives | @ # on Spanish/Portuguese layouts, [ ] | { } on
     * German, # on British; Option+letters give @ \ ~ é ñ ą ł… So only the chords the core binds and
     * that cost no essential character are taken here: Option+M (Alt+M, menu) instead of µ, on the
     * key that types "m" in the current layout. Everything else keeps typing through the text input;
     * Alt+1…9 (tabs) is Cmd+Option+1…9 above (Cmd+1…6 are headings, like Ctrl+1…6), and
     * Ctrl+Tab / Ctrl+Shift+Tab still cycle tabs. */
    if ((f & NSEventModifierFlagOption) && !(f & NSEventModifierFlagShift) && !(view && view->marked)) {
        NSString *s = [ev charactersByApplyingModifiers:0];
        u32 c = s.length ? [s characterAtIndex:0] : 0;
        if (c == 'm' || c == 'M') { e.type = EV_KEY; e.ch = 'm'; e.mods = mods; push(&e); return YES; }
    }
    return NO;
}
- (BOOL)performKeyEquivalent:(NSEvent *)ev {
    if (ev.type == NSEventTypeKeyDown && self.window.isKeyWindow &&
        (ev.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) && handle_key(ev)) return YES;
    return [super performKeyEquivalent:ev];
}
- (void)keyDown:(NSEvent *)ev {
    if (handle_key(ev)) return;
    if (ev.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) return;   /* unhandled chord: no beep */
    [self interpretKeyEvents:@[ev]];       /* text: dead keys, Option combos, the system layout */
}

/* ---- NSTextInputClient (WM_CHAR) */
- (void)insertText:(id)str replacementRange:(NSRange)rr {
    NSString *s = [str isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)str string] : (NSString *)str;
    marked = nil;
    NSUInteger n = s.length;
    for (NSUInteger i = 0; i < n; i++) {
        u32 c = [s characterAtIndex:i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            u32 lo = [s characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (c < 32 || c == 127) continue;
        if (c >= 0xF700 && c <= 0xF8FF) continue;   /* function-key codepoints */
        Event e = { 0 }; e.type = EV_KEY; e.ch = c; e.mods = pal_mods() & ~(MOD_CTRL | MOD_ALT); e.us_raw = false; push(&e);
    }
}
- (void)doCommandBySelector:(SEL)sel {}
- (void)setMarkedText:(id)str selectedRange:(NSRange)sr replacementRange:(NSRange)rr {
    NSString *s = [str isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)str string] : (NSString *)str;
    marked = s.length ? [s copy] : nil;
}
- (void)unmarkText { marked = nil; }
- (NSRange)selectedRange { return NSMakeRange(0, 0); }
- (NSRange)markedRange { return marked ? NSMakeRange(0, marked.length) : NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return marked != nil; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)ar { return nil; }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)ar {
    NSRect w = [self convertRect:NSMakeRect(0, 0, 1, 1) toView:nil];
    return [self.window convertRectToScreen:w];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)p { return NSNotFound; }
@end

static void present_now(void) {
    if (!view || !screen.px) return;
    @autoreleasepool {
        size_t bytes = (size_t)screen.stride * screen.h * 4;
        CFDataRef data = CFDataCreate(0, (const UInt8 *)screen.px, (CFIndex)bytes);   /* a copy: the core keeps drawing */
        CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate((size_t)screen.w, (size_t)screen.h, 8, 32, (size_t)screen.stride * 4, srgb,
                                       kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, dp, 0, false,
                                       kCGRenderingIntentDefault);
        CGDataProviderRelease(dp); CFRelease(data);
        if (view->image) CGImageRelease(view->image);
        view->image = img;
        presenting = true;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [view setNeedsDisplay:YES];
        [view displayIfNeeded];          /* updateLayer: contents + geometry, see there */
        [CATransaction commit];
        [CATransaction flush];
        presenting = false;
    }
}

@interface FolioDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation FolioDelegate
/* never let AppKit exit(): the core's loop must end so app_run saves everything */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a {
    Event e = { 0 }; e.type = EV_QUIT; push(&e);
    return NSTerminateCancel;
}
- (BOOL)windowShouldClose:(NSWindow *)w { Event e = { 0 }; e.type = EV_QUIT; push(&e); return NO; }
- (void)windowDidResize:(NSNotification *)n { sync_size(); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n { sync_size(); }
/* fullscreen hides the title bar (the content view grows) without necessarily resizing the window */
- (void)windowDidEnterFullScreen:(NSNotification *)n { [win invalidateCursorRectsForView:view]; sync_size(); }
- (void)windowDidExitFullScreen:(NSNotification *)n { [win invalidateCursorRectsForView:view]; sync_size(); }
- (void)windowDidChangeScreen:(NSNotification *)n { sync_size(); }
/* lost focus with buttons held (Cmd+Tab, another window): release them all (WM_KILLFOCUS) */
- (void)windowDidResignKey:(NSNotification *)n { release_held(); }
- (void)applicationDidResignActive:(NSNotification *)n { release_held(); }
@end

/* ------------------------------------------------------------ PNG (for screenshots) */
static u32 crc_t[256];
static u32 crc_up(u32 c, const u8 *b, size_t n) { for (size_t i = 0; i < n; i++) c = crc_t[(c ^ b[i]) & 255] ^ (c >> 8); return c; }
static void be32(FILE *f, u32 v) { u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *t, const u8 *d, u32 n) {
    be32(f, n); fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f);
    u32 c = crc_up(0xFFFFFFFFu, (const u8 *)t, 4); c = crc_up(c, d, n); be32(f, c ^ 0xFFFFFFFFu);
}
static void write_png(const char *path, Surface *s) {
    for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_t[i] = c; }
    size_t raw_n = (size_t)s->h * (s->w * 3 + 1);
    u8 *raw = malloc(raw_n);
    for (int y = 0; y < s->h; y++) {
        u8 *r = raw + (size_t)y * (s->w * 3 + 1); r[0] = 0;
        for (int x = 0; x < s->w; x++) { u32 p = s->px[y * s->stride + x]; r[1 + x * 3] = (u8)(p >> 16); r[2 + x * 3] = (u8)(p >> 8); r[3 + x * 3] = (u8)p; }
    }
    size_t nb = (raw_n + 65534) / 65535;
    u8 *z = malloc(raw_n + nb * 5 + 6); size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    u32 a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        u32 len = (u32)MIN((size_t)65535, raw_n - off);
        z[zn++] = off + len >= raw_n ? 1 : 0;
        z[zn++] = (u8)len; z[zn++] = (u8)(len >> 8); z[zn++] = (u8)~len; z[zn++] = (u8)(~len >> 8);
        memcpy(z + zn, raw + off, len); zn += len;
    }
    u32 ad = (b << 16) | a; z[zn++] = (u8)(ad >> 24); z[zn++] = (u8)(ad >> 16); z[zn++] = (u8)(ad >> 8); z[zn++] = (u8)ad;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ih[13] = { 0 };
    ih[0] = (u8)(s->w >> 24); ih[1] = (u8)(s->w >> 16); ih[2] = (u8)(s->w >> 8); ih[3] = (u8)s->w;
    ih[4] = (u8)(s->h >> 24); ih[5] = (u8)(s->h >> 16); ih[6] = (u8)(s->h >> 8); ih[7] = (u8)s->h;
    ih[8] = 8; ih[9] = 2;
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (u32)zn); chunk(f, "IEND", 0, 0);
    fclose(f); free(raw); free(z);
}

/* ------------------------------------------------------------ script (headless tests) */
static void frame(void) { app_frame(); }
static void advance(int ms) { while (ms > 0) { int st = MIN(ms, 16); fake_ms += (u64)st; ms -= st; Event e; while (pal_poll(&e)) app_event(&e); music_update(); frame(); } }
static int parse_key(const char *s, int *mods) {
    *mods = 0;
    while (1) {
        if (!strncasecmp(s, "ctrl+", 5)) { *mods |= MOD_CTRL; s += 5; }
        else if (!strncasecmp(s, "shift+", 6)) { *mods |= MOD_SHIFT; s += 6; }
        else if (!strncasecmp(s, "alt+", 4)) { *mods |= MOD_ALT; s += 4; }
        else break;
    }
    static const struct { const char *n; int k; } t[] = {
        { "enter", KEY_ENTER }, { "esc", KEY_ESC }, { "tab", KEY_TAB }, { "backspace", KEY_BACKSPACE }, { "delete", KEY_DELETE },
        { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "up", KEY_UP }, { "down", KEY_DOWN }, { "home", KEY_HOME }, { "end", KEY_END },
        { "pgup", KEY_PGUP }, { "pgdn", KEY_PGDN },
        { "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_F3 }, { "f4", KEY_F4 }, { "f5", KEY_F5 }, { "f6", KEY_F6 },
        { "f7", KEY_F7 }, { "f8", KEY_F8 }, { "f9", KEY_F9 }, { "f10", KEY_F10 }, { "f11", KEY_F11 }, { "f12", KEY_F12 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcasecmp(s, t[i].n)) return t[i].k;
    if (!strcasecmp(s, "space")) return -(int)' ';
    return -(int)(unsigned char)s[0];   /* letter with modifier */
}
static int run_script(const char *file, const char *outdir) {
    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "roteiro não encontrado: %s\n", file); return 1; }
    char line[4096];
    app_init();
    advance(100);
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *arg = strchr(line, ' '); if (arg) *arg++ = 0; else arg = line + n;
        Event e = { 0 };
        if (!strcmp(line, "type") || !strcmp(line, "raw")) {
            int L = (int)strlen(arg);
            for (int i = 0; i < L; ) {
                u32 cp; int k = utf8_decode(arg, L, i, &cp); i += k;
                if (cp == 0xB6) { e.type = EV_KEY; e.key = KEY_ENTER; e.ch = 0; push(&e); advance(16); continue; }
                e.type = EV_KEY; e.key = 0; e.ch = cp; e.us_raw = !strcmp(line, "raw"); push(&e); advance(16);
            }
        } else if (!strcmp(line, "key")) {
            int mods; int k = parse_key(arg, &mods);
            e.type = EV_KEY; e.mods = mods;
            if (k > 0) e.key = k; else e.ch = (u32)(-k);
            push(&e); advance(30);
        } else if (!strcmp(line, "move")) { e.type = EV_MOVE; sscanf(arg, "%d %d", &e.x, &e.y); push(&e); advance(30); }
        else if (!strcmp(line, "mclick")) {        /* middle button: short click */
            int x, y; sscanf(arg, "%d %d", &x, &y);
            e.type = EV_MOVE; e.x = x; e.y = y; push(&e); advance(16);
            e.type = EV_DOWN; e.button = 2; push(&e); advance(50);
            e.type = EV_UP; push(&e); advance(60);
        } else if (!strcmp(line, "barclick")) {    /* clicks a reading-bar button by name */
            static const char *nm[BAR_COUNT] = { "anterior", "play", "devagar", "velocidade", "depressa", "proxima" };
            int i = 0; while (i < BAR_COUNT && strcmp(arg, nm[i])) i++;
            if (i >= BAR_COUNT) { fprintf(stderr, "FALHA: botão da barra desconhecido: %s\n", arg); return 2; }
            Rect r = preview_bar_button(i);
            e.type = EV_MOVE; e.x = r.x + r.w / 2; e.y = r.y + r.h / 2; push(&e); advance(16);
            e.type = EV_DOWN; e.button = 0; push(&e); advance(16);
            e.type = EV_UP; push(&e); advance(60);
        } else if (!strcmp(line, "assert_still") || !strcmp(line, "assert_moving")) {
            /* reading stays still (or moves) for N ms: measures the scroll position, not the paragraph */
            bool still = !strcmp(line, "assert_still");
            float a = preview_scroll_pos(); advance(atoi(arg)); float b = preview_scroll_pos();
            float d = b - a; if (d < 0) d = -d;
            if (still ? d > 0.01f : d < 1.0f) { fprintf(stderr, "FALHA: %s %s: a rolagem foi de %.2f a %.2f\n", line, arg, a, b); return 2; }
            printf("ok: %s (%.2f -> %.2f)\n", line, a, b);
        } else if (!strcmp(line, "assert_auto")) {
            bool want = atoi(arg) != 0;
            if (preview_auto_on() != want) { fprintf(stderr, "FALHA: rolagem automática %s\n", want ? "desligada" : "ligada"); return 2; }
            printf("ok: rolagem automática %s\n", want ? "ligada" : "desligada");
        } else if (!strcmp(line, "mdown") || !strcmp(line, "mup")) {
            e.type = !strcmp(line, "mdown") ? EV_DOWN : EV_UP; e.button = 2; e.x = ui.in.mx; e.y = ui.in.my; push(&e); advance(16);
        }
        else if (!strcmp(line, "click") || !strcmp(line, "dblclick")) {
            int reps = !strcmp(line, "click") ? 1 : 2;
            int x, y; sscanf(arg, "%d %d", &x, &y);
            e.type = EV_MOVE; e.x = x; e.y = y; push(&e); advance(16);
            for (int r = 0; r < reps; r++) {
                e.type = EV_DOWN; e.button = 0; push(&e); advance(16);
                e.type = EV_UP; push(&e); advance(16);
            }
            advance(60);
        } else if (!strcmp(line, "wheel")) { e.type = EV_WHEEL; e.wheel = atoi(arg); push(&e); advance(300); }
        else if (!strcmp(line, "wait")) advance(atoi(arg));
        else if (!strcmp(line, "bench")) {
            /* bench N mode: real time of app_frame (mode: type = typing, idle = caret only, scroll) */
            int N = 200; char mode[32] = "type"; sscanf(arg, "%d %31s", &N, mode);
            struct timespec q0, q1;
            double tot = 0, worst = 0;
            for (int i = 0; i < N; i++) {
                if (!strcmp(mode, "type")) { e.type = EV_KEY; e.key = 0; e.ch = "abcde fghij "[i % 12]; e.us_raw = false; app_event(&e); }
                else if (!strcmp(mode, "scroll")) { e.type = EV_WHEEL; e.wheel = (i / 40) % 2 ? 1 : -1; app_event(&e); }
                else if (!strcmp(mode, "win")) { if (i % 40 == 0 || i % 40 == 20) { e.type = EV_KEY; e.ch = 0; e.key = i % 40 ? KEY_ESC : KEY_F12; app_event(&e); } }
                else if (!strcmp(mode, "move")) { e.type = EV_MOVE; e.x = 200 + (i * 7) % 900; e.y = 300 + (i * 3) % 300; app_event(&e); }
                fake_ms += 16;
                clock_gettime(CLOCK_MONOTONIC, &q0);
                app_frame();
                clock_gettime(CLOCK_MONOTONIC, &q1);
                double ms = mono_ms(&q0, &q1);
                tot += ms; if (ms > worst) worst = ms;
            }
            printf("bench %s %dx%d: média %.2f ms/quadro (%.0f fps), pior %.2f ms\n", mode, screen.w, screen.h, tot / N, 1000.0 / (tot / N), worst);
        }
        else if (!strcmp(line, "shot")) {
            char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", outdir, arg);
            advance(16);
            write_png(p, &screen);
            printf("captura: %s\n", p);
        } else if (!strcmp(line, "theme")) { app_set_theme(atoi(arg)); advance(50); }
        else if (!strcmp(line, "font")) { prefs.font = atoi(arg); app_apply_font(); advance(50); }
        else if (!strcmp(line, "lang")) {
            /* lang <code>: switches the language as the user would in Settings */
            int i = lang_find(arg);
            if (i < 0) { fprintf(stderr, "FALHA: idioma %s não encontrado em FOLIO/LANGUAGES\n", arg); return 2; }
            app_set_language(i, true); advance(50);
        } else if (!strcmp(line, "assert_lang")) {
            if (strcmp(i18n_code(), arg)) { fprintf(stderr, "FALHA: idioma %s (esperado %s)\n", i18n_code(), arg); return 2; }
            printf("ok: idioma %s (%d traduções)\n", arg, i18n_loaded());
        } else if (!strcmp(line, "assert_tr")) {
            /* assert_tr <original> => <expected translation> */
            char *sep = strstr(arg, " => ");
            if (!sep) { fprintf(stderr, "FALHA: assert_tr sem ' => '\n"); return 2; }
            *sep = 0;
            const char *got = L(arg);
            if (strcmp(got, sep + 4)) { fprintf(stderr, "FALHA: L(\"%s\") = \"%s\" (esperado \"%s\")\n", arg, got, sep + 4); return 2; }
            printf("ok: %s => %s\n", arg, got);
        } else if (!strcmp(line, "assert_plural")) {
            /* assert_plural <n> <singular> | <plural> => <expected> */
            long n = strtol(arg, 0, 10); char *p = strchr(arg, ' ');
            char *bar = p ? strstr(p, " | ") : 0, *sep = bar ? strstr(bar, " => ") : 0;
            if (!p || !bar || !sep) { fprintf(stderr, "FALHA: assert_plural mal formado\n"); return 2; }
            *bar = 0; *sep = 0;
            const char *got = LN(p + 1, bar + 3, n);
            if (strcmp(got, sep + 4)) { fprintf(stderr, "FALHA: LN(%ld) = \"%s\" (esperado \"%s\")\n", n, got, sep + 4); return 2; }
            printf("ok: plural %ld => %s\n", n, got);
        } else if (!strcmp(line, "assert_fmt")) {
            /* assert_fmt <format> | <int> <int> <int> => <expected>   (tests %1$d, %2$d…) */
            char *bar = strstr(arg, " | "), *sep = bar ? strstr(bar, " => ") : 0;
            if (!bar || !sep) { fprintf(stderr, "FALHA: assert_fmt mal formado\n"); return 2; }
            *bar = 0; *sep = 0;
            int a = 0, b = 0, c = 0; sscanf(bar + 3, "%d %d %d", &a, &b, &c);
            char out[200]; fmt(out, sizeof out, arg, a, b, c);
            if (strcmp(out, sep + 4)) { fprintf(stderr, "FALHA: fmt(\"%s\") = \"%s\" (esperado \"%s\")\n", arg, out, sep + 4); return 2; }
            printf("ok: fmt %s => %s\n", arg, out);
        } else if (!strcmp(line, "kbd")) { snprintf(prefs.kb, sizeof prefs.kb, "%s", arg); kbd_select(prefs.kb); advance(16); }
        else if (!strcmp(line, "pref")) {
            char k[64]; int v; if (sscanf(arg, "%63s %d", k, &v) == 2) {
                if (!strcmp(k, "focus")) prefs.focus_mode = v;
                else if (!strcmp(k, "typewriter")) prefs.typewriter = v;
                else if (!strcmp(k, "hide_markup")) { prefs.hide_markup = v; editor_invalidate(); }
                else if (!strcmp(k, "paper")) prefs.paper = v;
                else if (!strcmp(k, "fullwidth")) { prefs.full_width = v; editor_invalidate(); }
                else if (!strcmp(k, "column")) { prefs.column = (float)v; editor_invalidate(); }
                else if (!strcmp(k, "indent")) { prefs.first_indent = v; editor_invalidate(); }
                else if (!strcmp(k, "splash_secs")) { prefs.splash_secs = v; prefs_save(); }
            }
            advance(50);
        } else if (!strcmp(line, "assert_text")) {
            usize L; char *t = doc_text(&app.doc, &L);
            if (!strstr(t, arg)) { fprintf(stderr, "FALHA: texto não contém \"%s\"\n--- texto ---\n%s\n", arg, t); return 2; }
            printf("ok: contém \"%s\"\n", arg);
            mem_free(t);
        } else if (!strcmp(line, "assert_no_text")) {
            usize L; char *t = doc_text(&app.doc, &L);
            if (strstr(t, arg)) { fprintf(stderr, "FALHA: texto ainda contém \"%s\"\n", arg); return 2; }
            printf("ok: não contém \"%s\"\n", arg);
            mem_free(t);
        } else if (!strcmp(line, "assert_nowin")) {
            Window *w = wm_top();
            if (w) { fprintf(stderr, "FALHA: janela ainda aberta: %s\n", w->title); return 2; }
            printf("ok: nenhuma janela aberta\n");
        } else if (!strcmp(line, "assert_keywin")) {     /* quem recebe o teclado: "text" ou o título (msgid) da janela */
            const char *got = ui.key_win ? ui.key_win->title : "text";
            if (strcmp(got, arg)) { fprintf(stderr, "FALHA: teclado em \"%s\" (esperado \"%s\")\n", got, arg); return 2; }
            printf("ok: teclado em %s\n", arg);
        } else if (!strcmp(line, "assert_para")) {
            int a = 0, b = -1; if (sscanf(arg, "%d %d", &a, &b) < 2) b = a;
            int p = preview_active() ? preview_top_para() : app.caret.p;
            if (p < a || p > b) { fprintf(stderr, "FALHA: parágrafo %d fora de [%d, %d]%s\n", p, a, b, preview_active() ? " (leitura)" : ""); return 2; }
            printf("ok: parágrafo %d%s\n", p, preview_active() ? " (leitura)" : "");
        } else if (!strcmp(line, "assert_file")) {
            bool d; if (!pal_exists(0, arg, &d)) { fprintf(stderr, "FALHA: arquivo ausente %s\n", arg); return 2; }
            printf("ok: arquivo %s\n", arg);
        } else if (!strcmp(line, "export")) {
            char fmtname[16], path[512]; sscanf(arg, "%15s %511s", fmtname, path);
            int fm = -1; for (int i = 0; i < FMT_COUNT; i++) if (!strcasecmp(format_info(i)->ext + 1, fmtname)) fm = i;
            if (fm < 0) { fprintf(stderr, "formato inválido\n"); return 2; }
            app_save_as_done(0, path, fm); advance(50);
        } else if (!strcmp(line, "open")) { app_open_path(0, arg); advance(100); }
        else if (!strcmp(line, "restart")) {
            /* simulates power off/on: saves everything as at the end of app_run and starts again */
            app_flush_all(); prefs_save();
            app_init(); advance(100);
        } else if (!strcmp(line, "assert_tabs")) {
            int want = atoi(arg);
            if (app_tab_count() != want) { fprintf(stderr, "FALHA: %d abas (esperado %d)\n", app_tab_count(), want); return 2; }
            printf("ok: %d abas\n", want);
        } else if (!strcmp(line, "assert_active")) {
            int want = atoi(arg);
            if (app_tab_current() != want) { fprintf(stderr, "FALHA: aba ativa %d (esperado %d)\n", app_tab_current(), want); return 2; }
            printf("ok: aba ativa %d\n", want);
        } else if (!strcmp(line, "themename")) {
            int i = theme_find(arg);
            if (i < 0) { fprintf(stderr, "FALHA: tema não encontrado: %s\n", arg); return 2; }
            app_set_theme(i); advance(50);
        } else if (!strcmp(line, "assert_theme")) {
            if (strcmp(T->name, arg)) { fprintf(stderr, "FALHA: tema ativo \"%s\" (esperado \"%s\")\n", T->name, arg); return 2; }
            printf("ok: tema %s\n", arg);
        } else if (!strcmp(line, "assert_font")) {
            if (strcmp(font_get(prefs.font)->name, arg)) { fprintf(stderr, "FALHA: fonte \"%s\" (esperado \"%s\")\n", font_get(prefs.font)->name, arg); return 2; }
            printf("ok: fonte %s\n", arg);
        } else if (!strcmp(line, "assert_uifont")) {
            if (strcmp(font_ui()->name, arg)) { fprintf(stderr, "FALHA: fonte da interface \"%s\" (esperado \"%s\")\n", font_ui()->name, arg); return 2; }
            printf("ok: fonte da interface %s\n", arg);
        } else if (!strcmp(line, "theme_save_copy")) {
            /* saves a copy of the active theme with another name and paper (tests the FOLIO/THEMES writer) */
            Theme t = *T; t.paper = RGB(0x123456);
            int i = theme_save_custom(&t, arg, "Spectral", "", 0);
            if (i < 0) { fprintf(stderr, "FALHA: não gravou o tema %s\n", arg); return 2; }
            printf("ok: tema gravado (%d)\n", i);
        } else if (!strcmp(line, "assert_splash")) {
            if (splash_active() != (atoi(arg) != 0)) { fprintf(stderr, "FALHA: abertura %s\n", splash_active() ? "ainda visível" : "não visível"); return 2; }
            printf("ok: abertura %s\n", atoi(arg) ? "visível" : "encerrada");
        }
        else fprintf(stderr, "comando desconhecido: %s\n", line);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------ GUI startup */
static FolioDelegate *delegate;
static void gui_start(int W, int H) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    /* the .app gets its icon from Info.plist; the bare binary loads FolioOS.icns from its own folder */
    if (![[NSBundle mainBundle].bundlePath hasSuffix:@".app"]) {
        NSString *icns = [[[NSBundle mainBundle].executablePath stringByDeletingLastPathComponent]
                          stringByAppendingPathComponent:@"FolioOS.icns"];
        NSImage *icon = [[NSImage alloc] initWithContentsOfFile:icns];
        if (icon) NSApp.applicationIconImage = icon;
    }
    delegate = [FolioDelegate new];
    NSApp.delegate = delegate;
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit FolioOS Simulator" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];
    srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSScreen *scr = [NSScreen mainScreen];
    CGFloat cw = W, ch = H;
    if (scr) {
        NSRect maxc = [NSWindow contentRectForFrameRect:scr.visibleFrame styleMask:style];
        cw = MIN(cw, maxc.size.width); ch = MIN(ch, maxc.size.height);
    }
    win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, cw, ch) styleMask:style backing:NSBackingStoreBuffered defer:NO];
    win.releasedWhenClosed = NO;
    win.title = @"FolioOS — simulator";
    win.delegate = delegate;
    win.acceptsMouseMovedEvents = YES;
    win.contentMinSize = NSMakeSize(320, 240);
    view = [[FolioView alloc] initWithFrame:NSMakeRect(0, 0, cw, ch)];
    win.contentView = view;
    [win makeFirstResponder:view];
    [win center];
    [win makeKeyAndOrderFront:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    sync_size();
}

int main(int argc, char **argv) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    static u8 *heap; usize hs = 512u << 20;
    heap = mmap(0, hs, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (heap == MAP_FAILED) { fprintf(stderr, "out of memory\n"); return 1; }
    heap_init(heap, hs);
    int W = 1600, H = 900;
    const char *script = 0, *out = ".";
    bool have_root = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) { snprintf(roots[0], PATH_MAX, "%s", argv[++i]); have_root = true; }
        else if (!strcmp(argv[i], "--vol2") && i + 1 < argc) snprintf(roots[nroots++], PATH_MAX, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    if (!have_root) {
        /* build/usb_files from the repository root; otherwise next to the build folder of the
         * executable (build/macos/folio_sim or build/macos/FolioOS Simulator.app -> build/usb_files) */
        struct stat st;
        if (stat("build/usb_files", &st) == 0 && S_ISDIR(st.st_mode)) snprintf(roots[0], PATH_MAX, "build/usb_files");
        else @autoreleasepool {
            NSString *dir = [NSBundle mainBundle].bundlePath;
            if ([dir.pathExtension.lowercaseString isEqualToString:@"app"]) dir = dir.stringByDeletingLastPathComponent;
            dir = [dir.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"usb_files"];
            snprintf(roots[0], PATH_MAX, "%s", dir.fileSystemRepresentation);
        }
    }
    if (script) { headless = true; fake_ms = 1000; resize_surface(W, H); return run_script(script, out); }
    resize_surface(W, H);
    gui_start(W, H);
    app_run();
    return 0;
}

/* system language ("pt-BR"); in scripted tests FOLIO_PLATFORM_LANG (or none), so the result does not
 * depend on the machine the tests run on */
bool pal_lang(char *out, int cap) {
    if (headless) {
        const char *v = getenv("FOLIO_PLATFORM_LANG");
        if (!v || !v[0]) return false;
        snprintf(out, (size_t)cap, "%s", v);
        return true;
    }
    @autoreleasepool {
        NSString *pref = [NSLocale preferredLanguages].firstObject;
        if (!pref.length) return false;
        NSLocale *loc = [NSLocale localeWithLocaleIdentifier:pref];
        NSString *lang = [loc objectForKey:NSLocaleLanguageCode];
        NSString *region = [loc objectForKey:NSLocaleCountryCode];
        NSString *script = [loc objectForKey:NSLocaleScriptCode];
        if (!lang.length) return false;
        if (!region.length && [script isEqualToString:@"Hans"]) region = @"CN";
        if (!region.length && [script isEqualToString:@"Hant"]) region = @"TW";
        NSString *tag = region.length ? [NSString stringWithFormat:@"%@-%@", lang, region] : lang;
        snprintf(out, (size_t)cap, "%s", tag.UTF8String);
        return strlen(out) >= 2;
    }
}
