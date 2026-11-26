#define WNCK_I_KNOW_THIS_IS_UNSTABLE
#include <gtk/gtk.h>
#include <libwnck/libwnck.h>
#include <gdk/gdkkeysyms.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    WnckWindow *win;
    GtkWidget  *child;
} CardData;

static char g_pid_path[128];
static GtkWidget *g_flow = NULL;

static void remove_pid_file(void) {
    if (g_pid_path[0])
        unlink(g_pid_path);
}

static void on_sigterm(int sig) {
    (void)sig;
    remove_pid_file();
    _exit(0);
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
    return 0;
}

static gboolean on_draw_bg(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)w;
    (void)data;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.07, 0.08, 0.11, 0.86);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
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

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
    CardData *cd = (CardData *)user_data;
    (void)btn;
    if (cd->win && WNCK_IS_WINDOW(cd->win))
        wnck_window_close(cd->win, gtk_get_current_event_time());
    if (cd->child && g_flow) {
        gtk_container_remove(GTK_CONTAINER(g_flow), cd->child);
        GList *kids = gtk_container_get_children(GTK_CONTAINER(g_flow));
        if (!kids)
            gtk_main_quit();
        else
            g_list_free(kids);
    }
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer user_data) {
    (void)w;
    (void)user_data;
    if (ev->keyval == GDK_KEY_Escape || ev->keyval == GDK_KEY_Super_L || ev->keyval == GDK_KEY_Super_R) {
        gtk_main_quit();
        return TRUE;
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
        ".tv-title { color: #e8ecf4; font-size: 16px; font-weight: bold; }\n"
        ".tv-hint  { color: #9aa4b8; font-size: 12px; }\n"
        ".tv-card  { background: rgba(34, 38, 48, 0.94); border-radius: 12px;\n"
        "            border: 1px solid rgba(255,255,255,0.12); padding: 10px; }\n"
        ".tv-card-active { border: 2px solid #7aa2f7; background: rgba(45, 52, 70, 0.96); }\n"
        "flowboxchild:selected .tv-card, flowboxchild:focus .tv-card {\n"
        "            border: 2px solid #bb9af7; background: rgba(52, 58, 78, 0.98); }\n"
        ".tv-card:hover { border-color: #7dcfff; background: rgba(44, 50, 66, 0.98); }\n"
        ".tv-wtitle { color: #f0f4ff; font-size: 13px; font-weight: 600; }\n"
        ".tv-wsub   { color: #9aa4b8; font-size: 11px; }\n"
        ".tv-close  { color: #c0caf5; background: transparent; border: none;\n"
        "             border-radius: 6px; padding: 2px 6px; font-weight: bold; }\n"
        ".tv-close:hover { background: rgba(247, 118, 142, 0.35); color: #ffffff; }\n";
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

    WnckScreen *screen = wnck_screen_get_default();
    wnck_screen_force_update(screen);
    WnckWindow *active_win = wnck_screen_get_active_window(screen);

    GList *raw = wnck_screen_get_windows(screen);
    GList *wins = NULL;
    for (GList *l = raw; l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        if (wnck_window_get_window_type(w) != WNCK_WINDOW_NORMAL)
            continue;
        if (wnck_window_is_skip_tasklist(w))
            continue;
        wins = g_list_prepend(wins, w);
    }
    wins = g_list_sort(wins, compare_windows);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Task View");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_widget_set_app_paintable(window, TRUE);

    GdkScreen *gscr = gtk_widget_get_screen(window);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gscr);
    if (vis)
        gtk_widget_set_visual(window, vis);

    GdkDisplay *gdpy = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(gdpy);
    GdkDevice *ptr = gdk_seat_get_pointer(seat);
    int px = 0, py = 0;
    gdk_device_get_position(ptr, NULL, &px, &py);
    GdkMonitor *mon = gdk_display_get_monitor_at_point(gdpy, px, py);
    GdkRectangle geom;
    gdk_monitor_get_geometry(mon, &geom);
    gtk_window_move(GTK_WINDOW(window), geom.x, geom.y);
    gtk_window_set_default_size(GTK_WINDOW(window), geom.width, geom.height);
    gtk_window_fullscreen(GTK_WINDOW(window));

    g_signal_connect(window, "draw", G_CALLBACK(on_draw_bg), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *evbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(evbox), FALSE);
    g_signal_connect(evbox, "button-press-event", G_CALLBACK(on_bg_click), NULL);
    gtk_container_add(GTK_CONTAINER(window), evbox);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 36);
    gtk_container_add(GTK_CONTAINER(evbox), vbox);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = gtk_label_new("Open Windows (Task View)");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "tv-title");
    GtkWidget *hint = gtk_label_new("Click or Enter to switch  •  Middle-click / × to close  •  Esc / Swipe Down to exit");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "tv-hint");
    gtk_box_pack_start(GTK_BOX(hdr), title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hdr, FALSE, FALSE, 0);

    if (!wins) {
        GtkWidget *empty = gtk_label_new("No open windows");
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "tv-title");
        gtk_box_pack_start(GTK_BOX(vbox), empty, TRUE, TRUE, 0);
    } else {
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

        g_flow = gtk_flow_box_new();
        gtk_widget_set_valign(g_flow, GTK_ALIGN_START);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(g_flow), 4);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(g_flow), 1);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(g_flow), 16);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(g_flow), 16);
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(g_flow), GTK_SELECTION_SINGLE);
        gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(g_flow), TRUE);
        g_signal_connect(g_flow, "child-activated", G_CALLBACK(on_child_activated), NULL);
        gtk_container_add(GTK_CONTAINER(scroll), g_flow);

        GtkFlowBoxChild *first_focus = NULL;

        for (GList *l = wins; l; l = l->next) {
            WnckWindow *w = WNCK_WINDOW(l->data);
            GtkWidget *card_ev = gtk_event_box_new();
            gtk_event_box_set_visible_window(GTK_EVENT_BOX(card_ev), FALSE);
            g_signal_connect(card_ev, "button-press-event", G_CALLBACK(on_card_button_press), w);

            GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            gtk_widget_set_size_request(card, 270, 120);
            gtk_style_context_add_class(gtk_widget_get_style_context(card), "tv-card");
            if (w == active_win)
                gtk_style_context_add_class(gtk_widget_get_style_context(card), "tv-card-active");
            gtk_container_add(GTK_CONTAINER(card_ev), card);

            GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GdkPixbuf *pb = wnck_window_get_icon(w);
            GtkWidget *icon;
            if (pb) {
                GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, 40, 40, GDK_INTERP_BILINEAR);
                icon = gtk_image_new_from_pixbuf(scaled ? scaled : pb);
                if (scaled) g_object_unref(scaled);
            } else {
                icon = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
            }
            gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);

            GtkWidget *txtbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            const char *wname = wnck_window_get_name(w);
            GtkWidget *lbl = gtk_label_new(wname ? wname : "(Untitled)");
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(lbl), 26);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "tv-wtitle");
            gtk_box_pack_start(GTK_BOX(txtbox), lbl, FALSE, FALSE, 0);

            WnckWorkspace *ws = wnck_window_get_workspace(w);
            char sub[64];
            if (ws)
                snprintf(sub, sizeof(sub), "Desktop %d%s", wnck_workspace_get_number(ws) + 1,
                         wnck_window_is_minimized(w) ? "  •  Minimized" : "");
            else
                snprintf(sub, sizeof(sub), "All Desktops%s",
                         wnck_window_is_minimized(w) ? "  •  Minimized" : "");

            GtkWidget *sublbl = gtk_label_new(sub);
            gtk_label_set_xalign(GTK_LABEL(sublbl), 0.0f);
            gtk_style_context_add_class(gtk_widget_get_style_context(sublbl), "tv-wsub");
            gtk_box_pack_start(GTK_BOX(txtbox), sublbl, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(top), txtbox, TRUE, TRUE, 0);

            GtkWidget *close_btn = gtk_button_new_with_label("×");
            gtk_widget_set_valign(close_btn, GTK_ALIGN_START);
            gtk_style_context_add_class(gtk_widget_get_style_context(close_btn), "tv-close");
            gtk_box_pack_end(GTK_BOX(top), close_btn, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(card), top, TRUE, TRUE, 0);

            WnckApplication *app = wnck_window_get_application(w);
            const char *aname = app ? wnck_application_get_name(app) : "";
            if (aname && *aname) {
                GtkWidget *applbl = gtk_label_new(aname);
                gtk_label_set_ellipsize(GTK_LABEL(applbl), PANGO_ELLIPSIZE_END);
                gtk_label_set_xalign(GTK_LABEL(applbl), 0.0f);
                gtk_style_context_add_class(gtk_widget_get_style_context(applbl), "tv-wsub");
                gtk_box_pack_end(GTK_BOX(card), applbl, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(g_flow), card_ev);
            GtkWidget *fb_child = gtk_widget_get_parent(card_ev);
            g_object_set_data(G_OBJECT(fb_child), "wnck-win", w);

            CardData *cd = g_new0(CardData, 1);
            cd->win = w;
            cd->child = fb_child;
            g_signal_connect_data(close_btn, "clicked", G_CALLBACK(on_close_clicked),
                                  cd, free_card_data, 0);

            if (!first_focus || w == active_win)
                first_focus = GTK_FLOW_BOX_CHILD(fb_child);
        }
        g_list_free(wins);

        gtk_widget_show_all(window);
        if (first_focus) {
            gtk_flow_box_select_child(GTK_FLOW_BOX(g_flow), first_focus);
            gtk_widget_grab_focus(GTK_WIDGET(first_focus));
        }
        gtk_main();
        return 0;
    }

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
