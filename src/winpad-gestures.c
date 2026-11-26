#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#define WINPAD_VERSION "1.2.0"
#define MAX_SLOTS 16
#define ALT_WATCHDOG_MS 15000L
#define THREE_F_SETTLE_MS 32L
#define DROP_HYSTERESIS_MS 45L
#define OSD_DURATION_MS 750L
#define OSD_W 268
#define OSD_H 74
#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

typedef enum {
    TWO_F_OFF = 0,
    TWO_F_NATURAL = 1, /* swipe right -> Back, swipe left -> Forward (Windows Edge / macOS) */
    TWO_F_ARROW = 2    /* swipe left -> Back, swipe right -> Forward */
} TwoFingerMode;

typedef enum {
    THREE_H_OFF = 0,
    THREE_H_SCRUB = 1,
    THREE_H_ONCE = 2
} ThreeHorizMode;

typedef enum {
    FOUR_H_OFF = 0,
    FOUR_H_WORKSPACE_DIRECT = 1,  /* Windows default: swipe right -> next desktop, swipe left -> prev */
    FOUR_H_WORKSPACE_NATURAL = 2, /* swipe left -> next desktop, swipe right -> prev */
    FOUR_H_ALTTAB = 3
} FourHorizMode;

typedef struct {
    char device[512];
    char taskview_cmd[512];
    TwoFingerMode two_f_mode;
    double two_f_min_mm;
    long two_f_max_ms;
    ThreeHorizMode three_h_mode;
    int three_h_invert; /* 0 = direct (right=next/Tab, left=prev/Shift+Tab), 1 = natural */
    double three_h_thresh_mm;
    double three_h_step_mm;
    double three_v_thresh_mm;
    FourHorizMode four_h_mode;
    double four_h_thresh_mm;
    double four_v_thresh_mm;
} Config;

typedef struct {
    int32_t tracking_id;
    int32_t x;
    int32_t y;
    int32_t prev_x;
    int32_t prev_y;
    int was_active;
} Slot;

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_cfg = 0;

static Display *g_dpy = NULL;
static Window g_root = None;
static int g_debug = 0;
static int g_alt_held = 0;
static long g_alt_held_since = 0;

static Window g_osd_win = None;
static Colormap g_osd_cmap = None;
static Visual *g_osd_visual = NULL;
static int g_osd_argb = 0;
static long g_osd_hide_at = 0;

static KeyCode g_kc_alt = 0;
static KeyCode g_kc_shift = 0;
static KeyCode g_kc_tab = 0;
static KeyCode g_kc_left = 0;
static KeyCode g_kc_right = 0;
static KeyCode g_kc_back = 0;
static KeyCode g_kc_fwd = 0;

static Atom g_atom_showing_desktop = None;
static Atom g_atom_current_desktop = None;
static Atom g_atom_num_desktops = None;
static Atom g_atom_active_window = None;
static Atom g_atom_wm_type = None;
static Atom g_atom_wm_type_notif = None;

static Config g_cfg;

static int x11_quiet_err(Display *d, XErrorEvent *e) {
    (void)d;
    (void)e;
    return 0;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void on_signal(int sig) {
    if (sig == SIGHUP)
        g_reload_cfg = 1;
    else
        g_running = 0;
}

static void hide_osd(void) {
    if (g_osd_win != None && g_dpy) {
        XUnmapWindow(g_dpy, g_osd_win);
        XFlush(g_dpy);
        g_osd_hide_at = 0;
    }
}

static void release_alt_if_held(void) {
    if (g_alt_held && g_dpy && g_kc_alt) {
        XTestFakeKeyEvent(g_dpy, g_kc_alt, False, CurrentTime);
        XFlush(g_dpy);
        g_alt_held = 0;
        g_alt_held_since = 0;
    }
}

static void cleanup_all(void) {
    release_alt_if_held();
    if (g_dpy) {
        if (g_osd_win != None) XDestroyWindow(g_dpy, g_osd_win);
        if (g_osd_cmap != None) XFreeColormap(g_dpy, g_osd_cmap);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }
}

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

static void expand_home(const char *in, char *out, size_t outsz) {
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(out, outsz, "%s%s", home, in + 1);
            return;
        }
    }
    snprintf(out, outsz, "%s", in);
}

static void load_config(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.device, sizeof(g_cfg.device), "auto");
    expand_home("~/.local/bin/winpad-taskview", g_cfg.taskview_cmd, sizeof(g_cfg.taskview_cmd));
    g_cfg.two_f_mode = TWO_F_NATURAL;
    g_cfg.two_f_min_mm = 16.0;
    g_cfg.two_f_max_ms = 380;
    g_cfg.three_h_mode = THREE_H_SCRUB;
    g_cfg.three_h_invert = 0;
    g_cfg.three_h_thresh_mm = 8.5;
    g_cfg.three_h_step_mm = 14.0;
    g_cfg.three_v_thresh_mm = 9.5;
    g_cfg.four_h_mode = FOUR_H_WORKSPACE_DIRECT;
    g_cfg.four_h_thresh_mm = 10.0;
    g_cfg.four_v_thresh_mm = 10.0;

    char path[512];
    expand_home("~/.config/winpad-gestures.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[768];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        trim(line);
        if (!line[0]) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        trim(k);
        trim(v);

        if (strcmp(k, "device") == 0) {
            snprintf(g_cfg.device, sizeof(g_cfg.device), "%s", v);
        } else if (strcmp(k, "taskview_cmd") == 0) {
            expand_home(v, g_cfg.taskview_cmd, sizeof(g_cfg.taskview_cmd));
        } else if (strcmp(k, "two_finger_swipe") == 0) {
            if (strcmp(v, "off") == 0) g_cfg.two_f_mode = TWO_F_OFF;
            else if (strcmp(v, "arrow") == 0) g_cfg.two_f_mode = TWO_F_ARROW;
            else g_cfg.two_f_mode = TWO_F_NATURAL;
        } else if (strcmp(k, "two_finger_swipe_min_mm") == 0) {
            double val = atof(v);
            if (val >= 6.0 && val <= 100.0) g_cfg.two_f_min_mm = val;
        } else if (strcmp(k, "two_finger_swipe_max_ms") == 0) {
            long val = atol(v);
            if (val >= 100 && val <= 2000) g_cfg.two_f_max_ms = val;
        } else if (strcmp(k, "three_finger_horiz") == 0) {
            if (strcmp(v, "off") == 0) g_cfg.three_h_mode = THREE_H_OFF;
            else if (strcmp(v, "alt_tab_once") == 0) g_cfg.three_h_mode = THREE_H_ONCE;
            else g_cfg.three_h_mode = THREE_H_SCRUB;
        } else if (strcmp(k, "three_finger_direction") == 0) {
            g_cfg.three_h_invert = (strcmp(v, "natural") == 0) ? 1 : 0;
        } else if (strcmp(k, "three_finger_thresh_mm") == 0) {
            double val = atof(v);
            if (val >= 4.0 && val <= 80.0) g_cfg.three_h_thresh_mm = val;
        } else if (strcmp(k, "three_finger_v_thresh_mm") == 0) {
            double val = atof(v);
            if (val >= 4.0 && val <= 80.0) g_cfg.three_v_thresh_mm = val;
        } else if (strcmp(k, "three_finger_step_mm") == 0) {
            double val = atof(v);
            if (val >= 6.0 && val <= 80.0) g_cfg.three_h_step_mm = val;
        } else if (strcmp(k, "four_finger_horiz") == 0) {
            if (strcmp(v, "off") == 0) g_cfg.four_h_mode = FOUR_H_OFF;
            else if (strcmp(v, "workspace_natural") == 0) g_cfg.four_h_mode = FOUR_H_WORKSPACE_NATURAL;
            else if (strcmp(v, "alt_tab") == 0) g_cfg.four_h_mode = FOUR_H_ALTTAB;
            else g_cfg.four_h_mode = FOUR_H_WORKSPACE_DIRECT;
        } else if (strcmp(k, "four_finger_thresh_mm") == 0) {
            double val = atof(v);
            if (val >= 4.0 && val <= 80.0) {
                g_cfg.four_h_thresh_mm = val;
                g_cfg.four_v_thresh_mm = val;
            }
        }
    }
    fclose(f);
}

static int is_touchpad_fd(int fd, int *out_max_fingers, double *res_x, double *res_y) {
    unsigned long evbits[NBITS(EV_MAX)] = {0};
    unsigned long absbits[NBITS(ABS_MAX)] = {0};
    unsigned long keybits[NBITS(KEY_MAX)] = {0};
    unsigned long propbits[NBITS(INPUT_PROP_MAX)] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return 0;
    if (!TEST_BIT(EV_ABS, evbits) || !TEST_BIT(EV_KEY, evbits)) return 0;

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return 0;
    ioctl(fd, EVIOCGPROP(sizeof(propbits)), propbits);

    /* Ignore direct touchscreens. */
    if (TEST_BIT(INPUT_PROP_DIRECT, propbits)) return 0;

    if (!TEST_BIT(ABS_MT_POSITION_X, absbits) || !TEST_BIT(ABS_MT_POSITION_Y, absbits))
        return 0;
    if (!TEST_BIT(BTN_TOOL_FINGER, keybits))
        return 0;

    int maxf = 1;
    if (TEST_BIT(BTN_TOOL_QUINTTAP, keybits)) maxf = 5;
    else if (TEST_BIT(BTN_TOOL_QUADTAP, keybits)) maxf = 4;
    else if (TEST_BIT(BTN_TOOL_TRIPLETAP, keybits)) maxf = 3;
    else if (TEST_BIT(BTN_TOOL_DOUBLETAP, keybits)) maxf = 2;

    struct input_absinfo ax = {0}, ay = {0};
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0) return 0;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0) return 0;

    double rx = ax.resolution > 0 ? (double)ax.resolution
                                  : (double)(ax.maximum - ax.minimum) / 110.0;
    double ry = ay.resolution > 0 ? (double)ay.resolution
                                  : (double)(ay.maximum - ay.minimum) / 70.0;
    if (rx < 1.0) rx = 30.0;
    if (ry < 1.0) ry = 30.0;

    *out_max_fingers = maxf;
    *res_x = rx;
    *res_y = ry;
    return 1;
}

static int find_and_open_touchpad(char *out_path, size_t pathsz,
                                  char *out_name, size_t namesz,
                                  double *res_x, double *res_y, int *out_maxf,
                                  int *saw_perm_denied) {
    *saw_perm_denied = 0;

    if (strcmp(g_cfg.device, "auto") != 0) {
        int fd = open(g_cfg.device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES) *saw_perm_denied = 1;
            return -1;
        }
        int mf = 1;
        if (is_touchpad_fd(fd, &mf, res_x, res_y)) {
            snprintf(out_path, pathsz, "%s", g_cfg.device);
            if (ioctl(fd, EVIOCGNAME(namesz), out_name) < 0)
                snprintf(out_name, namesz, "Unknown Touchpad");
            *out_maxf = mf;
            return fd;
        }
        close(fd);
        return -1;
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    int best_fd = -1;
    int best_maxf = 0;
    double best_rx = 30.0, best_ry = 30.0;
    char best_path[512] = {0}, best_name[256] = {0};

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char p[512];
        snprintf(p, sizeof(p), "/dev/input/%s", de->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES) *saw_perm_denied = 1;
            continue;
        }
        int mf = 0;
        double rx = 30.0, ry = 30.0;
        if (is_touchpad_fd(fd, &mf, &rx, &ry)) {
            char nm[256] = {0};
            if (ioctl(fd, EVIOCGNAME(sizeof(nm)), nm) < 0)
                snprintf(nm, sizeof(nm), "Touchpad");
            if (mf > best_maxf) {
                if (best_fd >= 0) close(best_fd);
                best_fd = fd;
                best_maxf = mf;
                best_rx = rx;
                best_ry = ry;
                snprintf(best_path, sizeof(best_path), "%s", p);
                snprintf(best_name, sizeof(best_name), "%s", nm);
            } else {
                close(fd);
            }
        } else {
            close(fd);
        }
    }
    closedir(dir);

    if (best_fd >= 0) {
        snprintf(out_path, pathsz, "%s", best_path);
        snprintf(out_name, namesz, "%s", best_name);
        *res_x = best_rx;
        *res_y = best_ry;
        *out_maxf = best_maxf;
    }
    return best_fd;
}

static void init_osd_window(void) {
    int scr = DefaultScreen(g_dpy);
    XVisualInfo vinfo;
    if (XMatchVisualInfo(g_dpy, scr, 32, TrueColor, &vinfo)) {
        XRenderPictFormat *fmt = XRenderFindVisualFormat(g_dpy, vinfo.visual);
        if (fmt && fmt->direct.alphaMask) {
            g_osd_visual = vinfo.visual;
            g_osd_cmap = XCreateColormap(g_dpy, g_root, vinfo.visual, AllocNone);
            XSetWindowAttributes swa;
            memset(&swa, 0, sizeof(swa));
            swa.override_redirect = True;
            swa.colormap = g_osd_cmap;
            swa.background_pixel = 0;
            swa.border_pixel = 0;
            swa.save_under = True;
            g_osd_win = XCreateWindow(g_dpy, g_root, 0, 0, OSD_W, OSD_H, 0,
                                      32, InputOutput, vinfo.visual,
                                      CWOverrideRedirect | CWColormap | CWBackPixel |
                                      CWBorderPixel | CWSaveUnder, &swa);
            if (g_osd_win != None)
                g_osd_argb = 1;
        }
    }

    if (g_osd_win == None) {
        g_osd_visual = DefaultVisual(g_dpy, scr);
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.override_redirect = True;
        swa.background_pixel = 0x141824;
        swa.border_pixel = 0x7aa2f7;
        swa.save_under = True;
        g_osd_win = XCreateWindow(g_dpy, g_root, 0, 0, OSD_W, OSD_H, 1,
                                  CopyFromParent, InputOutput, CopyFromParent,
                                  CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWSaveUnder,
                                  &swa);
    }

    if (g_osd_win != None) {
        XChangeProperty(g_dpy, g_osd_win, g_atom_wm_type, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&g_atom_wm_type_notif, 1);
    }
}

static void init_x11_state(void) {
    g_root = DefaultRootWindow(g_dpy);

    g_kc_alt   = XKeysymToKeycode(g_dpy, XK_Alt_L);
    g_kc_shift = XKeysymToKeycode(g_dpy, XK_Shift_L);
    g_kc_tab   = XKeysymToKeycode(g_dpy, XK_Tab);
    g_kc_left  = XKeysymToKeycode(g_dpy, XK_Left);
    g_kc_right = XKeysymToKeycode(g_dpy, XK_Right);
#ifdef XF86XK_Back
    g_kc_back  = XKeysymToKeycode(g_dpy, XF86XK_Back);
    g_kc_fwd   = XKeysymToKeycode(g_dpy, XF86XK_Forward);
#endif

    g_atom_showing_desktop = XInternAtom(g_dpy, "_NET_SHOWING_DESKTOP", False);
    g_atom_current_desktop = XInternAtom(g_dpy, "_NET_CURRENT_DESKTOP", False);
    g_atom_num_desktops    = XInternAtom(g_dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    g_atom_active_window   = XInternAtom(g_dpy, "_NET_ACTIVE_WINDOW", False);
    g_atom_wm_type         = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    g_atom_wm_type_notif   = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);

    init_osd_window();
}

static void get_primary_monitor_rect(int *ox, int *oy, int *ow, int *oh) {
    int scr = DefaultScreen(g_dpy);
    *ox = 0;
    *oy = 0;
    *ow = DisplayWidth(g_dpy, scr);
    *oh = DisplayHeight(g_dpy, scr);

    int nmon = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(g_dpy, g_root, True, &nmon);
    if (mons && nmon > 0) {
        int chosen = 0;
        for (int i = 0; i < nmon; i++) {
            if (mons[i].primary) {
                chosen = i;
                break;
            }
        }
        *ox = mons[chosen].x;
        *oy = mons[chosen].y;
        *ow = mons[chosen].width;
        *oh = mons[chosen].height;
        XRRFreeMonitors(mons);
    }
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    const double deg = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -90 * deg, 0 * deg);
    cairo_arc(cr, x + w - r, y + h - r, r, 0 * deg, 90 * deg);
    cairo_arc(cr, x + r, y + h - r, r, 90 * deg, 180 * deg);
    cairo_arc(cr, x + r, y + r, r, 180 * deg, 270 * deg);
    cairo_close_path(cr);
}

static void show_desktop_osd(long cur_idx, long total) {
    if (g_osd_win == None || !g_dpy) return;

    int mx = 0, my = 0, mw = 1920, mh = 1080;
    get_primary_monitor_rect(&mx, &my, &mw, &mh);
    int x = mx + (mw - OSD_W) / 2;
    int y = my + mh - OSD_H - 72;

    XMoveResizeWindow(g_dpy, g_osd_win, x, y, OSD_W, OSD_H);
    XMapRaised(g_dpy, g_osd_win);

    cairo_surface_t *surf = cairo_xlib_surface_create(g_dpy, g_osd_win, g_osd_visual, OSD_W, OSD_H);
    cairo_t *cr = cairo_create(surf);

    if (g_osd_argb) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    }

    double radius = g_osd_argb ? 18.0 : 0.0;
    rounded_rect(cr, 1.0, 1.0, OSD_W - 2.0, OSD_H - 2.0, radius);
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, OSD_W, OSD_H);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.085, 0.105, 0.165, 0.93);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.060, 0.075, 0.125, 0.95);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    /* Subtle glowing border */
    cairo_set_source_rgba(cr, 0.478, 0.635, 0.969, 0.52);
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);

    /* Top inner sheen */
    rounded_rect(cr, 2.5, 2.5, OSD_W - 5.0, OSD_H - 5.0, radius > 2.0 ? radius - 1.5 : 0.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.07);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* Mini desktop emblem on left */
    rounded_rect(cr, 20.0, 15.0, 26.0, 19.0, 4.5);
    cairo_pattern_t *ico = cairo_pattern_create_linear(20.0, 15.0, 46.0, 34.0);
    cairo_pattern_add_color_stop_rgb(ico, 0.0, 0.49, 0.81, 1.0);
    cairo_pattern_add_color_stop_rgb(ico, 1.0, 0.48, 0.64, 0.97);
    cairo_set_source(cr, ico);
    cairo_fill(cr);
    cairo_pattern_destroy(ico);

    rounded_rect(cr, 24.0, 19.0, 18.0, 11.0, 2.0);
    cairo_set_source_rgba(cr, 0.07, 0.09, 0.14, 0.85);
    cairo_fill(cr);

    /* Crisp label: "Desktop X" + "of N" */
    char title[32], sub[16];
    snprintf(title, sizeof(title), "Desktop %ld", cur_idx + 1);
    snprintf(sub, sizeof(sub), "of %ld", total);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 15.5);
    cairo_text_extents_t te;
    cairo_text_extents(cr, title, &te);
    cairo_set_source_rgb(cr, 0.96, 0.97, 1.0);
    cairo_move_to(cr, 56.0, 30.0);
    cairo_show_text(cr, title);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5);
    cairo_set_source_rgba(cr, 0.62, 0.68, 0.80, 0.90);
    cairo_move_to(cr, 56.0 + te.x_advance + 8.0, 30.0);
    cairo_show_text(cr, sub);

    /* Sleek capsule indicator bar centered at bottom of pill */
    int dots = (int)(total > 8 ? 8 : total);
    double active_w = 30.0, idle_w = 12.0, dot_h = 6.0, gap = 8.0;
    double bar_w = (dots - 1) * idle_w + active_w + (dots - 1) * gap;
    double px = (OSD_W - bar_w) * 0.5;
    double py = 50.0;

    for (int i = 0; i < dots; i++) {
        double dw = (i == cur_idx) ? active_w : idle_w;
        rounded_rect(cr, px, py, dw, dot_h, 3.0);
        if (i == cur_idx) {
            cairo_pattern_t *act = cairo_pattern_create_linear(px, py, px + dw, py);
            cairo_pattern_add_color_stop_rgb(act, 0.0, 0.49, 0.81, 1.0);
            cairo_pattern_add_color_stop_rgb(act, 1.0, 0.73, 0.60, 0.97);
            cairo_set_source(cr, act);
            cairo_fill(cr);
            cairo_pattern_destroy(act);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
            cairo_fill(cr);
        }
        px += dw + gap;
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    XFlush(g_dpy);
    g_osd_hide_at = now_ms() + OSD_DURATION_MS;
}

static void send_key_combo(KeyCode mod, KeyCode key) {
    if (!g_dpy || !key) return;
    XkbStateRec xkb_before;
    int have_xkb = (mod == g_kc_shift) && (XkbGetState(g_dpy, XkbUseCoreKbd, &xkb_before) == Success);

    if (mod) XTestFakeKeyEvent(g_dpy, mod, True, CurrentTime);
    XTestFakeKeyEvent(g_dpy, key, True, CurrentTime);
    XTestFakeKeyEvent(g_dpy, key, False, CurrentTime);
    if (mod) XTestFakeKeyEvent(g_dpy, mod, False, CurrentTime);
    XFlush(g_dpy);

    if (have_xkb) {
        XkbStateRec xkb_after;
        if (XkbGetState(g_dpy, XkbUseCoreKbd, &xkb_after) == Success &&
            xkb_after.group != xkb_before.group) {
            XkbLockGroup(g_dpy, XkbUseCoreKbd, xkb_before.group);
            XFlush(g_dpy);
        }
    }
}

static void send_shift_tab_with_alt_held(void) {
    if (!g_dpy || !g_kc_shift || !g_kc_tab) return;
    XkbStateRec st_before;
    int have_xkb = (XkbGetState(g_dpy, XkbUseCoreKbd, &st_before) == Success);

    XTestFakeKeyEvent(g_dpy, g_kc_shift, True, CurrentTime);
    XTestFakeKeyEvent(g_dpy, g_kc_tab, True, CurrentTime);
    XTestFakeKeyEvent(g_dpy, g_kc_tab, False, CurrentTime);
    XTestFakeKeyEvent(g_dpy, g_kc_shift, False, CurrentTime);
    XFlush(g_dpy);

    if (have_xkb) {
        XkbStateRec st_after;
        if (XkbGetState(g_dpy, XkbUseCoreKbd, &st_after) == Success &&
            st_after.group != st_before.group) {
            XkbLockGroup(g_dpy, XkbUseCoreKbd, st_before.group);
            XFlush(g_dpy);
        }
    }
}

static void send_tab_with_alt_held(void) {
    if (!g_dpy || !g_kc_tab) return;
    XTestFakeKeyEvent(g_dpy, g_kc_tab, True, CurrentTime);
    XTestFakeKeyEvent(g_dpy, g_kc_tab, False, CurrentTime);
    XFlush(g_dpy);
}

static void send_alt_tab_once(int dir_right) {
    if (!g_dpy || !g_kc_alt || !g_kc_tab) return;
    int forward = g_cfg.three_h_invert ? !dir_right : dir_right;
    if (forward) {
        send_key_combo(g_kc_alt, g_kc_tab);
    } else {
        XTestFakeKeyEvent(g_dpy, g_kc_alt, True, CurrentTime);
        send_shift_tab_with_alt_held();
        XTestFakeKeyEvent(g_dpy, g_kc_alt, False, CurrentTime);
        XFlush(g_dpy);
    }
}

static int get_ewmh_cardinal(Atom prop, long *out_val) {
    if (!g_dpy || prop == None) return 0;
    Atom actual_type;
    int actual_fmt;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    int ok = 0;
    if (XGetWindowProperty(g_dpy, g_root, prop, 0, 1, False,
                           XA_CARDINAL, &actual_type, &actual_fmt,
                           &nitems, &bytes_after, &data) == Success) {
        if (data && nitems >= 1 && actual_fmt == 32) {
            *out_val = *(long *)data;
            ok = 1;
        }
        if (data) XFree(data);
    }
    return ok;
}

static void send_ewmh_root_msg(Atom prop, long d0, long d1) {
    if (!g_dpy || prop == None) return;
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = g_root;
    ev.xclient.message_type = prop;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = d0;
    ev.xclient.data.l[1] = d1;
    XSendEvent(g_dpy, g_root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(g_dpy);
}

static pid_t get_taskview_pid(void) {
    char pidpath[128];
    snprintf(pidpath, sizeof(pidpath), "/tmp/winpad-taskview-%d.pid", (int)getuid());
    FILE *f = fopen(pidpath, "r");
    if (!f) return 0;
    long pid = 0;
    pid_t alive = 0;
    if (fscanf(f, "%ld", &pid) == 1 && pid > 1) {
        char commpath[128], comm[64] = {0};
        snprintf(commpath, sizeof(commpath), "/proc/%ld/comm", pid);
        FILE *cf = fopen(commpath, "r");
        if (cf) {
            if (fgets(comm, sizeof(comm), cf) && strncmp(comm, "winpad-taskview", 15) == 0)
                alive = (pid_t)pid;
            fclose(cf);
        }
    }
    fclose(f);
    return alive;
}

static int switch_workspace(int delta) {
    long count = 1, cur = 0;
    if (!get_ewmh_cardinal(g_atom_num_desktops, &count) || count <= 1)
        return 0;
    get_ewmh_cardinal(g_atom_current_desktop, &cur);
    long target = cur + delta;
    if (target < 0) target = 0;
    if (target >= count) target = count - 1;
    if (target != cur) {
        send_ewmh_root_msg(g_atom_current_desktop, target, CurrentTime);
        pid_t tv = get_taskview_pid();
        if (tv > 0)
            kill(tv, SIGUSR1);
        else
            show_desktop_osd(target, count);
    } else {
        if (get_taskview_pid() == 0)
            show_desktop_osd(cur, count);
    }
    return 1;
}

static void spawn_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* Windows Precision Touchpad 3-state vertical cycle:
 *   [Task View] <-- Up / Down --> [Normal Windows] <-- Down / Up --> [Show Desktop]
 */
static void handle_vertical_gesture(int is_up) {
    long showing = 0;
    get_ewmh_cardinal(g_atom_showing_desktop, &showing);
    pid_t tv = get_taskview_pid();

    if (is_up) {
        if (tv > 0) return;
        if (showing) {
            if (g_debug) fprintf(stderr, "DEBUG: Up -> Restore windows from Show Desktop\n");
            send_ewmh_root_msg(g_atom_showing_desktop, 0, 0);
        } else {
            if (g_debug) fprintf(stderr, "DEBUG: Up -> Open Task View\n");
            spawn_cmd(g_cfg.taskview_cmd);
        }
    } else {
        if (tv > 0) {
            if (g_debug) fprintf(stderr, "DEBUG: Down -> Close Task View\n");
            kill(tv, SIGTERM);
            return;
        }
        if (!showing) {
            if (g_debug) fprintf(stderr, "DEBUG: Down -> Show Desktop\n");
            send_ewmh_root_msg(g_atom_showing_desktop, 1, 0);
        }
    }
}

static void get_active_window_class(char *buf, size_t bufsz) {
    buf[0] = '\0';
    if (!g_dpy || g_atom_active_window == None) return;
    Atom actual_type;
    int actual_fmt;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    Window active = None;
    if (XGetWindowProperty(g_dpy, g_root, g_atom_active_window, 0, 1, False,
                           XA_WINDOW, &actual_type, &actual_fmt,
                           &nitems, &bytes_after, &data) == Success) {
        if (data && nitems >= 1)
            active = *(Window *)data;
        if (data) XFree(data);
    }
    if (active == None) {
        int revert = 0;
        XGetInputFocus(g_dpy, &active, &revert);
    }
    if (active == None || active == PointerRoot) return;

    XClassHint ch = {0};
    if (XGetClassHint(g_dpy, active, &ch)) {
        if (ch.res_class) snprintf(buf, bufsz, "%s", ch.res_class);
        else if (ch.res_name) snprintf(buf, bufsz, "%s", ch.res_name);
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    for (size_t i = 0; buf[i]; i++)
        buf[i] = (char)tolower((unsigned char)buf[i]);
}

static void trigger_back_forward(int is_back) {
    char cls[128];
    get_active_window_class(cls, sizeof(cls));

    if (strstr(cls, "terminal") || strstr(cls, "alacritty") || strstr(cls, "kitty") ||
        strstr(cls, "wezterm") || strstr(cls, "konsole") || strstr(cls, "xterm") ||
        strstr(cls, "urxvt") || strstr(cls, "tilix")) {
        return;
    }

    if (strstr(cls, "code") || strstr(cls, "obsidian") || strstr(cls, "jetbrains")) {
        send_key_combo(g_kc_alt, is_back ? g_kc_left : g_kc_right);
        return;
    }
    if (g_kc_back && g_kc_fwd) {
        send_key_combo(0, is_back ? g_kc_back : g_kc_fwd);
    } else {
        send_key_combo(g_kc_alt, is_back ? g_kc_left : g_kc_right);
    }
}

static void reap_children(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static void evaluate_three_finger_gesture(double res_x, double res_y, double accum_x, double accum_y,
                                          double *step_anchor_x, int *gesture_fired, long tnow) {
    double dx_mm = accum_x / res_x;
    double dy_mm = accum_y / res_y;
    double adx = fabs(dx_mm);
    double ady = fabs(dy_mm);

    if (g_cfg.three_h_mode != THREE_H_OFF &&
        adx >= g_cfg.three_h_thresh_mm && adx > 1.35 * ady) {
        int dir_right = (dx_mm > 0.0);
        int forward = g_cfg.three_h_invert ? !dir_right : dir_right;
        if (get_taskview_pid() > 0) {
            send_key_combo(0, forward ? g_kc_right : g_kc_left);
            *gesture_fired = 1;
        } else if (g_cfg.three_h_mode == THREE_H_SCRUB) {
            XTestFakeKeyEvent(g_dpy, g_kc_alt, True, CurrentTime);
            XFlush(g_dpy);
            g_alt_held = 1;
            g_alt_held_since = tnow;
            if (forward) send_tab_with_alt_held();
            else send_shift_tab_with_alt_held();
            *step_anchor_x = accum_x;
        } else {
            send_alt_tab_once(dir_right);
            *gesture_fired = 1;
        }
    } else if (-dy_mm >= g_cfg.three_v_thresh_mm && ady > 1.35 * adx) {
        *gesture_fired = 1;
        handle_vertical_gesture(1);
    } else if (dy_mm >= g_cfg.three_v_thresh_mm && ady > 1.35 * adx) {
        *gesture_fired = 1;
        handle_vertical_gesture(0);
    }
}

static void print_usage(const char *prog) {
    printf("winpad-gestures %s - Lightweight Windows-style Precision Touchpad daemon for X11\n"
           "Usage: %s [--check | --test-osd | --debug | --version | --help]\n",
           WINPAD_VERSION, prog);
}

int main(int argc, char **argv) {
    int check_only = 0;
    int test_osd = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0 || strcmp(argv[i], "-c") == 0)
            check_only = 1;
        else if (strcmp(argv[i], "--test-osd") == 0)
            test_osd = 1;
        else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0)
            g_debug = 1;
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("winpad-gestures %s\n", WINPAD_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    load_config();

    if (test_osd) {
        g_dpy = XOpenDisplay(NULL);
        if (!g_dpy) return 1;
        XSetErrorHandler(x11_quiet_err);
        init_x11_state();
        int mx = 0, my = 0, mw = 0, mh = 0;
        get_primary_monitor_rect(&mx, &my, &mw, &mh);
        printf("OSD_PRIMARY_MONITOR x=%d y=%d w=%d h=%d argb=%d\n", mx, my, mw, mh, g_osd_argb);
        show_desktop_osd(1, 4);
        usleep(400000);
        hide_osd();
        cleanup_all();
        return 0;
    }

    if (check_only) {
        char path[512] = {0}, name[256] = {0};
        double rx = 0, ry = 0;
        int mf = 0, perm = 0;
        int fd = find_and_open_touchpad(path, sizeof(path), name, sizeof(name), &rx, &ry, &mf, &perm);
        if (fd >= 0) {
            printf("TOUCHPAD_OK path=%s name=\"%s\" max_fingers=%d res_x=%.1f res_y=%.1f\n",
                   path, name, mf, rx, ry);
            close(fd);
            return 0;
        }
        if (perm) {
            fprintf(stderr, "TOUCHPAD_PERM_DENIED: run setup.sh to grant touchpad uaccess.\n");
            return 2;
        }
        fprintf(stderr, "TOUCHPAD_NOT_FOUND\n");
        return 1;
    }

    char lockpath[128];
    snprintf(lockpath, sizeof(lockpath), "/tmp/winpad-gestures-%d.lock", (int)getuid());
    int lockfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd >= 0 && flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "winpad-gestures already running.\n");
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGCHLD, SIG_IGN);
    atexit(cleanup_all);

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "Failed to open X11 DISPLAY.\n");
        return 1;
    }
    XSetErrorHandler(x11_quiet_err);
    init_x11_state();

    while (g_running) {
        if (g_reload_cfg) {
            g_reload_cfg = 0;
            load_config();
        }

        char dev_path[512] = {0}, dev_name[256] = {0};
        double res_x = 30.0, res_y = 30.0;
        int max_f = 0, perm_denied = 0;
        int fd = find_and_open_touchpad(dev_path, sizeof(dev_path), dev_name, sizeof(dev_name),
                                        &res_x, &res_y, &max_f, &perm_denied);
        if (fd < 0) {
            if (perm_denied)
                fprintf(stderr, "Waiting for touchpad permission on /dev/input/event*...\n");
            sleep(2);
            continue;
        }

        fprintf(stderr, "Listening on %s (%s), %d-finger capable (res=%.1f,%.1f)\n",
                dev_path, dev_name, max_f, res_x, res_y);

        Slot slots[MAX_SLOTS];
        for (int s = 0; s < MAX_SLOTS; s++) {
            slots[s].tracking_id = -1;
            slots[s].x = slots[s].y = slots[s].prev_x = slots[s].prev_y = 0;
            slots[s].was_active = 0;
        }

        int cur_slot = 0;
        unsigned int tool_mask = 0;
        int mode_fingers = 0;
        int peak_fingers = 0;
        long drop_pending_since = 0;
        double accum_x = 0.0, accum_y = 0.0;
        double step_anchor_x = 0.0;
        long gesture_start_ms = 0;
        int gesture_fired = 0;
        int two_f_scroll_locked = 0;
        int two_f_triggered = 0;

        while (g_running) {
            if (g_reload_cfg) {
                g_reload_cfg = 0;
                load_config();
            }

            long tnow_loop = now_ms();
            if (g_osd_hide_at > 0 && tnow_loop >= g_osd_hide_at)
                hide_osd();

            int timeout_ms = 1000;
            if (g_alt_held && drop_pending_since > 0) timeout_ms = 25;
            else if (mode_fingers == 3 && !gesture_fired && !g_alt_held) timeout_ms = 20;
            else if (g_osd_hide_at > 0) timeout_ms = 50;

            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) {
                long tnow = now_ms();
                if (g_osd_hide_at > 0 && tnow >= g_osd_hide_at)
                    hide_osd();
                if (mode_fingers == 3 && !gesture_fired && !g_alt_held &&
                    drop_pending_since == 0 && (tnow - gesture_start_ms) >= THREE_F_SETTLE_MS) {
                    evaluate_three_finger_gesture(res_x, res_y, accum_x, accum_y,
                                                  &step_anchor_x, &gesture_fired, tnow);
                }
                if (drop_pending_since > 0 && (tnow - drop_pending_since) >= DROP_HYSTERESIS_MS) {
                    release_alt_if_held();
                    drop_pending_since = 0;
                    if (mode_fingers > 2) mode_fingers = 2;
                }
                if (g_alt_held && (tnow - g_alt_held_since) >= ALT_WATCHDOG_MS)
                    release_alt_if_held();
                reap_children();
                continue;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;

            struct input_event evs[64];
            ssize_t n = read(fd, evs, sizeof(evs));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                break;
            }
            int cnt = (int)(n / sizeof(struct input_event));
            for (int i = 0; i < cnt; i++) {
                struct input_event *ev = &evs[i];
                if (ev->type == EV_ABS) {
                    if (ev->code == ABS_MT_SLOT) {
                        if (ev->value >= 0 && ev->value < MAX_SLOTS)
                            cur_slot = ev->value;
                    } else if (ev->code == ABS_MT_TRACKING_ID) {
                        slots[cur_slot].tracking_id = ev->value;
                        if (ev->value < 0)
                            slots[cur_slot].was_active = 0;
                    } else if (ev->code == ABS_MT_POSITION_X) {
                        slots[cur_slot].x = ev->value;
                    } else if (ev->code == ABS_MT_POSITION_Y) {
                        slots[cur_slot].y = ev->value;
                    }
                } else if (ev->type == EV_KEY) {
                    if (ev->code == BTN_TOOL_FINGER) {
                        if (ev->value) tool_mask |= (1U << 1); else tool_mask &= ~(1U << 1);
                    } else if (ev->code == BTN_TOOL_DOUBLETAP) {
                        if (ev->value) tool_mask |= (1U << 2); else tool_mask &= ~(1U << 2);
                    } else if (ev->code == BTN_TOOL_TRIPLETAP) {
                        if (ev->value) tool_mask |= (1U << 3); else tool_mask &= ~(1U << 3);
                    } else if (ev->code == BTN_TOOL_QUADTAP) {
                        if (ev->value) tool_mask |= (1U << 4); else tool_mask &= ~(1U << 4);
                    } else if (ev->code == BTN_TOOL_QUINTTAP) {
                        if (ev->value) tool_mask |= (1U << 5); else tool_mask &= ~(1U << 5);
                    }
                } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                    int n_active = 0;
                    double sum_dx = 0.0, sum_dy = 0.0;
                    int shared = 0;

                    for (int s = 0; s < MAX_SLOTS; s++) {
                        if (slots[s].tracking_id >= 0) {
                            n_active++;
                            if (slots[s].was_active) {
                                sum_dx += (double)(slots[s].x - slots[s].prev_x);
                                sum_dy += (double)(slots[s].y - slots[s].prev_y);
                                shared++;
                            }
                            slots[s].prev_x = slots[s].x;
                            slots[s].prev_y = slots[s].y;
                            slots[s].was_active = 1;
                        } else {
                            slots[s].was_active = 0;
                        }
                    }

                    int tool_fingers = 0;
                    for (int f = 5; f >= 1; f--) {
                        if (tool_mask & (1U << f)) { tool_fingers = f; break; }
                    }
                    int fingers = n_active > tool_fingers ? n_active : tool_fingers;
                    if (n_active == 0 && tool_fingers == 0) fingers = 0;

                    long tnow = now_ms();

                    if (fingers == 0) {
                        release_alt_if_held();
                        mode_fingers = 0;
                        peak_fingers = 0;
                        drop_pending_since = 0;
                        accum_x = accum_y = 0.0;
                        step_anchor_x = 0.0;
                        gesture_fired = 0;
                        two_f_scroll_locked = 0;
                        two_f_triggered = 0;
                        gesture_start_ms = 0;
                        continue;
                    }

                    if (fingers > peak_fingers)
                        peak_fingers = fingers;

                    /* Finger count transitions with 45ms downward-bounce hysteresis */
                    if (fingers > mode_fingers) {
                        if (g_alt_held)
                            release_alt_if_held();
                        mode_fingers = fingers;
                        drop_pending_since = 0;
                        accum_x = accum_y = 0.0;
                        step_anchor_x = 0.0;
                        gesture_fired = 0;
                        gesture_start_ms = tnow;
                    } else if (fingers < mode_fingers) {
                        if (mode_fingers >= 3) {
                            if (drop_pending_since == 0) {
                                drop_pending_since = tnow;
                            } else if ((tnow - drop_pending_since) >= DROP_HYSTERESIS_MS) {
                                if (g_alt_held && fingers < 3)
                                    release_alt_if_held();
                                mode_fingers = fingers;
                                drop_pending_since = 0;
                                accum_x = accum_y = 0.0;
                                step_anchor_x = 0.0;
                                gesture_fired = 0;
                                gesture_start_ms = tnow;
                            }
                        } else {
                            mode_fingers = fingers;
                            accum_x = accum_y = 0.0;
                            gesture_start_ms = tnow;
                        }
                    } else {
                        drop_pending_since = 0;
                    }

                    if (shared > 0) {
                        double fdx = sum_dx / (double)shared;
                        double fdy = sum_dy / (double)shared;
                        accum_x += fdx;
                        accum_y += fdy;
                    }

                    double dx_mm = accum_x / res_x;
                    double dy_mm = accum_y / res_y;
                    double adx = fabs(dx_mm);
                    double ady = fabs(dy_mm);

                    /* 2-finger horizontal flick -> Back / Forward (guarded against scroll) */
                    if (mode_fingers == 2 && peak_fingers == 2 && g_cfg.two_f_mode != TWO_F_OFF &&
                        !two_f_scroll_locked && !two_f_triggered) {
                        if (ady >= 5.5) {
                            two_f_scroll_locked = 1;
                        } else if (adx >= g_cfg.two_f_min_mm) {
                            long dt = tnow - gesture_start_ms;
                            if (adx > 2.5 * ady && dt <= g_cfg.two_f_max_ms) {
                                two_f_triggered = 1;
                                int is_right = (dx_mm > 0.0);
                                int is_back = (g_cfg.two_f_mode == TWO_F_NATURAL) ? is_right : !is_right;
                                if (g_debug) fprintf(stderr, "DEBUG: 2F %s\n", is_back ? "Back" : "Forward");
                                trigger_back_forward(is_back);
                            } else {
                                two_f_scroll_locked = 1;
                            }
                        }
                    }

                    /* 3-finger gestures -> Alt+Tab scrub (L/R), Windows state cycle (Up/Down) */
                    else if (mode_fingers == 3) {
                        if (g_alt_held) {
                            double step_dx_mm = (accum_x - step_anchor_x) / res_x;
                            if (step_dx_mm >= g_cfg.three_h_step_mm) {
                                if (g_cfg.three_h_invert) send_shift_tab_with_alt_held();
                                else send_tab_with_alt_held();
                                step_anchor_x = accum_x;
                            } else if (step_dx_mm <= -g_cfg.three_h_step_mm) {
                                if (g_cfg.three_h_invert) send_tab_with_alt_held();
                                else send_shift_tab_with_alt_held();
                                step_anchor_x = accum_x;
                            }
                        } else if (!gesture_fired && (tnow - gesture_start_ms) >= THREE_F_SETTLE_MS) {
                            evaluate_three_finger_gesture(res_x, res_y, accum_x, accum_y,
                                                          &step_anchor_x, &gesture_fired, tnow);
                        }
                    }

                    /* 4-finger gestures -> Switch Virtual Desktop (L/R), Windows state cycle (Up/Down) */
                    else if (mode_fingers == 4 && !gesture_fired) {
                        if (g_cfg.four_h_mode != FOUR_H_OFF &&
                            adx >= g_cfg.four_h_thresh_mm && adx > 1.35 * ady) {
                            gesture_fired = 1;
                            int dir_right = (dx_mm > 0.0);
                            if (g_cfg.four_h_mode == FOUR_H_ALTTAB) {
                                send_alt_tab_once(dir_right);
                            } else {
                                int delta = (g_cfg.four_h_mode == FOUR_H_WORKSPACE_NATURAL)
                                                ? (dir_right ? -1 : 1)
                                                : (dir_right ? 1 : -1);
                                if (!switch_workspace(delta))
                                    send_alt_tab_once(dir_right);
                            }
                        } else if (-dy_mm >= g_cfg.four_v_thresh_mm && ady > 1.35 * adx) {
                            gesture_fired = 1;
                            handle_vertical_gesture(1);
                        } else if (dy_mm >= g_cfg.four_v_thresh_mm && ady > 1.35 * adx) {
                            gesture_fired = 1;
                            handle_vertical_gesture(0);
                        }
                    }
                }
            }
        }

        release_alt_if_held();
        close(fd);
        if (g_running) sleep(1);
    }

    cleanup_all();
    if (lockfd >= 0) close(lockfd);
    return 0;
}
