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

#define WINPAD_VERSION "1.5.0"
#define MAX_SLOTS 16
#define ALT_WATCHDOG_MS 15000L
#define THREE_F_SETTLE_MS 22L
#define LANDING_MERGE_MS 55L
#define DROP_HYSTERESIS_MS 45L
#define OSD_DURATION_MS 820L
#define OSD_FADE_MS 140L
#define OSD_FRAME_MS 15L
#define OSD_W 320
#define OSD_H 84
#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

typedef enum {
    TWO_F_OFF = 0,
    TWO_F_NATURAL = 1,
    TWO_F_ARROW = 2
} TwoFingerMode;

typedef enum {
    THREE_H_OFF = 0,
    THREE_H_SCRUB = 1,
    THREE_H_ONCE = 2
} ThreeHorizMode;

typedef enum {
    FOUR_H_OFF = 0,
    FOUR_H_WORKSPACE_DIRECT = 1,
    FOUR_H_WORKSPACE_NATURAL = 2,
    FOUR_H_ALTTAB = 3
} FourHorizMode;

typedef enum {
    THREE_TAP_OFF = 0,
    THREE_TAP_SUPER = 1
} ThreeTapMode;

typedef enum {
    FOUR_TAP_OFF = 0,
    FOUR_TAP_PLAY_PAUSE = 1,
    FOUR_TAP_NOTIFICATIONS = 2,
    FOUR_TAP_TASKVIEW = 3,
    FOUR_TAP_SHOW_DESKTOP = 4,
    FOUR_TAP_SUPER = 5
} FourTapMode;

typedef struct {
    char device[512];
    char taskview_cmd[512];
    char three_tap_cmd[512];
    char four_tap_cmd[512];
    TwoFingerMode two_f_mode;
    double two_f_min_mm;
    long two_f_max_ms;
    ThreeHorizMode three_h_mode;
    int three_h_invert;
    double three_h_thresh_mm;
    double three_h_step_mm;
    double three_v_thresh_mm;
    ThreeTapMode three_tap_mode;
    long three_tap_max_ms;
    double three_tap_max_mm;
    FourHorizMode four_h_mode;
    int four_h_scrub;
    double four_h_thresh_mm;
    double four_h_step_mm;
    double four_v_thresh_mm;
    FourTapMode four_tap_mode;
    long four_tap_max_ms;
    double four_tap_max_mm;
    int edge_drag_slide;
    double edge_drag_margin_mm;
    double edge_drag_speed;
} Config;

typedef struct {
    char path[512];
    char name[256];
    double res_x;
    double res_y;
    int max_fingers;
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
} TouchpadInfo;

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
static int g_alt_tab_idx = 0;
static int g_alt_tab_count = 0;
static int g_xfwm_cycle_workspaces = 0;
static int g_xfwm_cycle_minimized = 1;
static char g_tp_name_muted[256] = {0};

/* Animated Fluent Desktop OSD state */
static Window g_osd_win = None;
static Colormap g_osd_cmap = None;
static Visual *g_osd_visual = NULL;
static cairo_surface_t *g_osd_xsurf = NULL;
static cairo_surface_t *g_osd_back = NULL;
static int g_osd_argb = 0;
static int g_osd_visible = 0;
static long g_osd_shown_at = 0;
static long g_osd_hide_at = 0;
static long g_osd_last_render_ms = 0;
static long g_osd_total = 4;
static long g_osd_target_idx = 0;
static double g_osd_anim_pos = 0.0;

static KeyCode g_kc_alt = 0;
static KeyCode g_kc_shift = 0;
static KeyCode g_kc_tab = 0;
static KeyCode g_kc_super = 0;
static KeyCode g_kc_audio_play = 0;
static KeyCode g_kc_left = 0;
static KeyCode g_kc_right = 0;
static KeyCode g_kc_back = 0;
static KeyCode g_kc_fwd = 0;

static Atom g_atom_showing_desktop = None;
static Atom g_atom_current_desktop = None;
static Atom g_atom_num_desktops = None;
static Atom g_atom_active_window = None;
static Atom g_atom_client_list = None;
static Atom g_atom_wm_desktop = None;
static Atom g_atom_wm_state = None;
static Atom g_atom_wm_state_skip_tb = None;
static Atom g_atom_wm_state_hidden = None;
static Atom g_atom_wm_type = None;
static Atom g_atom_wm_type_normal = None;
static Atom g_atom_wm_type_dialog = None;
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
    if (g_osd_win != None && g_dpy && g_osd_visible) {
        XUnmapWindow(g_dpy, g_osd_win);
        XFlush(g_dpy);
        g_osd_visible = 0;
        g_osd_hide_at = 0;
        g_osd_last_render_ms = 0;
    }
}

static void release_alt_if_held(void) {
    if (g_alt_held && g_dpy && g_kc_alt) {
        XTestFakeKeyEvent(g_dpy, g_kc_alt, False, CurrentTime);
        XFlush(g_dpy);
        g_alt_held = 0;
        g_alt_held_since = 0;
    }
    g_alt_tab_idx = 0;
    g_alt_tab_count = 0;
}

static void sync_touchpad_middle_click(const char *dev_name, int mute) {
    if (!dev_name || !dev_name[0]) return;
    if (!mute && strcmp(g_tp_name_muted, dev_name) != 0) return;
    if (mute && strcmp(g_tp_name_muted, dev_name) == 0) return;

    static const char *script =
        "command -v xinput >/dev/null 2>&1 || exit 0\n"
        "dev=\"$1\"; b2=\"$2\"\n"
        "if map=$(xinput get-button-map \"pointer:$dev\" 2>/dev/null); then\n"
        "  dev=\"pointer:$dev\"\n"
        "else\n"
        "  map=$(xinput get-button-map \"$dev\" 2>/dev/null) || exit 0\n"
        "fi\n"
        "set -- $map\n"
        "[ $# -ge 3 ] || exit 0\n"
        "i=1; out=\"\"\n"
        "for b in \"$@\"; do\n"
        "  [ \"$i\" -eq 2 ] && out=\"$out $b2\" || out=\"$out $b\"\n"
        "  i=$((i + 1))\n"
        "done\n"
        "exec xinput set-button-map \"$dev\" $out 2>/dev/null\n";

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        setsid();
        execl("/bin/sh", "sh", "-c", script, "sh", dev_name, mute ? "0" : "2", (char *)NULL);
        _exit(127);
    }

    if (mute)
        snprintf(g_tp_name_muted, sizeof(g_tp_name_muted), "%s", dev_name);
    else
        g_tp_name_muted[0] = '\0';
}

static void cleanup_all(void) {
    release_alt_if_held();
    if (g_tp_name_muted[0])
        sync_touchpad_middle_click(g_tp_name_muted, 0);
    if (g_osd_back) {
        cairo_surface_destroy(g_osd_back);
        g_osd_back = NULL;
    }
    if (g_osd_xsurf) {
        cairo_surface_destroy(g_osd_xsurf);
        g_osd_xsurf = NULL;
    }
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

static int parse_bool(const char *v) {
    return (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
            strcmp(v, "yes") == 0 || strcmp(v, "on") == 0);
}

static void load_xfwm4_cycle_prefs(void) {
    g_xfwm_cycle_workspaces = 0;
    g_xfwm_cycle_minimized = 1;

    char path[512];
    expand_home("~/.config/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "name=\"cycle_workspaces\"")) {
            if (strstr(line, "value=\"true\"")) g_xfwm_cycle_workspaces = 1;
            else if (strstr(line, "value=\"false\"")) g_xfwm_cycle_workspaces = 0;
        } else if (strstr(line, "name=\"cycle_minimized\"")) {
            if (strstr(line, "value=\"false\"")) g_xfwm_cycle_minimized = 0;
            else if (strstr(line, "value=\"true\"")) g_xfwm_cycle_minimized = 1;
        }
    }
    fclose(f);
}

static void load_config(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.device, sizeof(g_cfg.device), "auto");
    expand_home("~/.local/bin/winpad-taskview", g_cfg.taskview_cmd, sizeof(g_cfg.taskview_cmd));
    g_cfg.two_f_mode = TWO_F_NATURAL;
    g_cfg.two_f_min_mm = 14.0;
    g_cfg.two_f_max_ms = 360;
    g_cfg.three_h_mode = THREE_H_SCRUB;
    g_cfg.three_h_invert = 0;
    g_cfg.three_h_thresh_mm = 6.5;
    g_cfg.three_h_step_mm = 11.5;
    g_cfg.three_v_thresh_mm = 7.5;
    g_cfg.three_tap_mode = THREE_TAP_SUPER;
    g_cfg.three_tap_max_ms = 260;
    g_cfg.three_tap_max_mm = 3.5;
    g_cfg.four_h_mode = FOUR_H_WORKSPACE_DIRECT;
    g_cfg.four_h_scrub = 0;
    g_cfg.four_h_thresh_mm = 7.5;
    g_cfg.four_h_step_mm = 15.0;
    g_cfg.four_v_thresh_mm = 8.0;
    g_cfg.four_tap_mode = FOUR_TAP_PLAY_PAUSE;
    g_cfg.four_tap_max_ms = 260;
    g_cfg.four_tap_max_mm = 3.5;
    g_cfg.edge_drag_slide = 1;
    g_cfg.edge_drag_margin_mm = 6.0;
    g_cfg.edge_drag_speed = 460.0;

    load_xfwm4_cycle_prefs();

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
            if (val >= 3.0 && val <= 80.0) g_cfg.three_h_thresh_mm = val;
        } else if (strcmp(k, "three_finger_v_thresh_mm") == 0) {
            double val = atof(v);
            if (val >= 3.0 && val <= 80.0) g_cfg.three_v_thresh_mm = val;
        } else if (strcmp(k, "three_finger_step_mm") == 0) {
            double val = atof(v);
            if (val >= 5.0 && val <= 80.0) g_cfg.three_h_step_mm = val;
        } else if (strcmp(k, "three_finger_tap") == 0) {
            if (strcmp(v, "off") == 0 || strcmp(v, "middle") == 0)
                g_cfg.three_tap_mode = THREE_TAP_OFF;
            else
                g_cfg.three_tap_mode = THREE_TAP_SUPER;
        } else if (strcmp(k, "three_finger_tap_cmd") == 0) {
            expand_home(v, g_cfg.three_tap_cmd, sizeof(g_cfg.three_tap_cmd));
        } else if (strcmp(k, "three_finger_tap_max_ms") == 0) {
            long val = atol(v);
            if (val >= 80 && val <= 800) g_cfg.three_tap_max_ms = val;
        } else if (strcmp(k, "three_finger_tap_max_mm") == 0) {
            double val = atof(v);
            if (val >= 1.0 && val <= 15.0) g_cfg.three_tap_max_mm = val;
        } else if (strcmp(k, "four_finger_horiz") == 0) {
            if (strcmp(v, "off") == 0) g_cfg.four_h_mode = FOUR_H_OFF;
            else if (strcmp(v, "workspace_natural") == 0) g_cfg.four_h_mode = FOUR_H_WORKSPACE_NATURAL;
            else if (strcmp(v, "alt_tab") == 0) g_cfg.four_h_mode = FOUR_H_ALTTAB;
            else g_cfg.four_h_mode = FOUR_H_WORKSPACE_DIRECT;
        } else if (strcmp(k, "four_finger_scrub") == 0) {
            g_cfg.four_h_scrub = parse_bool(v);
        } else if (strcmp(k, "four_finger_thresh_mm") == 0) {
            double val = atof(v);
            if (val >= 3.0 && val <= 80.0) {
                g_cfg.four_h_thresh_mm = val;
                g_cfg.four_v_thresh_mm = val + 0.5;
            }
        } else if (strcmp(k, "four_finger_v_thresh_mm") == 0) {
            double val = atof(v);
            if (val >= 3.0 && val <= 80.0) g_cfg.four_v_thresh_mm = val;
        } else if (strcmp(k, "four_finger_step_mm") == 0) {
            double val = atof(v);
            if (val >= 6.0 && val <= 80.0) g_cfg.four_h_step_mm = val;
        } else if (strcmp(k, "four_finger_tap") == 0) {
            if (strcmp(v, "off") == 0) g_cfg.four_tap_mode = FOUR_TAP_OFF;
            else if (strcmp(v, "notifications") == 0) g_cfg.four_tap_mode = FOUR_TAP_NOTIFICATIONS;
            else if (strcmp(v, "taskview") == 0) g_cfg.four_tap_mode = FOUR_TAP_TASKVIEW;
            else if (strcmp(v, "show_desktop") == 0) g_cfg.four_tap_mode = FOUR_TAP_SHOW_DESKTOP;
            else if (strcmp(v, "super") == 0) g_cfg.four_tap_mode = FOUR_TAP_SUPER;
            else g_cfg.four_tap_mode = FOUR_TAP_PLAY_PAUSE;
        } else if (strcmp(k, "four_finger_tap_cmd") == 0) {
            expand_home(v, g_cfg.four_tap_cmd, sizeof(g_cfg.four_tap_cmd));
        } else if (strcmp(k, "four_finger_tap_max_ms") == 0) {
            long val = atol(v);
            if (val >= 80 && val <= 800) g_cfg.four_tap_max_ms = val;
        } else if (strcmp(k, "four_finger_tap_max_mm") == 0) {
            double val = atof(v);
            if (val >= 1.0 && val <= 15.0) g_cfg.four_tap_max_mm = val;
        } else if (strcmp(k, "edge_drag_slide") == 0) {
            g_cfg.edge_drag_slide = parse_bool(v);
        } else if (strcmp(k, "edge_drag_margin_mm") == 0) {
            double val = atof(v);
            if (val >= 2.0 && val <= 25.0) g_cfg.edge_drag_margin_mm = val;
        } else if (strcmp(k, "edge_drag_speed") == 0) {
            double val = atof(v);
            if (val >= 80.0 && val <= 2000.0) g_cfg.edge_drag_speed = val;
        }
    }
    fclose(f);
}

static int is_touchpad_fd(int fd, TouchpadInfo *tp) {
    unsigned long evbits[NBITS(EV_MAX)] = {0};
    unsigned long absbits[NBITS(ABS_MAX)] = {0};
    unsigned long keybits[NBITS(KEY_MAX)] = {0};
    unsigned long propbits[NBITS(INPUT_PROP_MAX)] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return 0;
    if (!TEST_BIT(EV_ABS, evbits) || !TEST_BIT(EV_KEY, evbits)) return 0;

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return 0;
    ioctl(fd, EVIOCGPROP(sizeof(propbits)), propbits);

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

    tp->max_fingers = maxf;
    tp->res_x = rx;
    tp->res_y = ry;
    tp->min_x = ax.minimum;
    tp->max_x = ax.maximum;
    tp->min_y = ay.minimum;
    tp->max_y = ay.maximum;
    return 1;
}

static int find_and_open_touchpad(TouchpadInfo *tp, int *saw_perm_denied) {
    *saw_perm_denied = 0;
    memset(tp, 0, sizeof(*tp));

    if (strcmp(g_cfg.device, "auto") != 0) {
        int fd = open(g_cfg.device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES) *saw_perm_denied = 1;
            return -1;
        }
        TouchpadInfo cand = {0};
        if (is_touchpad_fd(fd, &cand)) {
            snprintf(cand.path, sizeof(cand.path), "%s", g_cfg.device);
            if (ioctl(fd, EVIOCGNAME(sizeof(cand.name)), cand.name) < 0)
                snprintf(cand.name, sizeof(cand.name), "Unknown Touchpad");
            *tp = cand;
            return fd;
        }
        close(fd);
        return -1;
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    int best_fd = -1;
    TouchpadInfo best = {0};

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
        TouchpadInfo cand = {0};
        if (is_touchpad_fd(fd, &cand)) {
            if (ioctl(fd, EVIOCGNAME(sizeof(cand.name)), cand.name) < 0)
                snprintf(cand.name, sizeof(cand.name), "Touchpad");
            if (cand.max_fingers > best.max_fingers) {
                if (best_fd >= 0) close(best_fd);
                best_fd = fd;
                snprintf(cand.path, sizeof(cand.path), "%s", p);
                best = cand;
            } else {
                close(fd);
            }
        } else {
            close(fd);
        }
    }
    closedir(dir);

    if (best_fd >= 0)
        *tp = best;
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
        g_osd_xsurf = cairo_xlib_surface_create(g_dpy, g_osd_win, g_osd_visual, OSD_W, OSD_H);
        g_osd_back = cairo_image_surface_create(g_osd_argb ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
                                                OSD_W, OSD_H);
    }
}

static void init_x11_state(void) {
    g_root = DefaultRootWindow(g_dpy);

    g_kc_alt   = XKeysymToKeycode(g_dpy, XK_Alt_L);
    g_kc_shift = XKeysymToKeycode(g_dpy, XK_Shift_L);
    g_kc_tab   = XKeysymToKeycode(g_dpy, XK_Tab);
    g_kc_super = XKeysymToKeycode(g_dpy, XK_Super_L);
    g_kc_left  = XKeysymToKeycode(g_dpy, XK_Left);
    g_kc_right = XKeysymToKeycode(g_dpy, XK_Right);
#ifdef XF86XK_Back
    g_kc_back  = XKeysymToKeycode(g_dpy, XF86XK_Back);
    g_kc_fwd   = XKeysymToKeycode(g_dpy, XF86XK_Forward);
#endif
#ifdef XF86XK_AudioPlay
    g_kc_audio_play = XKeysymToKeycode(g_dpy, XF86XK_AudioPlay);
#endif

    g_atom_showing_desktop  = XInternAtom(g_dpy, "_NET_SHOWING_DESKTOP", False);
    g_atom_current_desktop  = XInternAtom(g_dpy, "_NET_CURRENT_DESKTOP", False);
    g_atom_num_desktops     = XInternAtom(g_dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    g_atom_active_window    = XInternAtom(g_dpy, "_NET_ACTIVE_WINDOW", False);
    g_atom_client_list      = XInternAtom(g_dpy, "_NET_CLIENT_LIST", False);
    g_atom_wm_desktop       = XInternAtom(g_dpy, "_NET_WM_DESKTOP", False);
    g_atom_wm_state         = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    g_atom_wm_state_skip_tb = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    g_atom_wm_state_hidden  = XInternAtom(g_dpy, "_NET_WM_STATE_HIDDEN", False);
    g_atom_wm_type          = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    g_atom_wm_type_normal   = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    g_atom_wm_type_dialog   = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    g_atom_wm_type_notif    = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);

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

static void render_osd_frame(long tnow) {
    if (g_osd_win == None || !g_dpy || !g_osd_visible || !g_osd_xsurf || !g_osd_back) return;

    int tiles = (int)(g_osd_total > 8 ? 8 : (g_osd_total < 1 ? 1 : g_osd_total));
    long clamped_target = g_osd_target_idx;
    if (clamped_target >= tiles) clamped_target = tiles - 1;
    if (clamped_target < 0) clamped_target = 0;

    /* Smooth time-compensated glide towards target index + fade-in / fade-out */
    long dt_frame = (g_osd_last_render_ms > 0 && tnow > g_osd_last_render_ms)
                        ? (tnow - g_osd_last_render_ms) : 16L;
    if (dt_frame > 60L) dt_frame = 60L;
    g_osd_last_render_ms = tnow;
    double blend = 1.0 - exp(-0.032 * (double)dt_frame);
    g_osd_anim_pos += ((double)clamped_target - g_osd_anim_pos) * blend;
    if (fabs((double)clamped_target - g_osd_anim_pos) < 0.015)
        g_osd_anim_pos = (double)clamped_target;

    double alpha = 1.0;
    if (g_osd_argb) {
        long dt_in = tnow - g_osd_shown_at;
        long dt_rem = g_osd_hide_at - tnow;
        if (dt_in < 70) alpha = 0.35 + 0.65 * ((double)dt_in / 70.0);
        if (dt_rem < OSD_FADE_MS) alpha *= (double)(dt_rem > 0 ? dt_rem : 0) / (double)OSD_FADE_MS;
        if (alpha < 0.0) alpha = 0.0;
        if (alpha > 1.0) alpha = 1.0;
    }

    /* Double-buffered offscreen draw -> single zero-flicker blit */
    cairo_t *cr = cairo_create(g_osd_back);

    if (g_osd_argb) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    }

    double radius = g_osd_argb ? 20.0 : 0.0;
    rounded_rect(cr, 1.0, 1.0, OSD_W - 2.0, OSD_H - 2.0, radius);
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, OSD_W, OSD_H);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.078, 0.098, 0.155, 0.94 * alpha);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.055, 0.068, 0.115, 0.96 * alpha);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    cairo_pattern_t *brd = cairo_pattern_create_linear(0, 0, OSD_W, 0);
    cairo_pattern_add_color_stop_rgba(brd, 0.0, 0.49, 0.81, 1.0, 0.50 * alpha);
    cairo_pattern_add_color_stop_rgba(brd, 0.5, 0.48, 0.64, 0.97, 0.58 * alpha);
    cairo_pattern_add_color_stop_rgba(brd, 1.0, 0.73, 0.60, 0.97, 0.50 * alpha);
    cairo_set_source(cr, brd);
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);
    cairo_pattern_destroy(brd);

    char title[32], sub[16];
    snprintf(title, sizeof(title), "Desktop %ld", g_osd_target_idx + 1);
    snprintf(sub, sizeof(sub), "/ %ld", g_osd_total);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14.5);
    cairo_text_extents_t te, se;
    cairo_text_extents(cr, title, &te);
    cairo_set_font_size(cr, 12.0);
    cairo_text_extents(cr, sub, &se);

    double hdr_w = te.x_advance + 6.0 + se.x_advance;
    double hx = (OSD_W - hdr_w) * 0.5;
    cairo_set_font_size(cr, 14.5);
    cairo_set_source_rgba(cr, 0.96, 0.97, 1.0, alpha);
    cairo_move_to(cr, hx, 26.0);
    cairo_show_text(cr, title);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0);
    cairo_set_source_rgba(cr, 0.62, 0.70, 0.84, 0.90 * alpha);
    cairo_move_to(cr, hx + te.x_advance + 6.0, 26.0);
    cairo_show_text(cr, sub);

    /* Auto-fit tile width up to 8 desktops so indicator never overflows OSD_W */
    double gap = tiles > 5 ? 6.0 : 10.0;
    double max_row = OSD_W - 36.0;
    double tile_w = (max_row - (tiles - 1) * gap) / (double)tiles;
    if (tile_w > 54.0) tile_w = 54.0;
    double tile_h = 28.0;
    double row_w = tiles * tile_w + (tiles - 1) * gap;
    double sx = (OSD_W - row_w) * 0.5;
    double sy = 39.0;

    for (int i = 0; i < tiles; i++) {
        double tx = sx + i * (tile_w + gap);
        rounded_rect(cr, tx, sy, tile_w, tile_h, 7.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (i == clamped_target ? 0.14 : 0.06) * alpha);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10 * alpha);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        char nbuf[8];
        snprintf(nbuf, sizeof(nbuf), "%d", i + 1);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.5);
        cairo_text_extents_t ne;
        cairo_text_extents(cr, nbuf, &ne);
        cairo_set_source_rgba(cr, 0.85, 0.90, 1.0, (i == clamped_target ? 0.98 : 0.45) * alpha);
        cairo_move_to(cr, tx + (tile_w - ne.width) * 0.5 - ne.x_bearing,
                      sy + (tile_h + ne.height) * 0.5);
        cairo_show_text(cr, nbuf);
    }

    double sel_x = sx + g_osd_anim_pos * (tile_w + gap);
    rounded_rect(cr, sel_x, sy, tile_w, tile_h, 7.0);
    cairo_pattern_t *sel_fill = cairo_pattern_create_linear(sel_x, sy, sel_x + tile_w, sy + tile_h);
    cairo_pattern_add_color_stop_rgba(sel_fill, 0.0, 0.49, 0.81, 1.0, 0.22 * alpha);
    cairo_pattern_add_color_stop_rgba(sel_fill, 1.0, 0.73, 0.60, 0.97, 0.22 * alpha);
    cairo_set_source(cr, sel_fill);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(sel_fill);

    cairo_pattern_t *sel_brd = cairo_pattern_create_linear(sel_x, sy, sel_x + tile_w, sy);
    cairo_pattern_add_color_stop_rgba(sel_brd, 0.0, 0.49, 0.81, 1.0, 0.95 * alpha);
    cairo_pattern_add_color_stop_rgba(sel_brd, 1.0, 0.73, 0.60, 0.97, 0.95 * alpha);
    cairo_set_source(cr, sel_brd);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(sel_brd);

    cairo_destroy(cr);

    cairo_t *xcr = cairo_create(g_osd_xsurf);
    cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(xcr, g_osd_back, 0.0, 0.0);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    XFlush(g_dpy);
}

static void show_desktop_osd(long prev_idx, long cur_idx, long total) {
    if (g_osd_win == None || !g_dpy) return;
    long tnow = now_ms();

    if (!g_osd_visible) {
        int mx = 0, my = 0, mw = 1920, mh = 1080;
        get_primary_monitor_rect(&mx, &my, &mw, &mh);
        int x = mx + (mw - OSD_W) / 2;
        int y = my + mh - OSD_H - 72;
        XMoveResizeWindow(g_dpy, g_osd_win, x, y, OSD_W, OSD_H);
        XMapRaised(g_dpy, g_osd_win);
        g_osd_visible = 1;
        g_osd_shown_at = tnow;
        g_osd_anim_pos = (double)prev_idx;
    }

    g_osd_total = total;
    g_osd_target_idx = cur_idx;
    g_osd_hide_at = tnow + OSD_DURATION_MS;
    render_osd_frame(tnow);
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

static int is_cyclable_window(Window w, unsigned long cur_desk) {
    if (!g_xfwm_cycle_workspaces && g_atom_wm_desktop != None) {
        Atom atype;
        int afmt;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(g_dpy, w, g_atom_wm_desktop, 0, 1, False,
                               XA_CARDINAL, &atype, &afmt, &nitems,
                               &bytes_after, &data) == Success) {
            int on_desk = 1;
            if (data && nitems >= 1 && afmt == 32) {
                unsigned long d = *(unsigned long *)data;
                if (d != cur_desk && (d & 0xFFFFFFFFUL) != 0xFFFFFFFFUL)
                    on_desk = 0;
            }
            if (data) XFree(data);
            if (!on_desk) return 0;
        }
    }

    int is_dialog_only = 0;
    if (g_atom_wm_type != None) {
        Atom atype;
        int afmt;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(g_dpy, w, g_atom_wm_type, 0, 16, False,
                               XA_ATOM, &atype, &afmt, &nitems,
                               &bytes_after, &data) == Success) {
            if (data && nitems >= 1 && afmt == 32) {
                Atom *atoms = (Atom *)data;
                int has_normal = 0, has_dialog = 0;
                for (unsigned long i = 0; i < nitems; i++) {
                    if (atoms[i] == g_atom_wm_type_normal) has_normal = 1;
                    else if (atoms[i] == g_atom_wm_type_dialog) has_dialog = 1;
                }
                if (!has_normal && !has_dialog) {
                    XFree(data);
                    return 0;
                }
                is_dialog_only = (!has_normal && has_dialog);
            }
            if (data) XFree(data);
        }
    }

    if (is_dialog_only) {
        Window transient_for = None;
        if (XGetTransientForHint(g_dpy, w, &transient_for) &&
            transient_for != None && transient_for != g_root) {
            return 0;
        }
    }

    if (g_atom_wm_state != None) {
        Atom atype;
        int afmt;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(g_dpy, w, g_atom_wm_state, 0, 32, False,
                               XA_ATOM, &atype, &afmt, &nitems,
                               &bytes_after, &data) == Success) {
            if (data && nitems >= 1 && afmt == 32) {
                Atom *atoms = (Atom *)data;
                for (unsigned long i = 0; i < nitems; i++) {
                    if (atoms[i] == g_atom_wm_state_skip_tb) {
                        XFree(data);
                        return 0;
                    }
                    if (!g_xfwm_cycle_minimized && atoms[i] == g_atom_wm_state_hidden) {
                        XFree(data);
                        return 0;
                    }
                }
            }
            if (data) XFree(data);
        }
    }

    return 1;
}

static int count_cyclable_windows(int *out_active_in_list) {
    if (out_active_in_list) *out_active_in_list = 0;
    if (!g_dpy || g_atom_client_list == None) return -1;
    long cur_desk_l = 0;
    get_ewmh_cardinal(g_atom_current_desktop, &cur_desk_l);
    unsigned long cur_desk = (unsigned long)cur_desk_l;

    Window active_win = None;
    if (g_atom_active_window != None) {
        Atom act_type;
        int act_fmt;
        unsigned long act_n = 0, act_after = 0;
        unsigned char *act_data = NULL;
        if (XGetWindowProperty(g_dpy, g_root, g_atom_active_window, 0, 1, False,
                               XA_WINDOW, &act_type, &act_fmt, &act_n,
                               &act_after, &act_data) == Success) {
            if (act_data && act_n >= 1 && act_fmt == 32)
                active_win = *(Window *)act_data;
            if (act_data) XFree(act_data);
        }
    }

    Atom atype;
    int afmt;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(g_dpy, g_root, g_atom_client_list, 0, 1024, False,
                           XA_WINDOW, &atype, &afmt, &nitems,
                           &bytes_after, &data) != Success) {
        return -1;
    }
    if (!data || afmt != 32) {
        if (data) XFree(data);
        return -1;
    }

    Window *wins = (Window *)data;
    int count = 0;
    for (unsigned long i = 0; i < nitems; i++) {
        if (wins[i] != None && wins[i] != g_osd_win && is_cyclable_window(wins[i], cur_desk)) {
            if (out_active_in_list && wins[i] == active_win)
                *out_active_in_list = 1;
            count++;
        }
    }
    XFree(data);
    return count;
}

static void step_alt_tab(int delta) {
    if (delta > 0) {
        if (g_alt_tab_count < 0 || g_alt_tab_idx + 1 < g_alt_tab_count) {
            g_alt_tab_idx++;
            send_tab_with_alt_held();
        }
    } else if (delta < 0) {
        if (g_alt_tab_count < 0 || g_alt_tab_idx > 0) {
            if (g_alt_tab_idx > 0) g_alt_tab_idx--;
            send_shift_tab_with_alt_held();
        }
    }
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
            show_desktop_osd(cur, target, count);
    } else {
        if (get_taskview_pid() == 0)
            show_desktop_osd(cur, cur, count);
    }
    return 1;
}

static void spawn_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static int is_clean_tap(int target_fingers, int peak_fingers, int blocked,
                        long dt_ms, long max_ms,
                        double travel_mm, double net_disp_mm, double max_mm) {
    return (!blocked && peak_fingers == target_fingers &&
            dt_ms > 0 && dt_ms <= max_ms &&
            travel_mm <= max_mm && net_disp_mm <= max_mm);
}

static void trigger_four_finger_tap(void) {
    if (g_cfg.four_tap_cmd[0]) {
        spawn_cmd(g_cfg.four_tap_cmd);
        return;
    }
    switch (g_cfg.four_tap_mode) {
    case FOUR_TAP_PLAY_PAUSE:
        if (g_kc_audio_play)
            send_key_combo(0, g_kc_audio_play);
        else
            spawn_cmd("playerctl play-pause 2>/dev/null || true");
        break;
    case FOUR_TAP_NOTIFICATIONS:
        spawn_cmd("xfce4-notifyd-config");
        break;
    case FOUR_TAP_TASKVIEW:
        spawn_cmd(g_cfg.taskview_cmd);
        break;
    case FOUR_TAP_SHOW_DESKTOP: {
        long showing = 0;
        get_ewmh_cardinal(g_atom_showing_desktop, &showing);
        send_ewmh_root_msg(g_atom_showing_desktop, showing ? 0 : 1, 0);
        break;
    }
    case FOUR_TAP_SUPER:
        if (g_kc_super)
            send_key_combo(0, g_kc_super);
        break;
    case FOUR_TAP_OFF:
    default:
        break;
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
        strstr(cls, "urxvt") || strstr(cls, "tilix") || strstr(cls, "terminator") ||
        strstr(cls, "guake") || strstr(cls, "tilda")) {
        return;
    }

    if (strstr(cls, "code") || strstr(cls, "codium") || strstr(cls, "obsidian") ||
        strstr(cls, "jetbrains") || strstr(cls, "antigravity") || strstr(cls, "cursor") ||
        strstr(cls, "zed") || strstr(cls, "sublime")) {
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

static int is_pointer_btn1_pressed(void) {
    if (!g_dpy || g_root == None) return 0;
    Window rw = None, cw = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (XQueryPointer(g_dpy, g_root, &rw, &cw, &rx, &ry, &wx, &wy, &mask))
        return (mask & Button1Mask) != 0;
    return 0;
}

static void tick_edge_drag(long tnow, int btn_left_down, int *edge_sliding,
                           double edge_vx, double edge_vy,
                           double *edge_rem_x, double *edge_rem_y,
                           long *edge_last_tick_ms) {
    if (!*edge_sliding || !g_dpy) return;
    if (tnow - *edge_last_tick_ms < 15L) return;

    if (!btn_left_down && !is_pointer_btn1_pressed()) {
        *edge_sliding = 0;
        *edge_rem_x = 0.0;
        *edge_rem_y = 0.0;
        return;
    }

    long dt = tnow - *edge_last_tick_ms;
    if (dt <= 0) dt = 16L;
    if (dt > 60L) dt = 60L;
    *edge_last_tick_ms = tnow;

    double dt_s = (double)dt / 1000.0;
    *edge_rem_x += edge_vx * dt_s;
    *edge_rem_y += edge_vy * dt_s;
    int mx = (int)trunc(*edge_rem_x);
    int my = (int)trunc(*edge_rem_y);
    *edge_rem_x -= (double)mx;
    *edge_rem_y -= (double)my;

    if (mx != 0 || my != 0) {
        XTestFakeRelativeMotionEvent(g_dpy, mx, my, CurrentTime);
        XFlush(g_dpy);
    }
}

static void evaluate_three_finger_gesture(double res_x, double res_y, double accum_x, double accum_y,
                                          double *step_anchor_x, int *gesture_fired,
                                          int *three_tv_scrub, long tnow) {
    double dx_mm = accum_x / res_x;
    double dy_mm = accum_y / res_y;
    double adx = fabs(dx_mm);
    double ady = fabs(dy_mm);

    if (g_cfg.three_h_mode != THREE_H_OFF &&
        adx >= g_cfg.three_h_thresh_mm && adx > 1.30 * ady) {
        int dir_right = (dx_mm > 0.0);
        int forward = g_cfg.three_h_invert ? !dir_right : dir_right;
        if (get_taskview_pid() > 0) {
            send_key_combo(0, forward ? g_kc_right : g_kc_left);
            *three_tv_scrub = 1;
            *step_anchor_x = accum_x;
        } else if (g_cfg.three_h_mode == THREE_H_SCRUB) {
            int active_in_list = 0;
            int wcount = count_cyclable_windows(&active_in_list);
            if (wcount == 0) {
                *gesture_fired = 1;
                return;
            }
            g_alt_tab_count = wcount;
            g_alt_tab_idx = 0;
            XTestFakeKeyEvent(g_dpy, g_kc_alt, True, CurrentTime);
            XFlush(g_dpy);
            g_alt_held = 1;
            g_alt_held_since = tnow;
            if (forward) {
                if (active_in_list && (g_alt_tab_count < 0 || g_alt_tab_count > 1))
                    g_alt_tab_idx = 1;
                send_tab_with_alt_held();
            } else {
                /* Left swipe from index 0: open switcher clamped at first window (no wrap to last) */
                send_tab_with_alt_held();
                if (active_in_list && g_alt_tab_count != 1)
                    send_shift_tab_with_alt_held();
            }
            *step_anchor_x = accum_x;
        } else {
            send_alt_tab_once(dir_right);
            *gesture_fired = 1;
        }
    } else if (-dy_mm >= g_cfg.three_v_thresh_mm && ady > 1.30 * adx) {
        *gesture_fired = 1;
        handle_vertical_gesture(1);
    } else if (dy_mm >= g_cfg.three_v_thresh_mm && ady > 1.30 * adx) {
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
        show_desktop_osd(0, 2, 4);
        for (int f = 0; f < 18; f++) {
            usleep(16000);
            render_osd_frame(now_ms());
        }
        hide_osd();
        cleanup_all();
        return 0;
    }

    if (check_only) {
        TouchpadInfo tp;
        int perm = 0;
        int fd = find_and_open_touchpad(&tp, &perm);
        if (fd >= 0) {
            printf("TOUCHPAD_OK path=%s name=\"%s\" max_fingers=%d res_x=%.1f res_y=%.1f\n",
                   tp.path, tp.name, tp.max_fingers, tp.res_x, tp.res_y);
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

        TouchpadInfo tp;
        int perm_denied = 0;
        int fd = find_and_open_touchpad(&tp, &perm_denied);
        if (fd < 0) {
            if (perm_denied)
                fprintf(stderr, "Waiting for touchpad permission on /dev/input/event*...\n");
            sleep(2);
            continue;
        }

        double res_x = tp.res_x;
        double res_y = tp.res_y;
        fprintf(stderr, "Listening on %s (%s), %d-finger capable (res=%.1f,%.1f)\n",
                tp.path, tp.name, tp.max_fingers, res_x, res_y);

        sync_touchpad_middle_click(tp.name, g_cfg.three_tap_mode != THREE_TAP_OFF);

        Slot slots[MAX_SLOTS];
        for (int s = 0; s < MAX_SLOTS; s++) {
            slots[s].tracking_id = -1;
            slots[s].x = slots[s].y = slots[s].prev_x = slots[s].prev_y = 0;
            slots[s].was_active = 0;
        }

        int cur_slot = 0;
        unsigned int tool_mask = 0;
        int btn_left_down = 0;
        int mode_fingers = 0;
        int peak_fingers = 0;
        long touch_start_ms = 0;
        long drop_pending_since = 0;
        double accum_x = 0.0, accum_y = 0.0;
        double touch_travel_mm = 0.0;
        double step_anchor_x = 0.0;
        long gesture_start_ms = 0;
        int gesture_fired = 0;
        int any_multi_gesture = 0;
        int three_tv_scrub = 0;
        int four_ws_scrub = 0;
        int four_f_locked = 0;
        int two_f_scroll_locked = 0;
        int two_f_triggered = 0;

        /* Double-tap / click-drag edge auto-slide state */
        double one_f_travel_mm = 0.0;
        int one_f_inner_seen = 0;
        int edge_sliding = 0;
        double edge_vx = 0.0, edge_vy = 0.0;
        double edge_rem_x = 0.0, edge_rem_y = 0.0;
        long edge_last_tick_ms = 0;

        while (g_running) {
            if (g_reload_cfg) {
                g_reload_cfg = 0;
                load_config();
                sync_touchpad_middle_click(tp.name, g_cfg.three_tap_mode != THREE_TAP_OFF);
            }

            long tnow_loop = now_ms();
            if (g_osd_visible) {
                if (tnow_loop >= g_osd_hide_at)
                    hide_osd();
                else if (tnow_loop - g_osd_last_render_ms >= OSD_FRAME_MS)
                    render_osd_frame(tnow_loop);
            }
            if (edge_sliding) {
                tick_edge_drag(tnow_loop, btn_left_down, &edge_sliding,
                               edge_vx, edge_vy, &edge_rem_x, &edge_rem_y,
                               &edge_last_tick_ms);
            }

            int timeout_ms = 1000;
            if (g_osd_visible || edge_sliding) timeout_ms = 16; /* 60fps smooth OSD / edge drag glide */
            else if (g_alt_held && drop_pending_since > 0) timeout_ms = 20;
            else if (mode_fingers == 3 && !gesture_fired && !g_alt_held && !three_tv_scrub) timeout_ms = 12;

            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) {
                long tnow = now_ms();
                if (g_osd_visible) {
                    if (tnow >= g_osd_hide_at) hide_osd();
                    else if (tnow - g_osd_last_render_ms >= OSD_FRAME_MS) render_osd_frame(tnow);
                }
                if (edge_sliding) {
                    tick_edge_drag(tnow, btn_left_down, &edge_sliding,
                                   edge_vx, edge_vy, &edge_rem_x, &edge_rem_y,
                                   &edge_last_tick_ms);
                }
                if (mode_fingers == 3 && !gesture_fired && !g_alt_held && !three_tv_scrub &&
                    !four_f_locked && drop_pending_since == 0 &&
                    (tnow - gesture_start_ms) >= THREE_F_SETTLE_MS) {
                    evaluate_three_finger_gesture(res_x, res_y, accum_x, accum_y,
                                                  &step_anchor_x, &gesture_fired,
                                                  &three_tv_scrub, tnow);
                    if (gesture_fired || g_alt_held || three_tv_scrub)
                        any_multi_gesture = 1;
                }
                if (drop_pending_since > 0 && (tnow - drop_pending_since) >= DROP_HYSTERESIS_MS) {
                    release_alt_if_held();
                    drop_pending_since = 0;
                    if (mode_fingers > 2) mode_fingers = 2;
                    if (four_f_locked) gesture_fired = 1;
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
                    if (ev->code == BTN_LEFT) {
                        btn_left_down = (ev->value != 0);
                    } else if (ev->code == BTN_TOOL_FINGER) {
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
                } else if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
                    release_alt_if_held();
                    for (int s = 0; s < MAX_SLOTS; s++) {
                        slots[s].tracking_id = -1;
                        slots[s].was_active = 0;
                    }
                    tool_mask = 0;
                    btn_left_down = 0;
                    mode_fingers = 0;
                    peak_fingers = 0;
                    touch_start_ms = 0;
                    drop_pending_since = 0;
                    accum_x = accum_y = 0.0;
                    touch_travel_mm = 0.0;
                    step_anchor_x = 0.0;
                    gesture_fired = 0;
                    any_multi_gesture = 0;
                    three_tv_scrub = 0;
                    four_ws_scrub = 0;
                    four_f_locked = 0;
                    two_f_scroll_locked = 0;
                    two_f_triggered = 0;
                    gesture_start_ms = 0;
                    one_f_travel_mm = 0.0;
                    one_f_inner_seen = 0;
                    edge_sliding = 0;
                    edge_vx = edge_vy = 0.0;
                    edge_rem_x = edge_rem_y = 0.0;
                    edge_last_tick_ms = 0;
                } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                    int n_active = 0;
                    int one_slot = -1;
                    double sum_dx = 0.0, sum_dy = 0.0;
                    int shared = 0;

                    for (int s = 0; s < MAX_SLOTS; s++) {
                        if (slots[s].tracking_id >= 0) {
                            one_slot = s;
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
                        double net_disp_mm = hypot(accum_x / res_x, accum_y / res_y);
                        long tap_dt = touch_start_ms > 0 ? (tnow - touch_start_ms) : 0L;
                        int tap_blocked = (any_multi_gesture || g_alt_held || three_tv_scrub || four_f_locked);

                        if (g_cfg.three_tap_mode != THREE_TAP_OFF &&
                            is_clean_tap(3, peak_fingers, tap_blocked, tap_dt, g_cfg.three_tap_max_ms,
                                         touch_travel_mm, net_disp_mm, g_cfg.three_tap_max_mm)) {
                            if (g_debug)
                                fprintf(stderr, "DEBUG: 3-finger Tap (dt=%ldms travel=%.1fmm) -> Super\n",
                                        tap_dt, touch_travel_mm);
                            if (g_cfg.three_tap_cmd[0])
                                spawn_cmd(g_cfg.three_tap_cmd);
                            else if (g_kc_super)
                                send_key_combo(0, g_kc_super);
                        } else if ((g_cfg.four_tap_mode != FOUR_TAP_OFF || g_cfg.four_tap_cmd[0]) &&
                                   is_clean_tap(4, peak_fingers, tap_blocked, tap_dt, g_cfg.four_tap_max_ms,
                                                touch_travel_mm, net_disp_mm, g_cfg.four_tap_max_mm)) {
                            if (g_debug)
                                fprintf(stderr, "DEBUG: 4-finger Tap (dt=%ldms travel=%.1fmm)\n",
                                        tap_dt, touch_travel_mm);
                            trigger_four_finger_tap();
                        }

                        release_alt_if_held();
                        mode_fingers = 0;
                        peak_fingers = 0;
                        touch_start_ms = 0;
                        drop_pending_since = 0;
                        accum_x = accum_y = 0.0;
                        touch_travel_mm = 0.0;
                        step_anchor_x = 0.0;
                        gesture_fired = 0;
                        any_multi_gesture = 0;
                        three_tv_scrub = 0;
                        four_ws_scrub = 0;
                        four_f_locked = 0;
                        two_f_scroll_locked = 0;
                        two_f_triggered = 0;
                        gesture_start_ms = 0;
                        one_f_travel_mm = 0.0;
                        one_f_inner_seen = 0;
                        edge_sliding = 0;
                        edge_vx = edge_vy = 0.0;
                        edge_rem_x = edge_rem_y = 0.0;
                        edge_last_tick_ms = 0;
                        continue;
                    }

                    if (touch_start_ms == 0)
                        touch_start_ms = tnow;

                    if (fingers > peak_fingers)
                        peak_fingers = fingers;

                    if (fingers > 1 && edge_sliding) {
                        edge_sliding = 0;
                        edge_vx = edge_vy = 0.0;
                        edge_rem_x = edge_rem_y = 0.0;
                    }

                    /* Preserve fast initial flick momentum during rapid finger landing (<55ms) */
                    if (fingers > mode_fingers) {
                        if (g_alt_held)
                            release_alt_if_held();
                        int fast_landing = (mode_fingers > 0 && (tnow - touch_start_ms) <= LANDING_MERGE_MS && fingers >= 3);
                        mode_fingers = fingers;
                        drop_pending_since = 0;
                        if (!fast_landing) {
                            accum_x = accum_y = 0.0;
                            step_anchor_x = 0.0;
                        }
                        gesture_fired = four_f_locked ? 1 : 0;
                        three_tv_scrub = 0;
                        four_ws_scrub = 0;
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
                                gesture_fired = four_f_locked ? 1 : 0;
                                three_tv_scrub = 0;
                                four_ws_scrub = 0;
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
                        double step_dist_mm = hypot(fdx / res_x, fdy / res_y);
                        touch_travel_mm += step_dist_mm;
                        if (mode_fingers == 1 && peak_fingers == 1)
                            one_f_travel_mm += step_dist_mm;
                    }

                    double dx_mm = accum_x / res_x;
                    double dy_mm = accum_y / res_y;
                    double adx = fabs(dx_mm);
                    double ady = fabs(dy_mm);

                    /* 1-finger double-tap/click-drag edge auto-slide */
                    if (mode_fingers == 1 && peak_fingers == 1 && g_cfg.edge_drag_slide && one_slot >= 0) {
                        double span_x = (double)(tp.max_x - tp.min_x);
                        double span_y = (double)(tp.max_y - tp.min_y);
                        double mx_cnt = g_cfg.edge_drag_margin_mm * res_x;
                        double my_cnt = g_cfg.edge_drag_margin_mm * res_y;
                        if (mx_cnt > span_x * 0.22) mx_cnt = span_x * 0.22;
                        if (my_cnt > span_y * 0.22) my_cnt = span_y * 0.22;

                        if (mx_cnt >= 1.0 && my_cnt >= 1.0) {
                            double fx = (double)slots[one_slot].x;
                            double fy = (double)slots[one_slot].y;
                            double ex = 0.0, ey = 0.0;

                            if (fx <= (double)tp.min_x + mx_cnt)
                                ex = -(((double)tp.min_x + mx_cnt - fx) / mx_cnt);
                            else if (fx >= (double)tp.max_x - mx_cnt)
                                ex = (fx - ((double)tp.max_x - mx_cnt)) / mx_cnt;

                            if (fy <= (double)tp.min_y + my_cnt)
                                ey = -(((double)tp.min_y + my_cnt - fy) / my_cnt);
                            else if (fy >= (double)tp.max_y - my_cnt)
                                ey = (fy - ((double)tp.max_y - my_cnt)) / my_cnt;

                            if (ex < -1.0) ex = -1.0; else if (ex > 1.0) ex = 1.0;
                            if (ey < -1.0) ey = -1.0; else if (ey > 1.0) ey = 1.0;

                            if (ex == 0.0 && ey == 0.0) {
                                one_f_inner_seen = 1;
                                edge_sliding = 0;
                                edge_vx = edge_vy = 0.0;
                                edge_rem_x = edge_rem_y = 0.0;
                            } else if ((btn_left_down || one_f_inner_seen || one_f_travel_mm >= 2.0) &&
                                       (btn_left_down || is_pointer_btn1_pressed())) {
                                edge_vx = (ex != 0.0)
                                              ? (ex > 0.0 ? 1.0 : -1.0) * g_cfg.edge_drag_speed * (0.40 + 0.60 * fabs(ex))
                                              : 0.0;
                                edge_vy = (ey != 0.0)
                                              ? (ey > 0.0 ? 1.0 : -1.0) * g_cfg.edge_drag_speed * (0.40 + 0.60 * fabs(ey))
                                              : 0.0;
                                if (!edge_sliding) {
                                    edge_sliding = 1;
                                    edge_last_tick_ms = tnow;
                                    edge_rem_x = edge_rem_y = 0.0;
                                    if (g_debug)
                                        fprintf(stderr, "DEBUG: Edge drag slide active vx=%.0f vy=%.0f\n",
                                                edge_vx, edge_vy);
                                }
                            } else {
                                edge_sliding = 0;
                                edge_vx = edge_vy = 0.0;
                            }
                        }
                    }

                    /* 2-finger horizontal flick -> Back / Forward */
                    else if (mode_fingers == 2 && peak_fingers == 2 && g_cfg.two_f_mode != TWO_F_OFF &&
                        !two_f_scroll_locked && !two_f_triggered) {
                        if (ady >= 5.5) {
                            two_f_scroll_locked = 1;
                        } else if (adx >= g_cfg.two_f_min_mm) {
                            long dt = tnow - gesture_start_ms;
                            if (adx > 2.5 * ady && dt <= g_cfg.two_f_max_ms) {
                                two_f_triggered = 1;
                                any_multi_gesture = 1;
                                int is_right = (dx_mm > 0.0);
                                int is_back = (g_cfg.two_f_mode == TWO_F_NATURAL) ? is_right : !is_right;
                                trigger_back_forward(is_back);
                            } else {
                                two_f_scroll_locked = 1;
                            }
                        }
                    }

                    /* 3-finger gestures -> boundary-clamped Alt+Tab / Task View scrub + 3-state Up/Down */
                    else if (mode_fingers == 3) {
                        if (g_alt_held) {
                            double step_dx_mm = (accum_x - step_anchor_x) / res_x;
                            if (step_dx_mm >= g_cfg.three_h_step_mm) {
                                step_alt_tab(g_cfg.three_h_invert ? -1 : 1);
                                step_anchor_x = accum_x;
                            } else if (step_dx_mm <= -g_cfg.three_h_step_mm) {
                                step_alt_tab(g_cfg.three_h_invert ? 1 : -1);
                                step_anchor_x = accum_x;
                            }
                        } else if (three_tv_scrub) {
                            double step_dx_mm = (accum_x - step_anchor_x) / res_x;
                            if (step_dx_mm >= g_cfg.three_h_step_mm) {
                                send_key_combo(0, g_cfg.three_h_invert ? g_kc_left : g_kc_right);
                                step_anchor_x = accum_x;
                            } else if (step_dx_mm <= -g_cfg.three_h_step_mm) {
                                send_key_combo(0, g_cfg.three_h_invert ? g_kc_right : g_kc_left);
                                step_anchor_x = accum_x;
                            }
                        } else if (!gesture_fired && !four_f_locked &&
                                   (tnow - gesture_start_ms) >= THREE_F_SETTLE_MS) {
                            evaluate_three_finger_gesture(res_x, res_y, accum_x, accum_y,
                                                          &step_anchor_x, &gesture_fired,
                                                          &three_tv_scrub, tnow);
                            if (gesture_fired || g_alt_held || three_tv_scrub)
                                any_multi_gesture = 1;
                        }
                    }

                    /* 4-finger gestures -> 1 desktop per swipe (Windows default) + 3-state Up/Down */
                    else if (mode_fingers == 4) {
                        if (four_ws_scrub) {
                            double step_dx_mm = (accum_x - step_anchor_x) / res_x;
                            if (step_dx_mm >= g_cfg.four_h_step_mm) {
                                int delta = (g_cfg.four_h_mode == FOUR_H_WORKSPACE_NATURAL) ? -1 : 1;
                                switch_workspace(delta);
                                step_anchor_x = accum_x;
                            } else if (step_dx_mm <= -g_cfg.four_h_step_mm) {
                                int delta = (g_cfg.four_h_mode == FOUR_H_WORKSPACE_NATURAL) ? 1 : -1;
                                switch_workspace(delta);
                                step_anchor_x = accum_x;
                            }
                        } else if (!gesture_fired) {
                            if (g_cfg.four_h_mode != FOUR_H_OFF &&
                                adx >= g_cfg.four_h_thresh_mm && adx > 1.30 * ady) {
                                int dir_right = (dx_mm > 0.0);
                                four_f_locked = 1;
                                any_multi_gesture = 1;
                                if (g_cfg.four_h_mode == FOUR_H_ALTTAB) {
                                    gesture_fired = 1;
                                    send_alt_tab_once(dir_right);
                                } else {
                                    int delta = (g_cfg.four_h_mode == FOUR_H_WORKSPACE_NATURAL)
                                                    ? (dir_right ? -1 : 1)
                                                    : (dir_right ? 1 : -1);
                                    if (switch_workspace(delta)) {
                                        if (g_cfg.four_h_scrub) {
                                            four_ws_scrub = 1;
                                            step_anchor_x = accum_x;
                                        } else {
                                            gesture_fired = 1;
                                        }
                                    } else {
                                        gesture_fired = 1;
                                        send_alt_tab_once(dir_right);
                                    }
                                }
                            } else if (-dy_mm >= g_cfg.four_v_thresh_mm && ady > 1.30 * adx) {
                                gesture_fired = 1;
                                four_f_locked = 1;
                                any_multi_gesture = 1;
                                handle_vertical_gesture(1);
                            } else if (dy_mm >= g_cfg.four_v_thresh_mm && ady > 1.30 * adx) {
                                gesture_fired = 1;
                                four_f_locked = 1;
                                any_multi_gesture = 1;
                                handle_vertical_gesture(0);
                            }
                        }
                    }
                }
            }
        }

        release_alt_if_held();
        sync_touchpad_middle_click(tp.name, 0);
        close(fd);
        if (g_running) sleep(1);
    }

    cleanup_all();
    if (lockfd >= 0) close(lockfd);
    return 0;
}
