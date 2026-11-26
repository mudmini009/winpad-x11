#define WNCK_I_KNOW_THIS_IS_UNSTABLE
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <libwnck/libwnck.h>
#include <cairo/cairo.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_XCOMPOSITE
#include <cairo/cairo-xlib.h>
#include <X11/extensions/Xcomposite.h>
#endif

#define THUMB_W 300
#define THUMB_H 170

typedef struct {
    WnckWindow *win;
    GtkWidget  *child;
    int         ws_num; /* -1 = pinned/all */
} CardData;

typedef struct {
    cairo_surface_t *thumb;
    GdkPixbuf       *fallback_icon;
    char             badge[48];
} PreviewData;

static char g_pid_path[128];
static GtkWidget *g_window = NULL;
static GtkWidget *g_flow = NULL;
static GtkWidget *g_filter_lbl = NULL;
static GtkWidget *g_sub_lbl = NULL;
static GtkWidget *g_empty_lbl = NULL;
static GtkWidget *g_scroll = NULL;
static GtkWidget *g_all_btn = NULL;
static GtkWidget **g_ws_btns = NULL;
static int g_n_ws = 1;
static int g_view_ws = 0;
static int g_show_all = 0;
static double g_fade_alpha = 0.0;
static char g_filter[128] = {0};
static WnckScreen *g_screen = NULL;
static volatile sig_atomic_t g_refresh_ws = 0;

static void remove_pid_file(void) {
    if (g_pid_path[0])
        unlink(g_pid_path);
}

static void on_sigterm(int sig) {
    (void)sig;
    remove_pid_file();
    _exit(0);
}

static void on_sigusr1(int sig) {
    (void)sig;
    g_refresh_ws = 1;
}

/* Toggle off if another instance is already open. */
static int check_or_claim_instance(void) {
    snprintf(g_pid_path, sizeof(g_pid_path), "/tmp/winpad-taskview-%d.pid", (int)getuid());

    FILE *f = fopen(g_pid_path, "r");
    if (f) {
        long old_pid = 0;
        if (fscanf(f, "%ld", &old_pid) == 1 && old_pid > 1 && old_pid != (long)getpid()) {
            char comm_path[128], comm[64] = {0};
            snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", old_pid);
            FILE *cf = fopen(comm_path, "r");
            if (cf) {
                if (fgets(comm, sizeof(comm), cf) && strncmp(comm, "winpad-taskview", 15) == 0) {
                    fclose(cf);
                    fclose(f);
                    kill((pid_t)old_pid, SIGTERM);
                    return 1;
                }
                fclose(cf);
            }
        }
        fclose(f);
    }

    f = fopen(g_pid_path, "w");
    if (f) {
        fprintf(f, "%ld\n", (long)getpid());
        fclose(f);
    }
    atexit(remove_pid_file);
    signal(SIGTERM, on_sigterm);
    signal(SIGINT, on_sigterm);
    signal(SIGUSR1, on_sigusr1);
    return 0;
}

static void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    const double deg = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -90 * deg, 0 * deg);
    cairo_arc(cr, x + w - r, y + h - r, r, 0 * deg, 90 * deg);
    cairo_arc(cr, x + r, y + h - r, r, 90 * deg, 180 * deg);
    cairo_arc(cr, x + r, y + r, r, 180 * deg, 270 * deg);
    cairo_close_path(cr);
}

static gboolean on_draw_bg(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(w, &alloc);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, alloc.width, alloc.height);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.050, 0.060, 0.090, 0.88);
    cairo_pattern_add_color_stop_rgba(pat, 0.5, 0.068, 0.080, 0.118, 0.87);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.045, 0.055, 0.085, 0.90);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    return FALSE;
}

#ifdef HAVE_XCOMPOSITE
static Window find_toplevel_xid(Display *dpy, Window w) {
    Window root = DefaultRootWindow(dpy), parent = None, *children = NULL;
    unsigned int n = 0;
    Window cur = w;
    while (cur != None && cur != root) {
        if (!XQueryTree(dpy, cur, &root, &parent, &children, &n))
            break;
        if (children)
            XFree(children);
        if (parent == root)
            return cur;
        cur = parent;
    }
    return w;
}

static cairo_surface_t *capture_window_thumb(WnckWindow *win) {
    if (wnck_window_is_minimized(win))
        return NULL;

    GdkDisplay *gd = gdk_display_get_default();
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    if (!dpy) return NULL;

    static int xcomp_checked = 0, xcomp_ok = 0;
    if (!xcomp_checked) {
        int ev = 0, err = 0;
        xcomp_ok = XCompositeQueryExtension(dpy, &ev, &err);
        xcomp_checked = 1;
    }
    if (!xcomp_ok) return NULL;

    Window top = find_toplevel_xid(dpy, (Window)wnck_window_get_xid(win));
    if (top == None) return NULL;

    gdk_x11_display_error_trap_push(gd);
    XWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));
    Status ok = XGetWindowAttributes(dpy, top, &attr);
    if (!ok || attr.map_state != IsViewable || attr.width < 16 || attr.height < 16) {
        gdk_x11_display_error_trap_pop_ignored(gd);
        return NULL;
    }

    Pixmap pm = XCompositeNameWindowPixmap(dpy, top);
    if (pm == None) {
        gdk_x11_display_error_trap_pop_ignored(gd);
        return NULL;
    }

    cairo_surface_t *src = cairo_xlib_surface_create(dpy, pm, attr.visual, attr.width, attr.height);
    if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(src);
        XFreePixmap(dpy, pm);
        gdk_x11_display_error_trap_pop_ignored(gd);
        return NULL;
    }

    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, THUMB_W, THUMB_H);
    cairo_t *cr = cairo_create(dst);

    cairo_set_source_rgb(cr, 0.08, 0.09, 0.13);
    cairo_paint(cr);

    double sx = (double)THUMB_W / (double)attr.width;
    double sy = (double)THUMB_H / (double)attr.height;
    double sc = sx < sy ? sx : sy;
    double dw = attr.width * sc;
    double dh = attr.height * sc;
    double ox = (THUMB_W - dw) * 0.5;
    double oy = (THUMB_H - dh) * 0.5;

    cairo_translate(cr, ox, oy);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, src, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(src);
    XFreePixmap(dpy, pm);
    XSync(dpy, False);

    if (gdk_x11_display_error_trap_pop(gd) != 0) {
        cairo_surface_destroy(dst);
        return NULL;
    }
    return dst;
}
#else
static cairo_surface_t *capture_window_thumb(WnckWindow *win) {
    (void)win;
    return NULL;
}
#endif

static void free_preview_data(gpointer p) {
    PreviewData *pd = (PreviewData *)p;
    if (!pd) return;
    if (pd->thumb) cairo_surface_destroy(pd->thumb);
    if (pd->fallback_icon) g_object_unref(pd->fallback_icon);
    g_free(pd);
}

static gboolean on_draw_preview(GtkWidget *w, cairo_t *cr, gpointer user_data) {
    PreviewData *pd = (PreviewData *)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(w, &alloc);
    double width = alloc.width;
    double height = alloc.height;

    rounded_rect_path(cr, 0.5, 0.5, width - 1.0, height - 1.0, 9.0);
    cairo_clip(cr);

    if (pd->thumb) {
        cairo_set_source_surface(cr, pd->thumb, 0, 0);
        cairo_paint(cr);
    } else {
        cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, width, height);
        cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.11, 0.13, 0.19);
        cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.08, 0.09, 0.13);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);

        if (pd->fallback_icon) {
            int iw = gdk_pixbuf_get_width(pd->fallback_icon);
            int ih = gdk_pixbuf_get_height(pd->fallback_icon);
            gdk_cairo_set_source_pixbuf(cr, pd->fallback_icon,
                                        (width - iw) * 0.5, (height - ih) * 0.5 - 10.0);
            cairo_paint(cr);
        }

        if (pd->badge[0]) {
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 11.5);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, pd->badge, &ext);
            cairo_set_source_rgba(cr, 0.68, 0.75, 0.88, 0.90);
            cairo_move_to(cr, (width - ext.width) * 0.5, height - 18.0);
            cairo_show_text(cr, pd->badge);
        }
    }

    cairo_reset_clip(cr);
    rounded_rect_path(cr, 0.5, 0.5, width - 1.0, height - 1.0, 9.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.11);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    return FALSE;
}

static void activate_window_and_quit(WnckWindow *win) {
    if (!win || !WNCK_IS_WINDOW(win)) {
        gtk_main_quit();
        return;
    }
    guint32 ts = gtk_get_current_event_time();
    if (ts == GDK_CURRENT_TIME)
        ts = (guint32)(g_get_monotonic_time() / 1000);

    WnckWorkspace *ws = wnck_window_get_workspace(win);
    WnckScreen *scr = wnck_window_get_screen(win);
    if (ws && scr && wnck_screen_get_active_workspace(scr) != ws)
        wnck_workspace_activate(ws, ts);
    if (wnck_window_is_minimized(win))
        wnck_window_unminimize(win, ts);
    wnck_window_activate(win, ts);
    gtk_main_quit();
}

static void on_child_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data) {
    (void)box;
    (void)user_data;
    WnckWindow *win = g_object_get_data(G_OBJECT(child), "wnck-win");
    activate_window_and_quit(win);
}

static gboolean on_card_button_press(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    (void)w;
    if (ev->button == 1) {
        activate_window_and_quit((WnckWindow *)user_data);
        return TRUE;
    }
    if (ev->button == 2) {
        WnckWindow *win = (WnckWindow *)user_data;
        if (win && WNCK_IS_WINDOW(win))
            wnck_window_close(win, ev->time);
        return TRUE;
    }
    return FALSE;
}

static void free_card_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static gboolean filter_cards(GtkFlowBoxChild *child, gpointer user_data) {
    (void)user_data;
    int ws_num = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "ws-num"));
    if (!g_show_all && ws_num != -1 && ws_num != g_view_ws)
        return FALSE;

    if (!g_filter[0])
        return TRUE;
    const char *hay = g_object_get_data(G_OBJECT(child), "search-text");
    if (!hay) return TRUE;
    return strstr(hay, g_filter) != NULL;
}

static void refresh_view_state(void) {
    int visible = 0;
    if (g_flow) {
        gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(g_flow));
        GList *kids = gtk_container_get_children(GTK_CONTAINER(g_flow));
        GtkFlowBoxChild *first_vis = NULL;
        for (GList *l = kids; l; l = l->next) {
            GtkFlowBoxChild *ch = GTK_FLOW_BOX_CHILD(l->data);
            if (filter_cards(ch, NULL)) {
                visible++;
                if (!first_vis) first_vis = ch;
            }
        }
        g_list_free(kids);
        if (first_vis) {
            gtk_flow_box_select_child(GTK_FLOW_BOX(g_flow), first_vis);
            gtk_widget_grab_focus(GTK_WIDGET(first_vis));
        }
    }

    if (g_sub_lbl) {
        char buf[128];
        if (g_show_all) {
            snprintf(buf, sizeof(buf), "All Desktops  •  %d open window%s",
                     visible, visible == 1 ? "" : "s");
        } else {
            snprintf(buf, sizeof(buf), "Desktop %d of %d  •  %d open window%s",
                     g_view_ws + 1, g_n_ws, visible, visible == 1 ? "" : "s");
        }
        gtk_label_set_text(GTK_LABEL(g_sub_lbl), buf);
    }

    if (g_empty_lbl && g_scroll) {
        if (visible == 0) {
            char ebuf[128];
            if (g_filter[0])
                snprintf(ebuf, sizeof(ebuf), "No windows match \"%s\"", g_filter);
            else
                snprintf(ebuf, sizeof(ebuf), "No open windows on Desktop %d", g_view_ws + 1);
            gtk_label_set_text(GTK_LABEL(g_empty_lbl), ebuf);
            gtk_widget_hide(g_scroll);
            gtk_widget_show(g_empty_lbl);
        } else {
            gtk_widget_hide(g_empty_lbl);
            gtk_widget_show(g_scroll);
        }
    }

    if (g_filter_lbl) {
        if (g_filter[0]) {
            char fbuf[160];
            snprintf(fbuf, sizeof(fbuf), "Filter: %s", g_filter);
            gtk_label_set_text(GTK_LABEL(g_filter_lbl), fbuf);
            gtk_widget_show(g_filter_lbl);
        } else {
            gtk_widget_hide(g_filter_lbl);
        }
    }

    if (g_ws_btns) {
        for (int i = 0; i < g_n_ws; i++) {
            GtkStyleContext *ctx = gtk_widget_get_style_context(g_ws_btns[i]);
            if (!g_show_all && i == g_view_ws)
                gtk_style_context_add_class(ctx, "tv-desk-active");
            else
                gtk_style_context_remove_class(ctx, "tv-desk-active");
        }
    }

    if (g_all_btn) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(g_all_btn);
        if (g_show_all)
            gtk_style_context_add_class(ctx, "tv-pill-on");
        else
            gtk_style_context_remove_class(ctx, "tv-pill-on");
    }
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
    CardData *cd = (CardData *)user_data;
    (void)btn;
    if (cd->win && WNCK_IS_WINDOW(cd->win))
        wnck_window_close(cd->win, gtk_get_current_event_time());
    if (cd->child && g_flow) {
        gtk_container_remove(GTK_CONTAINER(g_flow), cd->child);
        refresh_view_state();
    }
}

static void on_desktop_card_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int ws_idx = GPOINTER_TO_INT(user_data);
    WnckWorkspace *ws = wnck_screen_get_workspace(g_screen, ws_idx);
    if (ws) {
        guint32 ts = gtk_get_current_event_time();
        if (ts == GDK_CURRENT_TIME)
            ts = (guint32)(g_get_monotonic_time() / 1000);
        wnck_workspace_activate(ws, ts);
    }
    gtk_main_quit();
}

static void on_all_toggle_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    g_show_all = !g_show_all;
    refresh_view_state();
}

static gboolean on_sigusr1_poll(gpointer user_data) {
    (void)user_data;
    if (g_refresh_ws && g_screen) {
        g_refresh_ws = 0;
        wnck_screen_force_update(g_screen);
        WnckWorkspace *ws = wnck_screen_get_active_workspace(g_screen);
        if (ws) {
            g_view_ws = wnck_workspace_get_number(ws);
            g_show_all = 0;
            refresh_view_state();
        }
    }
    return TRUE;
}

static gboolean on_fade_tick(gpointer user_data) {
    (void)user_data;
    if (!g_window) return FALSE;
    g_fade_alpha += 0.25;
    if (g_fade_alpha >= 1.0) {
        gtk_widget_set_opacity(g_window, 1.0);
        return FALSE;
    }
    double eased = 1.0 - pow(1.0 - g_fade_alpha, 3.0);
    gtk_widget_set_opacity(g_window, eased);
    return TRUE;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer user_data) {
    (void)w;
    (void)user_data;
    if (ev->keyval == GDK_KEY_Escape) {
        if (g_filter[0]) {
            g_filter[0] = '\0';
            refresh_view_state();
            return TRUE;
        }
        gtk_main_quit();
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Super_L || ev->keyval == GDK_KEY_Super_R) {
        gtk_main_quit();
        return TRUE;
    }
    /* Ctrl+Left/Right previews other desktops inside Task View */
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_Left || ev->keyval == GDK_KEY_Right)) {
        g_show_all = 0;
        if (ev->keyval == GDK_KEY_Left && g_view_ws > 0) g_view_ws--;
        else if (ev->keyval == GDK_KEY_Right && g_view_ws + 1 < g_n_ws) g_view_ws++;
        refresh_view_state();
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_BackSpace) {
        size_t len = strlen(g_filter);
        if (len > 0) {
            g_filter[len - 1] = '\0';
            refresh_view_state();
            return TRUE;
        }
    }
    if (ev->string && ev->string[0] >= 0x20 && ev->string[0] < 0x7f &&
        !(ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
        size_t len = strlen(g_filter);
        if (len + 1 < sizeof(g_filter)) {
            char c = (char)tolower((unsigned char)ev->string[0]);
            g_filter[len] = c;
            g_filter[len + 1] = '\0';
            refresh_view_state();
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_bg_click(GtkWidget *w, GdkEventButton *ev, gpointer user_data) {
    (void)w;
    (void)ev;
    (void)user_data;
    gtk_main_quit();
    return TRUE;
}

static gint compare_windows(gconstpointer a, gconstpointer b) {
    WnckWindow *wa = (WnckWindow *)a;
    WnckWindow *wb = (WnckWindow *)b;
    WnckWorkspace *wsa = wnck_window_get_workspace(wa);
    WnckWorkspace *wsb = wnck_window_get_workspace(wb);
    int na = wsa ? wnck_workspace_get_number(wsa) : 999;
    int nb = wsb ? wnck_workspace_get_number(wsb) : 999;
    if (na != nb) return na - nb;
    return g_strcmp0(wnck_window_get_name(wa), wnck_window_get_name(wb));
}

static void apply_css(void) {
    static const char *css =
        ".tv-title   { color: #f8fafc; font-size: 22px; font-weight: 700; }\n"
        ".tv-sub     { color: #94a3b8; font-size: 12.5px; }\n"
        ".tv-hint    { color: #94a3b8; font-size: 12px; }\n"
        ".tv-empty   { color: #cbd5e1; font-size: 17px; font-weight: 600; }\n"
        ".tv-filter  { background: rgba(122, 162, 247, 0.22); color: #dbeafe;\n"
        "              border: 1px solid rgba(122, 162, 247, 0.55); border-radius: 99px;\n"
        "              padding: 4px 12px; font-size: 12px; font-weight: 600; }\n"
        ".tv-pill    { background: rgba(255, 255, 255, 0.07); color: #cbd5e1;\n"
        "              border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 99px;\n"
        "              padding: 5px 14px; font-size: 12px; font-weight: 600; }\n"
        ".tv-pill:hover { background: rgba(125, 207, 255, 0.18); color: #ffffff; }\n"
        ".tv-pill-on { background: rgba(122, 162, 247, 0.28); color: #ffffff;\n"
        "              border-color: #7aa2f7; }\n"
        ".tv-card    { background: rgba(25, 29, 41, 0.92); border-radius: 14px;\n"
        "              border: 1px solid rgba(255, 255, 255, 0.12); padding: 10px; }\n"
        ".tv-card-active { border: 2px solid #7aa2f7; background: rgba(33, 40, 58, 0.96); }\n"
        "flowboxchild { outline-style: none; }\n"
        "flowboxchild:selected .tv-card, flowboxchild:focus .tv-card {\n"
        "              border: 2px solid #bb9af7; background: rgba(38, 45, 66, 0.98); }\n"
        ".tv-card:hover { border-color: #7dcfff; background: rgba(35, 42, 60, 0.98); }\n"
        ".tv-wtitle  { color: #f1f5f9; font-size: 13px; font-weight: 600; }\n"
        ".tv-badge   { background: rgba(255, 255, 255, 0.08); color: #a5b4fc;\n"
        "              border-radius: 6px; padding: 2px 7px; font-size: 10px; font-weight: 700; }\n"
        ".tv-close   { color: #94a3b8; background: transparent; border: none;\n"
        "              border-radius: 8px; padding: 1px 6px; font-size: 14px; font-weight: bold; }\n"
        ".tv-close:hover { background: rgba(247, 118, 142, 0.35); color: #ffffff; }\n"
        ".tv-dock    { background: rgba(14, 17, 26, 0.72); border-radius: 16px;\n"
        "              border: 1px solid rgba(255, 255, 255, 0.09); padding: 12px 16px; }\n"
        ".tv-desk-card { background: rgba(28, 33, 47, 0.88); border-radius: 12px;\n"
        "                border: 1px solid rgba(255, 255, 255, 0.10); padding: 10px 16px; }\n"
        ".tv-desk-card:hover { border-color: #7dcfff; background: rgba(36, 44, 64, 0.95); }\n"
        ".tv-desk-active { border: 2px solid #7aa2f7; background: rgba(42, 53, 82, 0.96); }\n"
        ".tv-desk-name { color: #f1f5f9; font-size: 13px; font-weight: 700; }\n"
        ".tv-desk-meta { color: #94a3b8; font-size: 11px; }\n";
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

int main(int argc, char **argv) {
    if (check_or_claim_instance())
        return 0;

    gtk_init(&argc, &argv);
    apply_css();

    g_screen = wnck_screen_get_default();
    wnck_screen_force_update(g_screen);
    WnckWindow *active_win = wnck_screen_get_active_window(g_screen);
    WnckWorkspace *active_ws = wnck_screen_get_active_workspace(g_screen);
    g_n_ws = wnck_screen_get_workspace_count(g_screen);
    if (g_n_ws < 1) g_n_ws = 1;
    g_view_ws = active_ws ? wnck_workspace_get_number(active_ws) : 0;

    GList *raw = wnck_screen_get_windows(g_screen);
    GList *wins = NULL;
    int *ws_counts = g_new0(int, g_n_ws);
    for (GList *l = raw; l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        if (wnck_window_get_window_type(w) != WNCK_WINDOW_NORMAL)
            continue;
        if (wnck_window_is_skip_tasklist(w))
            continue;
        wins = g_list_prepend(wins, w);
        WnckWorkspace *ws = wnck_window_get_workspace(w);
        if (ws) {
            int wn = wnck_workspace_get_number(ws);
            if (wn >= 0 && wn < g_n_ws) ws_counts[wn]++;
        }
    }
    wins = g_list_sort(wins, compare_windows);

    /* Capture live window thumbnails BEFORE showing our fullscreen overlay. */
    GHashTable *thumbs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free_preview_data);
    for (GList *l = wins; l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        PreviewData *pd = g_new0(PreviewData, 1);
        pd->thumb = capture_window_thumb(w);
        if (!pd->thumb) {
            GdkPixbuf *pb = wnck_window_get_icon(w);
            if (pb)
                pd->fallback_icon = gdk_pixbuf_scale_simple(pb, 56, 56, GDK_INTERP_BILINEAR);
            WnckWorkspace *ws = wnck_window_get_workspace(w);
            if (wnck_window_is_minimized(w))
                snprintf(pd->badge, sizeof(pd->badge), "Minimized");
            else if (ws && active_ws && ws != active_ws)
                snprintf(pd->badge, sizeof(pd->badge), "Desktop %d", wnck_workspace_get_number(ws) + 1);
        }
        g_hash_table_insert(thumbs, w, pd);
    }

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "Task View");
    gtk_window_set_decorated(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(g_window), TRUE);
    gtk_widget_set_app_paintable(g_window, TRUE);

    GdkScreen *gscr = gtk_widget_get_screen(g_window);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gscr);
    if (vis)
        gtk_widget_set_visual(g_window, vis);

    GdkDisplay *gdpy = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(gdpy);
    GdkDevice *ptr = gdk_seat_get_pointer(seat);
    int px = 0, py = 0;
    gdk_device_get_position(ptr, NULL, &px, &py);
    GdkMonitor *mon = gdk_display_get_monitor_at_point(gdpy, px, py);
    GdkRectangle geom;
    gdk_monitor_get_geometry(mon, &geom);
    gtk_window_move(GTK_WINDOW(g_window), geom.x, geom.y);
    gtk_window_set_default_size(GTK_WINDOW(g_window), geom.width, geom.height);
    gtk_window_fullscreen(GTK_WINDOW(g_window));
    gtk_widget_set_opacity(g_window, 0.15);

    g_signal_connect(g_window, "draw", G_CALLBACK(on_draw_bg), NULL);
    g_signal_connect(g_window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *evbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(evbox), FALSE);
    g_signal_connect(evbox, "button-press-event", G_CALLBACK(on_bg_click), NULL);
    gtk_container_add(GTK_CONTAINER(g_window), evbox);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 36);
    gtk_container_add(GTK_CONTAINER(evbox), vbox);

    /* Top bar: Task View title + filter badge + All Desktops toggle */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title = gtk_label_new("Task View");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "tv-title");

    g_sub_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_sub_lbl), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_sub_lbl), "tv-sub");
    gtk_box_pack_start(GTK_BOX(title_box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_box), g_sub_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), title_box, FALSE, FALSE, 0);

    g_filter_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_filter_lbl), "tv-filter");
    gtk_widget_set_valign(g_filter_lbl, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(hdr), g_filter_lbl, FALSE, FALSE, 8);

    if (g_n_ws > 1) {
        g_all_btn = gtk_button_new_with_label("All Desktops");
        gtk_widget_set_valign(g_all_btn, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_all_btn), "tv-pill");
        g_signal_connect(g_all_btn, "clicked", G_CALLBACK(on_all_toggle_clicked), NULL);
        gtk_box_pack_end(GTK_BOX(hdr), g_all_btn, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), hdr, FALSE, FALSE, 0);

    /* Center Windows Grid */
    g_empty_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_empty_lbl), "tv-empty");
    gtk_box_pack_start(GTK_BOX(vbox), g_empty_lbl, TRUE, TRUE, 0);

    g_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), g_scroll, TRUE, TRUE, 0);

    g_flow = gtk_flow_box_new();
    gtk_widget_set_valign(g_flow, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(g_flow, GTK_ALIGN_CENTER);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(g_flow), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(g_flow), 1);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(g_flow), 20);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(g_flow), 20);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(g_flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(g_flow), TRUE);
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(g_flow), filter_cards, NULL, NULL);
    g_signal_connect(g_flow, "child-activated", G_CALLBACK(on_child_activated), NULL);
    gtk_container_add(GTK_CONTAINER(g_scroll), g_flow);

    GtkFlowBoxChild *active_child = NULL;

    for (GList *l = wins; l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        GtkWidget *card_ev = gtk_event_box_new();
        gtk_event_box_set_visible_window(GTK_EVENT_BOX(card_ev), FALSE);
        g_signal_connect(card_ev, "button-press-event", G_CALLBACK(on_card_button_press), w);

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_size_request(card, THUMB_W + 20, THUMB_H + 54);
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "tv-card");
        if (w == active_win)
            gtk_style_context_add_class(gtk_widget_get_style_context(card), "tv-card-active");
        gtk_container_add(GTK_CONTAINER(card_ev), card);

        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GdkPixbuf *pb = wnck_window_get_icon(w);
        GtkWidget *icon;
        if (pb) {
            GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, 18, 18, GDK_INTERP_BILINEAR);
            icon = gtk_image_new_from_pixbuf(scaled ? scaled : pb);
            if (scaled) g_object_unref(scaled);
        } else {
            icon = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
        }
        gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);

        const char *wname = wnck_window_get_name(w);
        GtkWidget *lbl = gtk_label_new(wname ? wname : "(Untitled)");
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 24);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "tv-wtitle");
        gtk_box_pack_start(GTK_BOX(top), lbl, TRUE, TRUE, 0);

        WnckWorkspace *ws = wnck_window_get_workspace(w);
        int ws_num = ws ? wnck_workspace_get_number(ws) : -1;
        char dbadge[16];
        if (ws_num >= 0) snprintf(dbadge, sizeof(dbadge), "D%d", ws_num + 1);
        else snprintf(dbadge, sizeof(dbadge), "All");
        GtkWidget *db_lbl = gtk_label_new(dbadge);
        gtk_widget_set_valign(db_lbl, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(db_lbl), "tv-badge");
        gtk_box_pack_start(GTK_BOX(top), db_lbl, FALSE, FALSE, 0);

        GtkWidget *close_btn = gtk_button_new_with_label("×");
        gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(close_btn), "tv-close");
        gtk_box_pack_end(GTK_BOX(top), close_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), top, FALSE, FALSE, 0);

        PreviewData *pd = g_hash_table_lookup(thumbs, w);
        GtkWidget *preview = gtk_drawing_area_new();
        gtk_widget_set_size_request(preview, THUMB_W, THUMB_H);
        g_signal_connect(preview, "draw", G_CALLBACK(on_draw_preview), pd);
        gtk_box_pack_start(GTK_BOX(card), preview, TRUE, TRUE, 0);

        gtk_container_add(GTK_CONTAINER(g_flow), card_ev);
        GtkWidget *fb_child = gtk_widget_get_parent(card_ev);
        g_object_set_data(G_OBJECT(fb_child), "wnck-win", w);
        g_object_set_data(G_OBJECT(fb_child), "ws-num", GINT_TO_POINTER(ws_num));

        WnckApplication *app = wnck_window_get_application(w);
        const char *aname = app ? wnck_application_get_name(app) : "";
        gchar *raw_search = g_strdup_printf("%s %s", wname ? wname : "", aname ? aname : "");
        gchar *low_search = g_ascii_strdown(raw_search, -1);
        g_free(raw_search);
        g_object_set_data_full(G_OBJECT(fb_child), "search-text", low_search, g_free);

        CardData *cd = g_new0(CardData, 1);
        cd->win = w;
        cd->child = fb_child;
        g_signal_connect_data(close_btn, "clicked", G_CALLBACK(on_close_clicked),
                              cd, free_card_data, 0);

        if (w == active_win)
            active_child = GTK_FLOW_BOX_CHILD(fb_child);
    }
    g_list_free(wins);

    /* Bottom Windows 11 Virtual Desktops Dock */
    if (g_n_ws > 1) {
        GtkWidget *dock_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_halign(dock_wrap, GTK_ALIGN_CENTER);
        GtkWidget *dock = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(dock), "tv-dock");
        gtk_box_pack_start(GTK_BOX(dock_wrap), dock, FALSE, FALSE, 0);

        g_ws_btns = g_new0(GtkWidget *, g_n_ws);
        for (int i = 0; i < g_n_ws; i++) {
            GtkWidget *btn = gtk_button_new();
            gtk_widget_set_size_request(btn, 165, 62);
            gtk_style_context_add_class(gtk_widget_get_style_context(btn), "tv-desk-card");

            GtkWidget *dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            gtk_widget_set_valign(dbox, GTK_ALIGN_CENTER);
            char dname[32], dmeta[48];
            snprintf(dname, sizeof(dname), "Desktop %d", i + 1);
            if (ws_counts[i] == 0)
                snprintf(dmeta, sizeof(dmeta), "Empty");
            else
                snprintf(dmeta, sizeof(dmeta), "%d window%s", ws_counts[i], ws_counts[i] == 1 ? "" : "s");

            GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *nl = gtk_label_new(dname);
            gtk_label_set_xalign(GTK_LABEL(nl), 0.0f);
            gtk_style_context_add_class(gtk_widget_get_style_context(nl), "tv-desk-name");
            gtk_box_pack_start(GTK_BOX(top_row), nl, TRUE, TRUE, 0);

            /* Visual mini-window dots on each desktop card */
            int dots = ws_counts[i] > 4 ? 4 : ws_counts[i];
            if (dots > 0) {
                GtkWidget *dot_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
                gtk_widget_set_valign(dot_box, GTK_ALIGN_CENTER);
                for (int d = 0; d < dots; d++) {
                    GtkWidget *dot = gtk_label_new("●");
                    gtk_style_context_add_class(gtk_widget_get_style_context(dot), "tv-badge");
                    gtk_box_pack_start(GTK_BOX(dot_box), dot, FALSE, FALSE, 0);
                }
                gtk_box_pack_end(GTK_BOX(top_row), dot_box, FALSE, FALSE, 0);
            }

            GtkWidget *ml = gtk_label_new(dmeta);
            gtk_label_set_xalign(GTK_LABEL(ml), 0.0f);
            gtk_style_context_add_class(gtk_widget_get_style_context(ml), "tv-desk-meta");
            gtk_box_pack_start(GTK_BOX(dbox), top_row, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(dbox), ml, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(btn), dbox);

            g_signal_connect(btn, "clicked", G_CALLBACK(on_desktop_card_clicked), GINT_TO_POINTER(i));
            gtk_box_pack_start(GTK_BOX(dock), btn, FALSE, FALSE, 0);
            g_ws_btns[i] = btn;
        }
        gtk_box_pack_end(GTK_BOX(vbox), dock_wrap, FALSE, FALSE, 4);
    }

    GtkWidget *footer = gtk_label_new(
        "Click window or desktop to switch  •  Type to filter  •  Ctrl+←/→ preview desktops  •  Esc / Swipe Down to exit");
    gtk_style_context_add_class(gtk_widget_get_style_context(footer), "tv-hint");
    gtk_box_pack_end(GTK_BOX(vbox), footer, FALSE, FALSE, 0);

    gtk_widget_show_all(g_window);
    refresh_view_state();
    if (active_child && filter_cards(active_child, NULL)) {
        gtk_flow_box_select_child(GTK_FLOW_BOX(g_flow), active_child);
        gtk_widget_grab_focus(GTK_WIDGET(active_child));
    }

    g_timeout_add(12, on_fade_tick, NULL);
    g_timeout_add(40, on_sigusr1_poll, NULL);

    gtk_main();
    g_free(ws_counts);
    g_free(g_ws_btns);
    g_hash_table_destroy(thumbs);
    return 0;
}
