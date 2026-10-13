#include "window-internal.h"
#include "window-startup.h"
#include "window-console.h"
#include "window-lifecycle.h"
#include "window-players.h"
#include "player-heads.h"
#include "window-dialogs.h"
#include "window-player-data.h"
#include "window-server-actions.h"
#include "server-maintenance.h"
#include "server-command-tracker.h"
#include "window-worlds.h"
#include "window-minimap.h"
#include "minimap-store.h"
#include "minimap-cache.h"
#include "vendor/cjson/cJSON.h"
#include "refresh-button.h"
#include "build-metadata.h"
#include "i18n.h"
#include "language.h"
#include <locale.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>
#ifdef SMASHED_TEST_XTEST
#include <gdk/x11/gdkx.h>
#include <X11/extensions/XTest.h>
#endif

void smashed_pumpkin_register_resource(void);

static char *
notice_display_payload(PumpkinWindow *window, const char *notice)
{
  g_autofree char *display = format_console_line(window, notice, NULL);
  g_assert_nonnull(display);
  g_assert_cmpint(display[0], ==, '[');
  g_assert_true(g_ascii_isdigit(display[1]));
  const char *timestamp_end = strstr(display, "] ");
  g_assert_nonnull(timestamp_end);
  g_assert_cmpstr(timestamp_end + 2, !=, "");
  /* Compare normalized severity and message text without a newly generated
   * receipt time. Timestamp formatting has its own fixed-clock assertion. */
  return g_strdup(timestamp_end + 2);
}

static void spin(int milliseconds)
{
  gint64 end = g_get_monotonic_time() + milliseconds * 1000;
  do {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  } while (g_get_monotonic_time() < end);
}

static void screenshot(GtkWidget *window, const char *path)
{
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
  GskRenderNode *node = NULL;
  /* Wayland can defer the first frame after a resize or dialog transition. */
  for (guint attempt = 0; attempt < 10 && node == NULL; attempt++) {
    gtk_widget_queue_draw(window);
    spin(200);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, gtk_widget_get_width(window), gtk_widget_get_height(window));
    node = gtk_snapshot_free_to_node(snapshot);
  }
  if (node == NULL) g_test_message("No frame for %s (mapped: %d, size: %dx%d)", path,
    gtk_widget_get_mapped(window), gtk_widget_get_width(window), gtk_widget_get_height(window));
  g_assert_nonnull(node);
  GskRenderer *renderer = gsk_renderer_new_for_surface(gtk_native_get_surface(GTK_NATIVE(window)));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, NULL);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  gsk_renderer_unrealize(renderer);
  g_object_unref(renderer);
  gsk_render_node_unref(node);
}

static GtkWidget *find_widget(GtkWidget *root, GType type)
{
  if (g_type_is_a(G_OBJECT_TYPE(root), type)) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_widget(child, type);
    if (found != NULL) return found;
  }
  return NULL;
}

static GtkWidget *find_button(GtkWidget *root, const char *label)
{
  if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_button(child, label);
    if (found != NULL) return found;
  }
  return NULL;
}

static void click_dialog_backdrop(PumpkinWindow *window, double x, double y)
{
#ifdef SMASHED_TEST_XTEST
  if (g_getenv("SMASHED_PRIVATE_X11_POINTER") != NULL) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    /* GTK deprecated the X11 backend itself in 4.18. This optional regression
     * harness specifically needs its XID for real input on a private X server. */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Display *display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    Window xid = gdk_x11_surface_get_xid(surface), child;
    G_GNUC_END_IGNORE_DEPRECATIONS
    int root_x, root_y;
    XRaiseWindow(display, xid);
    XSetInputFocus(display, xid, RevertToParent, CurrentTime);
    XTranslateCoordinates(display, xid, DefaultRootWindow(display), (int)x, (int)y,
      &root_x, &root_y, &child);
    XTestFakeMotionEvent(display, -1, root_x, root_y, CurrentTime);
    XTestFakeButtonEvent(display, 1, True, CurrentTime);
    XTestFakeButtonEvent(display, 1, False, CurrentTime);
    XFlush(display);
    spin(300);
    g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    return;
  }
#endif
  g_assert_true(pumpkin_window_dismiss_dialog_at(GTK_WINDOW(window), x, y));
}

static GtkEventController *motion_for(GtkWidget *overlay)
{
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(overlay);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_MOTION(controller)) return controller;
    g_object_unref(controller);
  }
  g_error("No graph motion controller");
}

static char *buffer_text(GtkTextBuffer *buffer)
{
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

static void assert_log_tag(GtkTextBuffer *buffer, const char *word, const char *tag_name)
{
  g_autofree char *text = buffer_text(buffer);
  const char *position = strstr(text, word);
  g_assert_nonnull(position);
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_offset(buffer, &iter, (int)g_utf8_pointer_to_offset(text, position));
  GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), tag_name);
  g_assert_nonnull(tag);
  g_assert_true(gtk_text_iter_has_tag(&iter, tag));
}

static void test_console_polling(PumpkinWindow *window, PumpkinServer *server)
{
  const char *reply = "[INFO] [pumpkin::command] There are 0 of a max of 20 players online:";
  g_assert_true(is_auto_poll_noise_line(reply));
  g_assert_false(is_auto_poll_noise_line("[INFO] <Alex> There are 0 of a max of 20 players online:"));
  g_assert_false(is_auto_poll_noise_line("[INFO] <Alex> TPS: 20.0"));
  g_autoptr(PumpkinServer) other = pumpkin_server_store_add(window->store, "Polling isolation", NULL);
  ensure_server_log_handler(window, server);
  ensure_server_log_handler(window, other);
  GtkTextBuffer *buffer = g_hash_table_lookup(window->console_buffers, server);
  g_assert_nonnull(buffer);
  gtk_text_buffer_set_text(buffer, "", 0);
  pumpkin_command_tracking_reset(G_OBJECT(server));
  for (guint i = 0; i < 4; i++) {
    pumpkin_command_track(G_OBJECT(server), "list uuids", TRUE, g_get_monotonic_time());
  }
  pumpkin_window_select_server(window, pumpkin_server_get_id(other));
  g_assert_true(window->current == other);
  g_signal_emit_by_name(server, "log-line", reply);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  g_assert_true(window->current == server);
  for (guint i = 0; i < 3; i++) g_signal_emit_by_name(server, "log-line", reply);
  g_autofree char *text = buffer_text(buffer);
  g_assert_null(strstr(text, "of a max of"));
  g_assert_cmpint(window->list_snapshot_players, ==, 0);
  pumpkin_command_track(G_OBJECT(server), "list", FALSE, g_get_monotonic_time());
  g_signal_emit_by_name(server, "log-line", reply);
  g_clear_pointer(&text, g_free);
  text = buffer_text(buffer);
  g_assert_nonnull(strstr(text, "of a max of"));
  pumpkin_command_track(G_OBJECT(server), "list uuids", TRUE, g_get_monotonic_time());
  g_signal_emit_by_name(server, "log-line", "[INFO] <Alex> There are 0 of a max of 20 players online:");
  g_assert_false(pumpkin_command_poll_ready(G_OBJECT(server), "list uuids", g_get_monotonic_time()));
  g_signal_emit_by_name(server, "log-line", reply);
  g_assert_true(pumpkin_command_poll_ready(G_OBJECT(server), "list uuids", g_get_monotonic_time()));
  pumpkin_command_track(G_OBJECT(server), "tps", TRUE, g_get_monotonic_time());
  g_signal_emit_by_name(server, "log-line", "[INFO] <Alex> TPS: 20.0");
  g_assert_false(pumpkin_command_poll_ready(G_OBJECT(server), "tps", g_get_monotonic_time()));
  g_signal_emit_by_name(server, "log-line", "[INFO] TPS: 20.0 MSPT: 0.5ms");
  g_assert_true(pumpkin_command_poll_ready(G_OBJECT(server), "tps", g_get_monotonic_time()));
  pumpkin_command_track(G_OBJECT(server), "tps", TRUE, g_get_monotonic_time());
  window->ui_state = UI_STATE_RUNNING;
  window->user_stop_requested = TRUE;
  const char *ordinary_logs[] = {"Server process exited", "Auto-restart scheduled", "Serverprozess beendet"};
  for (guint i = 0; i < G_N_ELEMENTS(ordinary_logs); ++i) {
    g_signal_emit_by_name(server, "log-line", ordinary_logs[i]);
    g_assert_false(pumpkin_command_poll_ready(G_OBJECT(server), "tps", g_get_monotonic_time()));
    g_assert_cmpint(window->ui_state, ==, UI_STATE_RUNNING);
    g_assert_true(window->user_stop_requested);
  }
  g_clear_pointer(&text, g_free);
  text = buffer_text(buffer);
  for (guint i = 0; i < G_N_ELEMENTS(ordinary_logs); ++i) g_assert_nonnull(strstr(text, ordinary_logs[i]));
  g_signal_emit_by_name(server, "process-exited");
  g_assert_true(pumpkin_command_poll_ready(G_OBJECT(server), "tps", g_get_monotonic_time()));
  g_assert_cmpint(window->ui_state, ==, UI_STATE_IDLE);
  g_assert_false(window->user_stop_requested);
  /* An exit for a background server clears only that server's tracking. */
  pumpkin_command_track(G_OBJECT(other), "list uuids", TRUE, g_get_monotonic_time());
  pumpkin_command_track(G_OBJECT(server), "list uuids", TRUE, g_get_monotonic_time());
  g_signal_emit_by_name(other, "process-exited");
  g_assert_true(pumpkin_command_poll_ready(G_OBJECT(other), "list uuids", g_get_monotonic_time()));
  g_assert_false(pumpkin_command_poll_ready(G_OBJECT(server), "list uuids", g_get_monotonic_time()));
  pumpkin_command_tracking_reset(G_OBJECT(server));

  /* Display-only notices can contain protocol-like or translated text without
   * changing readiness, players, statistics, or pending poll replies. */
  ensure_server_log_handler(window, server);
  ensure_server_log_handler(window, server);
  pumpkin_command_track(G_OBJECT(server), "list uuids", TRUE, g_get_monotonic_time());
  pumpkin_command_track(G_OBJECT(server), "tps", TRUE, g_get_monotonic_time());
  guint player_states = g_hash_table_size(window->player_states);
  window->ui_state = UI_STATE_STARTING;
  window->list_snapshot_players = 3;
  const char *notices[] = {
    "Listening: application notice only",
    "[INFO] TPS: 1.0 MSPT: 100.0ms",
    "[INFO] There are 1 of a max of 20 players online: NoticePlayer",
    "Serverprozess beendet",
    "[SMPK] 服务器已停止；Done / TPS: 1.0 / 玩家 NoticePlayer"
  };
  gtk_text_buffer_set_text(buffer, "", 0);
  for (guint i = 0; i < G_N_ELEMENTS(notices); ++i) {
    g_signal_emit_by_name(server, "console-notice", notices[i]);
    g_assert_false(server_is_running_ui(window, server));
    g_assert_cmpint(window->ui_state, ==, UI_STATE_STARTING);
    g_assert_false(window->last_tps_valid);
    g_assert_cmpint(window->list_snapshot_players, ==, 3);
    g_assert_cmpuint(g_hash_table_size(window->player_states), ==, player_states);
    g_assert_false(pumpkin_command_poll_ready(G_OBJECT(server), "list uuids", g_get_monotonic_time()));
    g_assert_false(pumpkin_command_poll_ready(G_OBJECT(server), "tps", g_get_monotonic_time()));
  }
  /* Reading later must still find each notice exactly once, regardless of
   * the timestamp assigned when it was received. */
  spin(1100);
  g_clear_pointer(&text, g_free);
  text = buffer_text(buffer);
  for (guint i = 0; i < G_N_ELEMENTS(notices); ++i) {
    g_autofree char *display = notice_display_payload(window, notices[i]);
    g_assert_nonnull(strstr(text, display));
    g_assert_true(strstr(text, display) == g_strrstr(text, display));
  }
  window->ui_state = UI_STATE_IDLE;
  window->list_snapshot_players = 0;
  pumpkin_command_tracking_reset(G_OBJECT(server));
  g_print("PASS: late polling replies are isolated; lifecycle uses typed events, not raw English or translated logs\n");
  g_print("PASS: application notices display once without driving server protocol state\n");
}

static void test_console_layout(PumpkinWindow *window, PumpkinServer *server)
{
  const char *pages[] = { "console", "logs", "stats", "settings", "plugins" };
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(window->log_view);
  for (guint round = 0; round < 20; round++) {
    adw_view_stack_set_visible_child_name(window->details_stack, "console");
    gtk_text_buffer_set_text(buffer, "", 0);
    gtk_window_set_default_size(GTK_WINDOW(window), round % 2 ? 800 : 1180,
                                round % 2 ? 580 : 860);
    for (guint line = 0; line < 120; line++) {
      append_console_line(window, server, "[INFO] Layout regression: new lines create the console scrollbar");
    }
    spin(80);
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(window->log_view));
    if (gtk_widget_get_mapped(GTK_WIDGET(window->log_view))) {
      double bottom = MAX(gtk_adjustment_get_lower(adjustment),
        gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
      g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment), bottom, 1.0);
    }
    adw_view_stack_set_visible_child_name(window->details_stack, pages[round % G_N_ELEMENTS(pages)]);
    spin(20);
  }
  gtk_window_set_default_size(GTK_WINDOW(window), 1180, 860);
  adw_view_stack_set_visible_child_name(window->details_stack, "console");
  spin(200);
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(window->log_view));
  gtk_adjustment_set_value(adjustment, 0);
  adw_view_stack_set_visible_child_name(window->details_stack, "stats");
  spin(200);
  double hidden_value = gtk_adjustment_get_value(adjustment);
  append_console_line(window, server, "[INFO] Hidden output must not start a scroll animation");
  spin(100);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment), hidden_value, 1.0);
  adw_view_stack_set_visible_child_name(window->details_stack, "console");
  spin(200);
  double bottom = MAX(gtk_adjustment_get_lower(adjustment),
    gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment), bottom, 1.0);
  g_print("PASS: console scrollbar creation, buffer clearing, page changes and resizing produce no GTK warnings\n");
}

static GtkWidget *find_tooltip(GtkWidget *root, const char *tooltip)
{
  if (g_strcmp0(gtk_widget_get_tooltip_text(root), tooltip) == 0) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_tooltip(child, tooltip);
    if (found != NULL) return found;
  }
  return NULL;
}

static GtkWidget *find_label(GtkWidget *root, const char *text)
{
  if (GTK_IS_LABEL(root) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text) == 0) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_label(child, text);
    if (found != NULL) return found;
  }
  return NULL;
}

static GtkListBoxRow *world_row(PumpkinWindow *window, const char *name)
{
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(window->world_list)); row != NULL;
       row = gtk_widget_get_next_sibling(row)) {
    if (ADW_IS_PREFERENCES_ROW(row) && g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), name) == 0)
      return GTK_LIST_BOX_ROW(row);
  }
  return NULL;
}

static char *world_fixture(const char *parent, const char *name)
{
  char *path = g_build_filename(parent, name, NULL);
  g_assert_cmpint(g_mkdir_with_parents(path, 0700), ==, 0);
  g_autofree char *level = g_build_filename(path, "level.dat", NULL);
  g_assert_true(g_file_set_contents(level, "test-world", -1, NULL));
  return path;
}

static void test_worlds(PumpkinWindow *window, PumpkinServer *server)
{
  g_autofree char *data = pumpkin_server_get_data_dir(server);
  g_autofree char *storage = pumpkin_server_get_worlds_dir(server);
  g_autofree char *configured = world_fixture(data, "Configured World");
  g_autofree char *archive = world_fixture(storage, "Creative Archive");
  g_autofree char *backup = world_fixture(storage, "Survival Backup");
  g_autofree char *literal = world_fixture(storage, "World & More");
  g_autofree char *unknown = g_build_filename(storage, "Unrecognized Folder", NULL);
  g_assert_cmpint(g_mkdir(unknown, 0700), ==, 0);
  g_autofree char *config_dir = g_build_filename(data, "config", NULL);
  g_assert_cmpint(g_mkdir_with_parents(config_dir, 0700), ==, 0);
  g_autofree char *config = g_build_filename(data, "pumpkin.toml", NULL);
  g_assert_true(g_file_set_contents(config, "default_level_name = 'Configured World'\n", -1, NULL));
  adw_view_stack_set_visible_child_name(window->details_stack, "worlds");
  refresh_world_list(window);
  spin(350);
  GtkListBoxRow *row = world_row(window, "Configured World");
  g_assert_nonnull(row);
  g_assert_true(row == gtk_list_box_get_row_at_index(window->world_list, 0));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), "Configured world"));
  g_assert_null(g_object_get_data(G_OBJECT(world_row(window, "Unrecognized Folder")), "world-remove-button"));
  g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(world_row(window, "World & More"))));
  GtkWidget *search = find_widget(GTK_WIDGET(window->world_page), GTK_TYPE_SEARCH_ENTRY);
  gtk_editable_set_text(GTK_EDITABLE(search), "creative");
  spin(200);
  g_assert_false(gtk_widget_get_child_visible(GTK_WIDGET(row)));
  g_assert_true(gtk_widget_get_child_visible(GTK_WIDGET(world_row(window, "Creative Archive"))));
  gtk_editable_set_text(GTK_EDITABLE(search), "no-matching-world");
  spin(200);
  GtkWidget *placeholder = find_label(GTK_WIDGET(window->world_list), "No matching worlds");
  g_assert_nonnull(placeholder);
  g_assert_true(gtk_widget_get_child_visible(placeholder));
  g_assert_nonnull(find_label(GTK_WIDGET(window->world_page), "0 of 5 worlds"));
  gtk_editable_set_text(GTK_EDITABLE(search), "");
  spin(200);
  AdwStyleManager *style = adw_style_manager_get_default();
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_LIGHT);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-worlds-light.png");
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-worlds-dark.png");
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 580);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-worlds-compact.png");
  gtk_window_set_default_size(GTK_WINDOW(window), 1180, 860);
  spin(200);

  GtkButton *reload = GTK_BUTTON(find_tooltip(GTK_WIDGET(window->world_page), "Reload worlds"));
  g_assert_nonnull(reload);
  g_signal_emit_by_name(reload, "clicked");
  g_assert_true(pumpkin_refresh_button_is_animating(reload));
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(reload)));
  g_assert_false(gtk_widget_get_can_target(GTK_WIDGET(reload)));
  g_assert_null(gtk_widget_pick(GTK_WIDGET(reload),
    gtk_widget_get_width(GTK_WIDGET(reload)) / 2.0, gtk_widget_get_height(GTK_WIDGET(reload)) / 2.0,
    GTK_PICK_DEFAULT));
  g_assert_false(pumpkin_refresh_button_begin(reload));
  g_signal_emit_by_name(reload, "clicked");
  spin(120);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-worlds-reload.png");
  spin(550);
  g_assert_false(pumpkin_refresh_button_is_animating(reload));
  g_assert_true(gtk_widget_get_can_target(GTK_WIDGET(reload)));
  GtkSettings *settings = gtk_widget_get_settings(GTK_WIDGET(window));
  gboolean animations;
  g_object_get(settings, "gtk-enable-animations", &animations, NULL);
  g_object_set(settings, "gtk-enable-animations", FALSE, NULL);
  g_assert_true(pumpkin_refresh_button_begin(reload));
  g_assert_false(pumpkin_refresh_button_begin(reload));
  adw_view_stack_set_visible_child_name(window->details_stack, "console");
  spin(550);
  g_assert_false(pumpkin_refresh_button_is_animating(reload));
  g_object_set(settings, "gtk-enable-animations", animations, NULL);
  adw_view_stack_set_visible_child_name(window->details_stack, "worlds");

  GtkWidget *import = find_button(GTK_WIDGET(window->world_page), "Import World...");
  g_assert_true(gtk_widget_get_sensitive(import));
  g_assert_true(pumpkin_server_begin_maintenance(server, NULL));
  g_assert_false(gtk_widget_get_sensitive(import));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->btn_details_start)));
  pumpkin_server_end_maintenance(server);
  g_assert_true(gtk_widget_get_sensitive(import));

  row = world_row(window, "Survival Backup");
  GtkWidget *menu = find_widget(GTK_WIDGET(row), GTK_TYPE_MENU_BUTTON);
  gtk_menu_button_popup(GTK_MENU_BUTTON(menu));
  spin(100);
  GtkWidget *remove = g_object_get_data(G_OBJECT(row), "world-remove-button");
  g_signal_emit_by_name(remove, "clicked");
  spin(100);
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  g_assert_true(pumpkin_server_begin_maintenance(server, NULL));
  GtkWidget *confirm = find_button(GTK_WIDGET(dialog), "Remove World");
  g_assert_nonnull(confirm);
  g_signal_emit_by_name(confirm, "clicked");
  spin(250);
  g_assert_true(g_file_test(backup, G_FILE_TEST_IS_DIR));
  pumpkin_server_end_maintenance(server);

  /* Dialog context must retain the original server, not whatever is selected later. */
  g_autoptr(PumpkinServer) other = pumpkin_server_store_add(window->store, "Other Worlds Server", NULL);
  g_autofree char *other_storage = pumpkin_server_get_worlds_dir(other);
  g_autofree char *other_world = world_fixture(other_storage, "Survival Backup");
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  spin(200);
  row = world_row(window, "Survival Backup");
  menu = find_widget(GTK_WIDGET(row), GTK_TYPE_MENU_BUTTON);
  gtk_menu_button_popup(GTK_MENU_BUTTON(menu));
  spin(80);
  refresh_world_list(window);
  g_assert_true(row == world_row(window, "Survival Backup")); /* Never destroy an open popover. */
  remove = g_object_get_data(G_OBJECT(row), "world-remove-button");
  g_signal_emit_by_name(remove, "clicked");
  spin(150);
  dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  pumpkin_window_select_server(window, pumpkin_server_get_id(other));
  confirm = find_button(GTK_WIDGET(dialog), "Remove World");
  g_assert_nonnull(confirm);
  g_signal_emit_by_name(confirm, "clicked");
  spin(350);
  g_assert_false(g_file_test(backup, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(other_world, G_FILE_TEST_IS_DIR));
  g_assert_true(window->current == other);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  spin(200);

  g_autofree char *source = world_fixture(g_get_home_dir(), "Imported World");
  g_autoptr(GFile) file = g_file_new_for_path(source);
  GFile *files[] = { file };
  GdkFileList *file_list = gdk_file_list_new_from_array(files, G_N_ELEMENTS(files));
  GValue value = G_VALUE_INIT;
  g_value_init(&value, GDK_TYPE_FILE_LIST);
  g_value_take_boxed(&value, file_list);
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(GTK_WIDGET(window->world_page));
  gboolean handled = FALSE;
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GObject) controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_DROP_TARGET(controller)) g_signal_emit_by_name(controller, "drop", &value, 0.0, 0.0, &handled);
  }
  g_value_unset(&value);
  g_assert_true(handled);
  spin(350);
  g_assert_false(pumpkin_server_get_maintenance(server));
  g_assert_nonnull(world_row(window, "Imported World"));
  g_assert_true(g_file_test(source, G_FILE_TEST_IS_DIR));
  g_assert_true(g_file_test(configured, G_FILE_TEST_IS_DIR));
  /* A bad config must produce a retryable page, not an endless loading state. */
  g_assert_true(g_file_set_contents(config, "default_level_name = ", -1, NULL));
  refresh_world_list(window);
  spin(200);
  AdwStatusPage *status = ADW_STATUS_PAGE(find_widget(GTK_WIDGET(window->world_page), ADW_TYPE_STATUS_PAGE));
  g_assert_cmpstr(adw_status_page_get_title(status), ==, "Could Not Load Worlds");
  g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(status)));
  screenshot(GTK_WIDGET(window), "/tmp/smashed-worlds-error.png");
  g_assert_true(g_file_set_contents(config, "default_level_name = 'Configured World'\n", -1, NULL));
  refresh_world_list(window);
  refresh_world_list(window);
  spin(250);
  g_assert_nonnull(world_row(window, "Configured World"));
  g_print("PASS: worlds, configuration, async import, safe mutation, search and non-grey reload click lock\n");
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
}

static void test_reload_controls(PumpkinWindow *window)
{
  gboolean loading = window->marketplace_loading;
  window->marketplace_loading = TRUE; /* Keep this local; no external HTTP request needed. */
  g_signal_emit_by_name(window->btn_marketplace_refresh, "clicked");
  g_assert_false(pumpkin_refresh_button_is_animating(window->btn_marketplace_refresh));
  window->marketplace_loading = loading;

  GtkButton *buttons[] = { window->btn_marketplace_refresh, window->btn_marketplace_retry, window->btn_refresh_logs };
  for (guint i = 0; i < G_N_ELEMENTS(buttons); i++) {
    g_assert_true(pumpkin_refresh_button_begin(buttons[i]));
    g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(buttons[i])));
    g_assert_false(pumpkin_refresh_button_begin(buttons[i]));
  }
  g_assert_nonnull(find_label(GTK_WIDGET(window->btn_marketplace_retry), "Reload"));
  g_signal_emit_by_name(window->btn_marketplace_refresh, "clicked");
  g_signal_emit_by_name(window->btn_marketplace_retry, "clicked");
  g_assert_true(window->marketplace_loading == loading);
  spin(550);
  for (guint i = 0; i < G_N_ELEMENTS(buttons); i++) g_assert_false(pumpkin_refresh_button_is_animating(buttons[i]));
  g_print("PASS: reload controls preserve text and styling; busy/repeated activation is ignored\n");
}

static void test_logs(PumpkinWindow *window, PumpkinServer *server)
{
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_autofree char *bin_dir = g_path_get_dirname(bin);
  g_mkdir_with_parents(bin_dir, 0700);
  g_autofree char *binary = NULL;
  gsize binary_size = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_PUMPKIN_BINARY"), &binary, &binary_size, NULL));
  /* Match the app's size check without shipping a real server in the test. */
  if (binary_size < MIN_VALID_PUMPKIN_BINARY_BYTES) {
    gsize padded_size = MIN_VALID_PUMPKIN_BINARY_BYTES;
    binary = g_realloc(binary, padded_size);
    memset(binary + binary_size, 0, padded_size - binary_size);
    binary_size = padded_size;
  }
  g_assert_true(g_file_set_contents(bin, binary, (gssize)binary_size, NULL));
  g_assert_cmpint(g_chmod(bin, 0700), ==, 0);
  window->user_stop_requested = TRUE;
  append_console_line(window, server, "[SMPK] Old session must disappear");
  g_autoptr(GError) error = NULL;
  gboolean started = pumpkin_server_start(server, &error);
  g_assert_no_error(error);
  g_assert_true(started);
  spin(100);
  GtkTextBuffer *console = gtk_text_view_get_buffer(window->log_view);
  g_autofree char *text = buffer_text(console);
  g_assert_null(strstr(text, "Old session"));
  g_assert_nonnull(strstr(text, "Server session started"));
  assert_log_tag(console, "INFO", "console-token-info");
  assert_log_tag(console, "WARN", "console-token-warn");
  assert_log_tag(console, "ERROR", "console-token-error");
  append_console_line(window, server, "\033[2m19:06:39\033[0m \033[33m WARN\033[0m Short clock warning");
  assert_log_tag(console, "WARN] Short clock", "console-token-warn");
  ConsoleLevel level;
  g_autofree char *clock_line = format_console_line(window, "19:06:39 INFO Clock format", &level);
  g_assert_cmpint(level, ==, CONSOLE_LEVEL_INFO);
  g_assert_cmpstr(clock_line, ==, "[19:06:39] [INFO] Clock format");
  append_console_line(window, server, "[SMPK] Running session stays");
  g_assert_true(pumpkin_server_start(server, &error));
  g_clear_pointer(&text, g_free);
  text = buffer_text(console);
  g_assert_nonnull(strstr(text, "Running session stays"));
  spin(650);
  window->user_stop_requested = TRUE;
  started = pumpkin_server_start(server, &error);
  g_assert_no_error(error);
  g_assert_true(started);
  spin(100);
  g_clear_pointer(&text, g_free);
  text = buffer_text(console);
  g_assert_null(strstr(text, "Running session stays"));
  spin(650);
  g_autofree char *logs_dir = pumpkin_server_get_logs_dir(server);
  GDir *dir = g_dir_open(logs_dir, 0, NULL);
  g_assert_nonnull(dir);
  guint count = 0;
  const char *entry;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    if (!g_str_has_suffix(entry, ".log")) continue;
    g_autofree char *path = g_build_filename(logs_dir, entry, NULL);
    g_autofree char *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "Server session started"));
    g_assert_nonnull(strstr(contents, "Server process exited"));
    g_assert_nonnull(strstr(contents, "Example error"));
    count++;
  }
  g_dir_close(dir);
  g_assert_cmpuint(count, ==, 2);

  const char *fixture = "2026-10-08T09:30:00.123Z INFO pumpkin::server: Server session started\n"
    "2026-10-08T09:30:01.456Z DEBUG pumpkin::plugin: Loading plugins\n"
    "[08.10.2026 11:30:02] [INFO] [pumpkin::server] Ready in 240ms\n"
    "[08.10.2026 11:30:03] [WARNING] A tick took longer than expected\n"
    "[08.10.2026 11:30:04] [ERROR] Could not load ExamplePlugin\n"
    "[08.10.2026 11:30:05] [CRITICAL] Example critical message\n"
    "[08.10.2026 11:30:06] [TRACE] Tick trace\n"
    "\033[2m19:06:39\033[0m \033[33m WARN\033[0m Short clock warning\n"
    "No synthetic timestamp in archived messages\n";
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
  g_autofree char *name = g_strdup_printf("session-%s-999999.log", stamp);
  g_autofree char *path = g_build_filename(logs_dir, name, NULL);
  g_assert_true(g_file_set_contents(path, fixture, -1, NULL));
  g_signal_emit_by_name(window->btn_refresh_logs, "clicked");
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(window->log_files_list, 0);
  g_assert_cmpstr(g_object_get_data(G_OBJECT(row), "log-path"), ==, path);
  gtk_list_box_select_row(window->log_files_list, row);
  GtkTextBuffer *archive = gtk_text_view_get_buffer(window->log_file_view);
  assert_log_tag(archive, "INFO", "console-token-info");
  assert_log_tag(archive, "DEBUG", "console-token-debug");
  assert_log_tag(archive, "WARNING", "console-token-warn");
  assert_log_tag(archive, "ERROR", "console-token-error");
  assert_log_tag(archive, "CRITICAL", "console-token-error");
  assert_log_tag(archive, "TRACE", "console-token-trace");
  assert_log_tag(archive, "WARN] Short clock", "console-token-warn");
  g_clear_pointer(&text, g_free);
  text = buffer_text(archive);
  g_assert_nonnull(strstr(text, "\nNo synthetic timestamp"));
  g_signal_emit_by_name(window->btn_refresh_logs, "clicked");
  g_assert_cmpstr(window->current_log_path, ==, path);
  gtk_drop_down_set_selected(window->log_level_filter, 3);
  g_clear_pointer(&text, g_free);
  text = buffer_text(archive);
  g_assert_nonnull(strstr(text, "critical message"));
  g_assert_null(strstr(text, "Ready"));
  gtk_drop_down_set_selected(window->log_level_filter, 0);
  g_autofree char *clock = g_date_time_format(now, "%H:%M:%S");
  gtk_editable_set_text(GTK_EDITABLE(window->log_search), clock);
  g_assert_nonnull(gtk_list_box_get_row_at_index(window->log_files_list, 0));
  gtk_editable_set_text(GTK_EDITABLE(window->log_search), "");

  g_autoptr(GtkTextBuffer) unicode = gtk_text_buffer_new(NULL);
  ensure_log_buffer_tags(unicode);
  const char *unicode_line = "[8 Oktober, spät] [WARN] Unicode before severity, 250ms";
  gtk_text_buffer_set_text(unicode, unicode_line, -1);
  apply_console_inline_tags(unicode, 0, unicode_line, CONSOLE_LEVEL_WARN);
  assert_log_tag(unicode, "WARN", "console-token-warn");
  assert_log_tag(unicode, "250ms", "console-token-startup-ms");
  adw_view_stack_set_visible_child_name(window->details_stack, "logs");
  AdwStyleManager *style = adw_style_manager_get_default();
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_LIGHT);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-logs-light.png");
  GtkTextTag *info_tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(archive), "console-token-info");
  GdkRGBA *light_color = NULL;
  g_object_get(info_tag, "foreground-rgba", &light_color, NULL);
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-logs-dark.png");
  GdkRGBA *dark_color = NULL;
  g_object_get(info_tag, "foreground-rgba", &dark_color, NULL);
  g_assert_false(gdk_rgba_equal(light_color, dark_color));
  gdk_rgba_free(light_color);
  gdk_rgba_free(dark_color);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 580);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-logs-compact.png");
  gtk_window_set_default_size(GTK_WINDOW(window), 1180, 860);
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
  gtk_editable_set_text(GTK_EDITABLE(window->log_search), "no-such-session");
  g_assert_null(gtk_list_box_get_row_at_index(window->log_files_list, 0));
  g_assert_null(window->current_log_path);
  gtk_editable_set_text(GTK_EDITABLE(window->log_search), "");
  g_assert_nonnull(gtk_list_box_get_selected_row(window->log_files_list));
  g_remove(bin);
  g_clear_pointer(&text, g_free);
  text = buffer_text(console);
  g_assert_false(pumpkin_server_start(server, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_autofree char *after_failure = buffer_text(console);
  g_assert_cmpstr(after_failure, ==, text);
  g_print("PASS: fresh consoles, persistent sessions, severity colors, Unicode, archive sorting/filtering/selection\n");
}

static guint
player_row_count(GtkListBox *list)
{
  guint count = 0;
  while (gtk_list_box_get_row_at_index(list, (int)count) != NULL) count++;
  return count;
}

static void
cache_test_head(const char *uuid, guint variant)
{
  g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "smashed-pumpkin", "player-heads-v2", NULL);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  g_autofree char *file = g_strdup_printf("%s/%s.png", dir, uuid);
  guchar *pixels = g_malloc(64 * 64 * 4);
  for (guint y = 0; y < 64; y++) {
    for (guint x = 0; x < 64; x++) {
      guint at = (y * 64 + x) * 4;
      gboolean hair = y < 16 || x < 8 || x >= 56;
      gboolean eye = y >= 24 && y < 32 && ((x >= 16 && x < 24) || (x >= 40 && x < 48));
      gboolean mouth = y >= 48 && y < 56 && x >= 24 && x < 40;
      pixels[at] = hair || mouth ? 62 + variant * 12 : 194;
      pixels[at + 1] = hair || mouth ? 38 + variant * 9 : 139;
      pixels[at + 2] = eye ? 220 : (hair || mouth ? 27 : 95);
      pixels[at + 3] = 255;
    }
  }
  g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, 64 * 64 * 4);
  g_autoptr(GdkTexture) texture = GDK_TEXTURE(gdk_memory_texture_new(64, 64, GDK_MEMORY_R8G8B8A8, bytes, 64 * 4));
  g_assert_true(gdk_texture_save_to_png(texture, file));
}

static void
remove_test_player(PumpkinWindow *window, const char *key)
{
  PlayerState *state = g_hash_table_lookup(window->player_states, key);
  g_assert_nonnull(state);
  g_autofree char *name_key = normalized_key(state->name);
  if (g_hash_table_lookup(window->player_states_by_name, name_key) == state)
    g_hash_table_remove(window->player_states_by_name, name_key);
  g_hash_table_remove(window->player_states_by_uuid, key);
  g_assert_true(g_hash_table_remove(window->player_states, key));
}

static void
test_skin_pipeline(void)
{
  const char *uuid = "faf39d18-9aae-4294-9289-ba3836ccb271";
  const char *hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  g_autofree char *payload = g_strdup_printf(
    "{\"textures\":{\"SKIN\":{\"url\":\"http://textures.minecraft.net/texture/%s\"}}}", hash);
  g_autofree char *encoded = g_base64_encode((const guchar *)payload, strlen(payload));
  g_autofree char *profile = g_strdup_printf(
    "{\"id\":\"faf39d189aae42949289ba3836ccb271\",\"properties\":[{\"name\":\"textures\",\"value\":\"%s\"}]}", encoded);
  g_autofree char *url = pumpkin_player_skin_url(profile, uuid);
  g_autofree char *expected = g_strdup_printf("https://textures.minecraft.net/texture/%s", hash);
  g_assert_cmpstr(url, ==, expected);
  g_assert_null(pumpkin_player_skin_url(profile, "069a79f4-44e9-4726-a5be-fca90e38aaf5"));
  g_assert_null(pumpkin_player_skin_url("malformed", uuid));
  g_autofree char *evil_payload = g_strdup_printf(
    "{\"textures\":{\"SKIN\":{\"url\":\"https://evil.invalid/texture/%s\"}}}", hash);
  g_autofree char *evil_encoded = g_base64_encode((const guchar *)evil_payload, strlen(evil_payload));
  g_autofree char *evil_profile = g_strdup_printf(
    "{\"id\":\"faf39d189aae42949289ba3836ccb271\",\"properties\":[{\"name\":\"textures\",\"value\":\"%s\"}]}", evil_encoded);
  g_assert_null(pumpkin_player_skin_url(evil_profile, uuid));
  g_autoptr(GdkPixbuf) skin = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 64, 64);
  gdk_pixbuf_fill(skin, 0);
  guchar *pixels = gdk_pixbuf_get_pixels(skin);
  int stride = gdk_pixbuf_get_rowstride(skin);
  for (int y = 8; y < 16; y++) {
    for (int x = 8; x < 16; x++) {
      guchar *pixel = pixels + y * stride + x * 4;
      pixel[0] = 200; pixel[1] = 30; pixel[2] = 20; pixel[3] = 255;
    }
  }
  guchar *hat = pixels + 8 * stride + 40 * 4;
  hat[2] = 255; hat[3] = 255;
  char *png = NULL;
  gsize size;
  g_assert_true(gdk_pixbuf_save_to_buffer(skin, &png, &size, "png", NULL, NULL));
  g_autoptr(GBytes) bytes = g_bytes_new_take(png, size);
  g_autoptr(GdkTexture) head = pumpkin_player_head_from_skin(bytes);
  g_assert_nonnull(head);
  g_assert_cmpint(gdk_texture_get_width(head), ==, 64);
  g_autoptr(GBytes) head_png = gdk_texture_save_to_png_bytes(head);
  g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(head_png);
  g_autoptr(GdkPixbuf) decoded = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
  const guchar *result = gdk_pixbuf_read_pixels(decoded);
  g_assert_cmpuint(result[2], ==, 255);
  g_assert_cmpuint(result[4 * 8], ==, 200);
  g_autoptr(GBytes) invalid = g_bytes_new_static("not-png", 7);
  g_assert_null(pumpkin_player_head_from_skin(invalid));

  g_print("PASS: Mojang identity, trusted texture URL, face/hat crop and nearest scaling\n");
  /* External service availability must not gate unrelated UI regressions. */
  if (g_getenv("SMASHED_TEST_LIVE_SKINS") == NULL) {
    cache_test_head(uuid, 0);
    g_autofree char *marker = g_build_filename(g_get_user_cache_dir(), "smashed-pumpkin",
      "player-heads-v2", "faf39d18-9aae-4294-9289-ba3836ccb271.png.java", NULL);
    g_assert_true(g_file_set_contents(marker, "verified-test-fixture", -1, NULL));
    return;
  }
  /* Optional real-service check, still using only the isolated test cache. */
  g_autoptr(GtkWidget) live = g_object_ref_sink(gtk_image_new());
  pumpkin_player_head_set(GTK_IMAGE(live), uuid, "Rotstein152", FALSE, FALSE);
  gint64 deadline = g_get_monotonic_time() + 22 * G_USEC_PER_SEC;
  while (!pumpkin_player_head_is_java(GTK_IMAGE(live)) && g_get_monotonic_time() < deadline) spin(50);
  g_assert_true(pumpkin_player_head_is_java(GTK_IMAGE(live)));
  g_assert_nonnull(gtk_image_get_paintable(GTK_IMAGE(live)));
  g_assert_true(gdk_texture_save_to_png(GDK_TEXTURE(gtk_image_get_paintable(GTK_IMAGE(live))),
    "/tmp/smashed-verified-player-head.png"));
  g_print("PASS: live Mojang custom skin\n");
}

static void
test_players(PumpkinWindow *window, PumpkinServer *server)
{
  test_skin_pipeline();
  const char *java_uuid = "069a79f4-44e9-4726-a5be-fca90e38aaf5";
  const char *bedrock_uuid = "00000000-0000-0000-0009-01f64b7f326e";
  const char *offline_uuid = "8667ba71-b85a-4004-af54-457a9734eed7";
  /* Keep the fixture's online states independent of the stopped fake server. */
  if (window->stats_refresh_id != 0) g_source_remove(window->stats_refresh_id);
  window->stats_refresh_id = 0;
  player_states_clear(window);
  g_assert_null(ensure_player_state(window, "not-a-uuid", "bad\nname", TRUE));
  PlayerState *a = ensure_player_state(window, java_uuid, "SameName", TRUE);
  PlayerState *b = ensure_player_state(window, bedrock_uuid, "SameName", TRUE);
  g_assert_true(a != b);
  g_assert_cmpuint(g_hash_table_size(window->player_states), ==, 2);
  g_assert_true(ensure_player_state(window, java_uuid, "NewName", TRUE) == a);
  g_assert_true(ensure_player_state(window, bedrock_uuid, "SameName", FALSE) == b);
  player_state_mark_online(window, a, PLAYER_PLATFORM_JAVA);
  g_assert_true(ensure_player_state(window, java_uuid, "RenamedOnline", TRUE) == a);
  g_assert_false(g_hash_table_contains(window->live_player_names, "NewName"));
  g_assert_true(g_hash_table_contains(window->live_player_names, "RenamedOnline"));
  player_state_mark_offline(window, a);
  player_states_clear(window);

  player_states_consume_log(window, "\033[2m19:06:40\033[0m \033[32m INFO\033[0m ClockPlayer joined the game");
  PlayerState *clock_player = ensure_player_state(window, NULL, "ClockPlayer", FALSE);
  g_assert_nonnull(clock_player);
  g_assert_true(clock_player->online);
  player_states_consume_log(window, "19:06:41 INFO ClockPlayer left the game");
  g_assert_false(clock_player->online);
  player_states_clear(window);

  player_states_consume_log(window, "[DEBUG] [pumpkin::net] Accepted connection from Java Edition: 127.0.0.1:1000 (id 1)");
  player_states_consume_log(window, "[DEBUG] [pumpkin::net] Accepted connection from Bedrock Edition: 127.0.0.1:2000 (id 2)");
  player_states_consume_log(window, "[INFO] [pumpkin::world] JavaClient[/127.0.0.1:1000] logged in with entity id 1");
  player_states_consume_log(window, "[INFO] [pumpkin::world] BedrockClient[/127.0.0.1:2000] logged in with entity id 2");
  PlayerState *java_client = ensure_player_state(window, NULL, "JavaClient", FALSE);
  PlayerState *bedrock_client = ensure_player_state(window, NULL, "BedrockClient", FALSE);
  g_assert_cmpint(java_client->platform, ==, PLAYER_PLATFORM_JAVA);
  g_assert_cmpint(bedrock_client->platform, ==, PLAYER_PLATFORM_BEDROCK);
  g_assert_false(java_client->online);
  g_assert_false(bedrock_client->online);
  player_states_clear(window);

  player_states_consume_log(window, "[08.10.2026 12:00:00] [INFO] [pumpkin::world] JavaBuilder joined the game");
  PlayerState *unknown = ensure_player_state(window, NULL, "JavaBuilder", FALSE);
  g_assert_nonnull(unknown);
  g_assert_true(unknown->online);
  g_assert_cmpint(unknown->platform, ==, PLAYER_PLATFORM_UNKNOWN);
  player_states_consume_log(window, "[INFO] [pumpkin::world] <Someone> Fake joined the game");
  g_assert_cmpuint(g_hash_table_size(window->player_states), ==, 1);
  player_states_consume_log(window, "[INFO] [pumpkin::net::bedrock::login] Some Player joined the game");
  PlayerState *bedrock = ensure_player_state(window, bedrock_uuid, "Some Player", TRUE);
  g_assert_cmpint(bedrock->platform, ==, PLAYER_PLATFORM_BEDROCK);
  player_states_consume_log(window, "[INFO] [pumpkin::net::java::play] Rotstein joined the game");
  PlayerState *java = ensure_player_state(window, java_uuid, "Rotstein", TRUE);
  g_assert_cmpint(java->platform, ==, PLAYER_PLATFORM_JAVA);
  player_states_consume_log(window, "[INFO] [pumpkin::command] There are 3 of a max of 20 players online: Rotstein (069a79f4-44e9-4726-a5be-fca90e38aaf5)");
  g_assert_true(bedrock->online);
  g_assert_true(unknown->online);
  player_states_consume_log(window, "[INFO] [pumpkin::command] There are 2 of a max of 20 players online: Rotstein (069a79f4-44e9-4726-a5be-fca90e38aaf5), Some Player (00000000-0000-0000-0009-01f64b7f326e)");
  g_assert_true(java->online);
  g_assert_true(bedrock->online);
  g_assert_false(unknown->online);
  g_assert_cmpint(bedrock->platform, ==, PLAYER_PLATFORM_BEDROCK);
  g_assert_cmpuint(g_hash_table_size(window->player_states), ==, 3);
  player_states_consume_log(window, "[INFO] [pumpkin::world] Some Player left the game");
  g_assert_false(bedrock->online);
  player_states_consume_log(window, "[INFO] [pumpkin::command] There are 0 of a max of 20 players online:");
  g_assert_cmpint(player_online_count(window), ==, 0);
  player_states_consume_log(window, "[INFO] [pumpkin::world] Rotstein joined the game");
  g_signal_emit_by_name(server, "session-started");
  g_assert_cmpint(player_online_count(window), ==, 0);
  player_states_clear(window);

  PlayerState *verified = ensure_player_state(window,
    "faf39d18-9aae-4294-9289-ba3836ccb271", "Rotstein152", TRUE);
  g_assert_cmpint(verified->platform, ==, PLAYER_PLATFORM_UNKNOWN);
  invalidate_player_list_signature(window);
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_assert_cmpint(verified->platform, ==, PLAYER_PLATFORM_JAVA);
  player_states_clear(window);

  cache_test_head(java_uuid, 0);
  cache_test_head(bedrock_uuid, 1);
  cache_test_head(offline_uuid, 2);
  g_autoptr(GtkWidget) native_head = g_object_ref_sink(gtk_image_new());
  pumpkin_player_head_set(GTK_IMAGE(native_head), offline_uuid, "Bedrock Player", FALSE, TRUE);
  g_assert_null(gtk_image_get_paintable(GTK_IMAGE(native_head)));
  java = ensure_player_state(window, java_uuid, "Rotstein", TRUE);
  player_state_mark_online(window, java, PLAYER_PLATFORM_JAVA);
  java->playtime_seconds = 12420;
  bedrock = ensure_player_state(window, bedrock_uuid, "Some Player", TRUE);
  player_state_mark_online(window, bedrock, PLAYER_PLATFORM_BEDROCK);
  bedrock->playtime_seconds = 3240;
  unknown = ensure_player_state(window, NULL, "JavaBuilder", TRUE);
  player_state_mark_online(window, unknown, PLAYER_PLATFORM_UNKNOWN);
  PlayerState *offline = ensure_player_state(window, offline_uuid, "Alex", TRUE);
  offline->platform = PLAYER_PLATFORM_JAVA;
  offline->playtime_seconds = 28380;
  offline->last_online_unix = g_get_real_time() / G_USEC_PER_SEC - 7200;
  offline->first_joined_unix = offline->last_online_unix - 6 * 86400;
  ensure_player_state(window, NULL, "Long_Player_Name_For_Layout_Check", TRUE);
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  g_autofree char *ops_path = g_build_filename(data_dir, "ops.json", NULL);
  g_autofree char *whitelist_path = g_build_filename(data_dir, "whitelist.json", NULL);
  g_assert_true(g_file_set_contents(ops_path,
    "[{\"uuid\":\"069a79f4-44e9-4726-a5be-fca90e38aaf5\",\"name\":\"Rotstein\",\"level\":4}]", -1, NULL));
  g_assert_true(g_file_set_contents(whitelist_path,
    "[{\"uuid\":\"00000000-0000-0000-0009-01f64b7f326e\",\"name\":\"Some Player\"}]", -1, NULL));
  g_autofree char *players_dir = pumpkin_server_get_players_dir(server);
  g_assert_cmpint(g_mkdir_with_parents(players_dir, 0700), ==, 0);
  g_autofree char *junk = g_build_filename(players_dir, "not-a-player.txt", NULL);
  g_assert_true(g_file_set_contents(junk, "ignore me", -1, NULL));
  adw_view_stack_set_visible_child_name(window->details_stack, "players");
  adw_view_stack_set_visible_child_name(window->players_stack, "all_players");
  window->player_list_pointer_inside = TRUE;
  window->player_list_interaction_until = 0;
  invalidate_player_list_signature(window);
  g_signal_emit_by_name(window->player_search, "search-changed");
  spin(300);
  g_assert_cmpuint(player_row_count(window->player_list), ==, 5);
  g_assert_cmpstr(gtk_label_get_text(window->player_summary), ==, "3 online  ·  5 known players");
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(window->player_list, 0);
  g_assert_true(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(first), "player-online")) != 0);
  GtkWidget *image = find_widget(GTK_WIDGET(first), GTK_TYPE_IMAGE);
  g_assert_nonnull(image);
  g_assert_cmpint(gtk_image_get_pixel_size(GTK_IMAGE(image)), ==, 48);
  g_assert_nonnull(gtk_image_get_paintable(GTK_IMAGE(image)));
  screenshot(GTK_WIDGET(window), "/tmp/smashed-players.png");
  gtk_drop_down_set_selected(window->player_presence_filter, 1);
  g_assert_cmpuint(player_row_count(window->player_list), ==, 3);
  gtk_drop_down_set_selected(window->player_presence_filter, 2);
  g_assert_cmpuint(player_row_count(window->player_list), ==, 2);
  gtk_drop_down_set_selected(window->player_presence_filter, 0);
  gtk_editable_set_text(GTK_EDITABLE(window->player_search), "NothingMatches");
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_assert_cmpuint(player_row_count(window->player_list), ==, 0);
  gtk_editable_set_text(GTK_EDITABLE(window->player_search), "");
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_signal_emit_by_name(window->player_list, "row-activated", gtk_list_box_get_row_at_index(window->player_list, 0));
  spin(250);
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_nonnull(dialog);
  g_assert_cmpstr(adw_dialog_get_title(dialog), ==, "Player Details");
  GtkWidget *kick = find_button(GTK_WIDGET(dialog), "Kick");
  g_assert_nonnull(kick);
  g_assert_false(gtk_widget_get_sensitive(kick));
  g_assert_nonnull(find_button(GTK_WIDGET(dialog), "Revoke"));
  AdwActionRow *time_row = NULL;
  GList *queue = g_list_append(NULL, GTK_WIDGET(dialog));
  while (queue != NULL) {
    GtkWidget *widget = queue->data;
    queue = g_list_delete_link(queue, queue);
    if (ADW_IS_ACTION_ROW(widget) && g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), "Playtime") == 0)
      time_row = ADW_ACTION_ROW(widget);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL; child = gtk_widget_get_next_sibling(child))
      queue = g_list_append(queue, child);
  }
  g_assert_nonnull(time_row);
  g_autofree char *old_playtime = g_strdup(adw_action_row_get_subtitle(time_row));
  PlayerState *viewed = ensure_player_state(window,
    g_object_get_data(G_OBJECT(dialog), "player-uuid"), NULL, FALSE);
  g_assert_nonnull(viewed);
  viewed->session_started_mono -= 65 * G_USEC_PER_SEC;
  spin(1200);
  g_assert_cmpstr(adw_action_row_get_subtitle(time_row), !=, old_playtime);
  player_state_mark_offline(window, viewed);
  spin(1100);
  g_assert_false(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "player-online")) != 0);
  player_state_mark_online(window, viewed, viewed->platform);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-player-details.png");
  AdwViewStack *detail_pages = ADW_VIEW_STACK(find_widget(GTK_WIDGET(dialog), ADW_TYPE_VIEW_STACK));
  g_assert_nonnull(detail_pages);
  adw_view_stack_set_visible_child_name(detail_pages, "manage");
  screenshot(GTK_WIDGET(window), "/tmp/smashed-player-manage.png");
  graphene_rect_t dialog_bounds;
  g_assert_true(gtk_widget_compute_bounds(adw_dialog_get_child(dialog), GTK_WIDGET(window), &dialog_bounds));
  g_assert_false(pumpkin_window_dismiss_dialog_at(GTK_WINDOW(window),
    dialog_bounds.origin.x + dialog_bounds.size.width / 2,
    dialog_bounds.origin.y + dialog_bounds.size.height / 2));
  click_dialog_backdrop(window, 4, 4);
  spin(200);
  AdwStyleManager *style = adw_style_manager_get_default();
  AdwColorScheme previous_scheme = adw_style_manager_get_color_scheme(style);
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-players-dark.png");
  adw_style_manager_set_color_scheme(style, previous_scheme);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 580);
  spin(300);
  if (g_getenv("SMASHED_PRIVATE_X11_POINTER") == NULL)
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), ==, 800);
  else
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), >=, 780);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-players-compact.png");
  gtk_editable_set_text(GTK_EDITABLE(window->player_search), "NothingMatches");
  g_signal_emit_by_name(window->player_search, "search-changed");
  screenshot(GTK_WIDGET(window), "/tmp/smashed-players-empty.png");
  gtk_editable_set_text(GTK_EDITABLE(window->player_search), "");
  g_signal_emit_by_name(window->player_search, "search-changed");
  gtk_window_set_default_size(GTK_WINDOW(window), 1180, 860);
  adw_view_stack_set_visible_child_name(window->players_stack, "whitelisted");
  PlayerState *collision = ensure_player_state(window,
    "11111111-1111-4111-8111-111111111111", "Some Player", TRUE);
  g_autofree char *collision_key = g_strdup(collision->key);
  invalidate_player_list_signature(window);
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_assert_cmpuint(player_row_count(window->whitelist_list), ==, 1);
  remove_test_player(window, collision_key);
  g_assert_cmpstr(gtk_label_get_text(window->player_heading), ==, "Whitelist");
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window->player_presence_filter)));
  screenshot(GTK_WIDGET(window), "/tmp/smashed-players-whitelist.png");
  gtk_drop_down_set_selected(window->player_presence_filter, 2);
  g_assert_cmpuint(player_row_count(window->whitelist_list), ==, 0);
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window->btn_player_reset_filters)));
  g_signal_emit_by_name(window->btn_player_reset_filters, "clicked");
  g_assert_cmpuint(player_row_count(window->whitelist_list), ==, 1);
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->btn_player_reset_filters)));
  g_signal_emit_by_name(window->whitelist_list, "row-activated",
    gtk_list_box_get_row_at_index(window->whitelist_list, 0));
  spin(250);
  g_assert_nonnull(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  click_dialog_backdrop(window, 4, 4);
  spin(200);

  g_autofree char *account_bans = g_build_filename(data_dir, "banned-players.json", NULL);
  g_autofree char *address_bans = g_build_filename(data_dir, "banned-ips.json", NULL);
  g_assert_true(g_file_set_contents(account_bans,
    "[{\"uuid\":\"8667ba71-b85a-4004-af54-457a9734eed7\",\"name\":\"Alex\","
    "\"reason\":\"Misuse of \\\"chat\\\" {rule}\"}]", -1, NULL));
  g_assert_true(g_file_set_contents(address_bans,
    "[{\"ip\":\"192.0.2.10\",\"reason\":\"Repeated spam\"}]", -1, NULL));
  g_autoptr(GPtrArray) decoded_bans = load_player_entries_from_file(account_bans);
  g_assert_nonnull(decoded_bans);
  g_assert_cmpuint(decoded_bans->len, ==, 1);
  g_assert_cmpstr(((PlayerEntry *)g_ptr_array_index(decoded_bans, 0))->reason, ==, "Misuse of \"chat\" {rule}");
  adw_view_stack_set_visible_child_name(window->players_stack, "bans");
  collision = ensure_player_state(window,
    "11111111-1111-4111-8111-111111111111", "Alex", TRUE);
  g_clear_pointer(&collision_key, g_free);
  collision_key = g_strdup(collision->key);
  invalidate_player_list_signature(window);
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_assert_cmpuint(player_row_count(window->banned_list), ==, 1);
  remove_test_player(window, collision_key);
  g_assert_cmpuint(player_row_count(window->ip_bans_list), ==, 1);
  g_assert_cmpstr(gtk_label_get_text(window->player_summary), ==, "1 of 1 account bans  ·  1 of 1 IP bans");
  GtkWidget *unban = find_button(GTK_WIDGET(window->ip_bans_list), "Unban");
  g_assert_nonnull(unban);
  g_assert_false(gtk_widget_get_sensitive(unban));
  screenshot(GTK_WIDGET(window), "/tmp/smashed-players-bans.png");
  gtk_drop_down_set_selected(window->player_presence_filter, 1);
  g_assert_cmpuint(player_row_count(window->banned_list), ==, 0);
  g_assert_cmpuint(player_row_count(window->ip_bans_list), ==, 1);
  g_signal_emit_by_name(window->btn_player_reset_filters, "clicked");
  gtk_editable_set_text(GTK_EDITABLE(window->player_search), "chat");
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_assert_cmpuint(player_row_count(window->banned_list), ==, 1);
  g_assert_cmpuint(player_row_count(window->ip_bans_list), ==, 0);
  gtk_editable_set_text(GTK_EDITABLE(window->player_search), "spam");
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_assert_cmpuint(player_row_count(window->banned_list), ==, 0);
  g_assert_cmpuint(player_row_count(window->ip_bans_list), ==, 1);
  gtk_editable_set_text(GTK_EDITABLE(window->player_search), "NothingMatches");
  g_signal_emit_by_name(window->player_search, "search-changed");
  g_assert_cmpuint(player_row_count(window->banned_list), ==, 0);
  g_assert_cmpuint(player_row_count(window->ip_bans_list), ==, 0);
  g_signal_emit_by_name(window->btn_player_reset_filters, "clicked");
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(window->player_search)), ==, "");
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 580);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-players-bans-compact.png");
  gtk_window_set_default_size(GTK_WINDOW(window), 1180, 860);
  g_unlink(account_bans);
  g_unlink(address_bans);
  adw_view_stack_set_visible_child_name(window->players_stack, "all_players");
  g_autofree char *world_data = g_strdup_printf("%s/%s.dat", players_dir, offline_uuid);
  g_assert_true(g_file_set_contents(world_data, "world-data-must-stay", -1, NULL));
  for (guint attempt = 0; attempt < 2; attempt++) {
    GtkListBoxRow *alex_row = NULL;
    for (guint i = 0; i < player_row_count(window->player_list); i++) {
      GtkListBoxRow *candidate = gtk_list_box_get_row_at_index(window->player_list, (int)i);
      if (g_strcmp0(g_object_get_data(G_OBJECT(candidate), "player-name"), "Alex") == 0)
        alex_row = candidate;
    }
    g_assert_nonnull(alex_row);
    g_signal_emit_by_name(window->player_list, "row-activated", alex_row);
    spin(200);
    dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
    detail_pages = ADW_VIEW_STACK(find_widget(GTK_WIDGET(dialog), ADW_TYPE_VIEW_STACK));
    adw_view_stack_set_visible_child_name(detail_pages, "manage");
    GtkWidget *remove = find_button(GTK_WIDGET(dialog), "Remove");
    g_assert_true(gtk_widget_get_sensitive(remove));
    g_signal_emit_by_name(remove, "clicked");
    spin(250);
    AdwDialog *confirmation = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
    g_assert_true(ADW_IS_ALERT_DIALOG(confirmation));
    if (attempt == 0) {
      click_dialog_backdrop(window, 4, 4);
    } else {
      GtkWidget *response = find_button(GTK_WIDGET(confirmation), "Remove");
      g_assert_nonnull(response);
      g_signal_emit_by_name(response, "clicked");
    }
    spin(250);
    g_assert_true(g_file_test(world_data, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(g_hash_table_size(window->player_states), ==, attempt == 0 ? 5 : 4);
  }
  g_unlink(world_data);
  g_unlink(ops_path);
  g_unlink(whitelist_path);
  window->player_list_pointer_inside = FALSE;
  g_unlink(junk);
  g_print("PASS: player identity, edition evidence, complete snapshots, session reset, heads, status filters and compact layout\n");
  g_print("PASS: shared whitelist/ban search, escaped JSON reasons, filter reset, IP bans and outside-click cancellation\n");
}

typedef struct {
  GBytes *binary;
  SoupServerMessage *message;
  gboolean fail;
  gboolean invalid;
  guint requests;
} UpdateFixture;

static gboolean
send_update_response(gpointer data)
{
  UpdateFixture *fixture = data;
  SoupServerMessage *message = fixture->message;
  soup_server_message_set_status(message, fixture->fail ? 500 : 200, NULL);
  gsize size = 0;
  const char *bytes = g_bytes_get_data(fixture->binary, &size);
  soup_server_message_set_response(message, "application/octet-stream", SOUP_MEMORY_COPY,
                                   fixture->invalid ? "<html>not a server</html>" : bytes,
                                   fixture->invalid ? strlen("<html>not a server</html>") : size);
  soup_server_message_unpause(message);
  g_clear_object(&fixture->message);
  return G_SOURCE_REMOVE;
}

static void
serve_update(SoupServer *server, SoupServerMessage *message, const char *path,
              GHashTable *query, gpointer data)
{
  (void)server;
  (void)path;
  (void)query;
  UpdateFixture *fixture = data;
  fixture->requests++;
  fixture->message = g_object_ref(message);
  soup_server_message_pause(message);
  g_timeout_add(400, send_update_response, fixture);
}

static void
wait_for_test_server_exit(PumpkinServer *server)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (pumpkin_server_get_running(server) && g_get_monotonic_time() < deadline) {
    spin(10);
  }
  g_assert_false(pumpkin_server_get_running(server));
}

static void
capture_console_notice(PumpkinServer *server, const char *message, char **notice)
{
  (void)server;
  g_free(*notice);
  *notice = g_strdup(message);
}

static void
test_failed_restart_notice(PumpkinWindow *window, const char *binary, gsize size)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "Failed restart", &error);
  g_assert_no_error(error);
  g_assert_nonnull(server);
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_assert_true(g_file_set_contents(bin, binary, (gssize)size, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
  pumpkin_server_set_auto_restart_delay(server, 500);
  g_autofree char *notice = NULL;
  gulong notice_handler = g_signal_connect(server, "console-notice", G_CALLBACK(capture_console_notice), &notice);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  ensure_server_log_handler(window, server);
  g_setenv("SMASHED_TEST_SERVER_MODE", "normal", TRUE);
  g_assert_true(pumpkin_server_start(server, &error));
  g_assert_no_error(error);
  window->ui_state = UI_STATE_RUNNING;
  g_assert_true(pumpkin_server_send_command(server, "crash", &error));
  g_assert_no_error(error);
  wait_for_test_server_exit(server);
  g_assert_false(server_is_running_ui(window, server));
  g_assert_cmpint(g_remove(bin), ==, 0);
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (notice == NULL && g_get_monotonic_time() < deadline) spin(10);
  g_assert_nonnull(notice);
  g_assert_cmpstr(notice, !=, "");
  /* Inspect after a clock-second boundary: the assertion must not recreate
   * the receipt timestamp with a later call to the live formatter. */
  spin(1100);
  /* A failed automatic restart is an application notice, not evidence of a
   * running process. Parsing it as raw server output would revive the hint. */
  g_assert_false(pumpkin_server_get_running(server));
  g_assert_false(server_is_running_ui(window, server));
  g_assert_cmpint(window->ui_state, ==, UI_STATE_IDLE);
  GtkTextBuffer *buffer = g_hash_table_lookup(window->console_buffers, server);
  g_assert_nonnull(buffer);
  g_autofree char *text = buffer_text(buffer);
  g_autofree char *display = notice_display_payload(window, notice);
  g_assert_nonnull(strstr(text, display));
  g_assert_true(strstr(text, display) == g_strrstr(text, display));
  g_signal_handler_disconnect(server, notice_handler);
  pumpkin_server_set_auto_restart(server, FALSE);
  pumpkin_server_stop(server);
  g_print("PASS: automatic restart failures remain visible without reviving a stopped server\n");
}

static char *
write_recovery_cache(PumpkinWindow *window, const char *url, const char *identity,
                     const char *binary, gsize size)
{
  g_autofree char *key = identity != NULL ? g_strdup_printf("%s|%s", url, identity) : g_strdup(url);
  g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
  g_autofree char *dir = g_build_filename(pumpkin_config_get_base_dir(window->config),
                                         ".pumpkin", "versions", hash, NULL);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  char *path = g_build_filename(dir, "pumpkin", NULL);
  g_assert_true(g_file_set_contents(path, binary, (gssize)size, NULL));
  return path;
}

static void
test_cache_recovery_candidates(PumpkinWindow *window, const char *binary, gsize size)
{
  g_autofree char *previous_url = g_steal_pointer(&window->latest_url);
  g_autofree char *previous_id = g_steal_pointer(&window->latest_build_id);
  g_autofree char *previous_label = g_steal_pointer(&window->latest_build_label);
  g_autofree char *url_only_binary = g_memdup2(binary, size);
  url_only_binary[size - 1] = 'U';
  /* Missing URLs, missing/empty identities, invalid primary cache, latest
   * label identity, and the legacy URL-only fallback are independent cases. */
  for (guint mode = 0; mode < 8; ++mode) {
    g_autofree char *name = g_strdup_printf("Cache recovery %u", mode);
    g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, name, NULL);
    g_assert_nonnull(server);
    g_autofree char *bin = pumpkin_server_get_bin_path(server);
    g_autofree char *installed_url = g_strdup_printf("https://example.org/recovery/%u/installed", mode);
    g_autofree char *download_url = g_strdup_printf("https://example.org/recovery/%u/download", mode);
    g_autofree char *latest_url = g_strdup_printf("https://example.org/recovery/%u/latest", mode);
    const char *raw_label = mode == 3 || mode >= 6 ? NULL : "Benutzerdefinierter Build 2026-1-02";
    pumpkin_server_set_installed_url(server, mode >= 2 ? (mode == 4 ? "" : installed_url) : NULL);
    pumpkin_server_set_download_url(server, mode == 0 || mode == 5 ? download_url : NULL);
    pumpkin_server_set_installed_build_id(server, mode == 4 ? "" : NULL);
    pumpkin_server_set_installed_build_label(server, raw_label);
    g_clear_pointer(&window->latest_url, g_free);
    window->latest_url = mode == 1 || mode == 4 ? g_strdup(latest_url) : NULL;
    g_clear_pointer(&window->latest_build_id, g_free);
    window->latest_build_id = mode == 3 ? g_strdup("latest-identity-after-null-fields") : NULL;
    g_clear_pointer(&window->latest_build_label, g_free);
    window->latest_build_label = mode == 6 ? g_strdup("Opaque latest build label") : NULL;
    const char *source_url = mode == 0 || mode == 5 ? download_url :
                             mode == 1 || mode == 4 ? latest_url : installed_url;
    const char *identity = mode == 3 ? window->latest_build_id :
                           mode == 6 ? window->latest_build_label : raw_label;
    g_autofree char *specific = identity != NULL ?
      write_recovery_cache(window, source_url, identity, binary, size) : NULL;
    g_autofree char *url_only = write_recovery_cache(window, source_url, NULL,
                                                   identity != NULL ? url_only_binary : binary, size);
    g_autofree char *invalid = mode == 5 ?
      write_recovery_cache(window, installed_url, identity, "Invalid binary", strlen("Invalid binary")) : NULL;
    g_assert_false(g_file_test(bin, G_FILE_TEST_EXISTS));
    g_assert_true(server_has_installation(window, server));
    g_autofree char *restored = NULL;
    gsize restored_size = 0;
    g_assert_true(g_file_get_contents(bin, &restored, &restored_size, NULL));
    g_assert_cmpmem(restored, restored_size, binary, size);
    g_assert_cmpstr(pumpkin_server_get_installed_build_label(server), ==, raw_label);
    if (invalid != NULL) g_assert_false(g_file_test(invalid, G_FILE_TEST_EXISTS));
  }
  /* Repeated misses must only read the cache, without creating empty buckets. */
  g_autoptr(PumpkinServer) missing = pumpkin_server_store_add(window->store, "Missing cache", NULL);
  const char *miss_urls[] = {
    "https://example.org/missing-cache/installed",
    "https://example.org/missing-cache/download",
    "https://example.org/missing-cache/latest"
  };
  const char *miss_identities[] = { "installed-id", "installed-label", "latest-id", "latest-label", NULL };
  pumpkin_server_set_installed_url(missing, miss_urls[0]);
  pumpkin_server_set_download_url(missing, miss_urls[1]);
  pumpkin_server_set_installed_build_id(missing, miss_identities[0]);
  pumpkin_server_set_installed_build_label(missing, miss_identities[1]);
  g_clear_pointer(&window->latest_url, g_free);
  window->latest_url = g_strdup(miss_urls[2]);
  g_clear_pointer(&window->latest_build_id, g_free);
  window->latest_build_id = g_strdup(miss_identities[2]);
  g_clear_pointer(&window->latest_build_label, g_free);
  window->latest_build_label = g_strdup(miss_identities[3]);
  g_assert_false(server_has_installation(window, missing));
  g_assert_false(server_has_installation(window, missing));
  for (guint u = 0; u < G_N_ELEMENTS(miss_urls); ++u) {
    for (guint i = 0; i < G_N_ELEMENTS(miss_identities); ++i) {
      g_autofree char *key = miss_identities[i] != NULL ?
        g_strdup_printf("%s|%s", miss_urls[u], miss_identities[i]) : g_strdup(miss_urls[u]);
      g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
      g_autofree char *dir = g_build_filename(pumpkin_config_get_base_dir(window->config),
                                             ".pumpkin", "versions", hash, NULL);
      g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
    }
  }
  g_clear_pointer(&window->latest_url, g_free);
  window->latest_url = g_steal_pointer(&previous_url);
  g_clear_pointer(&window->latest_build_id, g_free);
  window->latest_build_id = g_steal_pointer(&previous_id);
  g_clear_pointer(&window->latest_build_label, g_free);
  window->latest_build_label = g_steal_pointer(&previous_label);
  g_print("PASS: cache recovery skips missing URLs and identities before the URL-only fallback\n");
}

static void
test_update_pipeline(PumpkinWindow *window)
{
  g_autofree char *executable = NULL;
  gsize executable_size = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_MANAGED_PUMPKIN_BINARY"),
                                   &executable, &executable_size, NULL));
  gsize size = MAX(executable_size + 1, (gsize)MIN_VALID_PUMPKIN_BINARY_BYTES + 1);
  g_autofree char *old_binary = g_malloc0(size);
  memcpy(old_binary, executable, executable_size);
  old_binary[size - 1] = '0';
  test_failed_restart_notice(window, old_binary, size);
  test_cache_recovery_candidates(window, old_binary, size);
  g_autofree char *new_binary = g_memdup2(old_binary, size);
  new_binary[size - 1] = '1';
  UpdateFixture fixture = { .binary = g_bytes_new(new_binary, size) };
  g_autoptr(SoupServer) http = soup_server_new(NULL, NULL);
  g_autoptr(GError) error = NULL;
  soup_server_add_handler(http, NULL, serve_update, &fixture, NULL);
  g_assert_true(soup_server_listen_local(http, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(http);
  g_autofree char *base_url = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

  /* Failure, direct/cache/shared downloads, manual stop and invalid payload. */
  for (guint mode = 0; mode < 7; mode++) {
    gboolean failed_download = mode == 0 || mode == 6;
    g_autofree char *name = g_strdup_printf("Maintenance test %u", mode);
    g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, name, &error);
    g_assert_no_error(error);
    g_assert_nonnull(server);
    g_autofree char *bin = pumpkin_server_get_bin_path(server);
    g_assert_true(g_file_set_contents(bin, old_binary, (gssize)size, NULL));
    g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
    pumpkin_server_set_installed_build_id(server, "old");
    pumpkin_server_set_installed_build_label(server, "Build 2020-01-02 03:04 UTC");
    g_autofree char *url = g_strdup_printf("%sdownload-%u", base_url, mode);
    g_autofree char *build = g_strdup_printf("new-%u", mode);
    pumpkin_window_select_server(window, pumpkin_server_get_id(server));
    g_free(window->latest_url);
    window->latest_url = g_strdup(url);
    g_free(window->latest_build_id);
    window->latest_build_id = g_strdup(build);
    g_free(window->latest_build_label);
    const char *raw_metadata = mode == 1 ? NULL : "Build 2026-10-12 03:04 UTC";
    window->latest_build_label = g_strdup(raw_metadata);
    pumpkin_config_set_date_format(window->config, (PumpkinDateFormat)(mode % 3));
    pumpkin_config_set_time_format(window->config, (PumpkinTimeFormat)(mode % 2));
    pumpkin_config_set_use_cache(window->config, mode == 2 || mode == 3 || mode == 5);
    fixture.fail = mode == 0;
    fixture.invalid = mode == 6;
    guint requests_before = fixture.requests;
    g_setenv("SMASHED_TEST_SERVER_MODE", "normal", TRUE);
    g_assert_true(pumpkin_server_start(server, &error));
    g_assert_no_error(error);
    window->ui_state = UI_STATE_RUNNING;
    pumpkin_window_update_server_actions(window, TRUE, TRUE, FALSE, TRUE);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->btn_details_install)));
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->btn_details_start)));
    g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(window->btn_details_update)));
    g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->btn_remove_server)));
    g_signal_emit_by_name(window->btn_remove_server, "clicked");
    g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    g_assert_true(g_file_test(bin, G_FILE_TEST_EXISTS));

    g_autofree char *cache = NULL;
    g_autofree char *lock = NULL;
    if (mode == 3 || mode == 5) {
      g_autofree char *key = g_strdup_printf("%s|%s", url, build);
      g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
      g_autofree char *dir = g_build_filename(pumpkin_config_get_base_dir(window->config),
                                             ".pumpkin", "versions", hash, NULL);
      g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
      cache = g_build_filename(dir, "pumpkin", NULL);
      lock = g_strconcat(cache, ".tmp", NULL);
      g_assert_true(g_file_set_contents(mode == 3 ? cache : lock,
                                        mode == 3 ? new_binary : "",
                                        mode == 3 ? (gssize)size : 0, NULL));
    }
    g_signal_emit_by_name(window->btn_details_update, "clicked");
    DownloadProgressState *state = get_download_progress_state(window, server, FALSE);
    g_assert_nonnull(state);
    g_assert_true(state->active);
    if (mode != 3) {
      spin(60);
      g_assert_true(pumpkin_server_get_running(server));
      g_assert_false(pumpkin_server_get_stopping(server));
    }
    if (mode == 4) {
      g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(window->btn_details_stop)));
      g_signal_emit_by_name(window->btn_details_stop, "clicked");
      wait_for_test_server_exit(server);
    }
    if (mode == 5) {
      spin(200);
      g_assert_false(pumpkin_server_get_stopping(server));
      g_assert_true(g_file_set_contents(cache, new_binary, (gssize)size, NULL));
      g_remove(lock);
    }
    gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (state->active && g_get_monotonic_time() < deadline) spin(10);
    g_assert_false(state->active);
    g_autofree char *installed = NULL;
    gsize installed_size = 0;
    g_assert_true(g_file_get_contents(bin, &installed, &installed_size, NULL));
    g_assert_cmpuint(installed_size, ==, size);
    g_assert_cmpmem(installed, installed_size, failed_download ? old_binary : new_binary, size);
    if (failed_download) {
      g_assert_true(pumpkin_server_get_running(server));
      g_assert_false(pumpkin_server_get_stopping(server));
      g_assert_cmpstr(pumpkin_server_get_installed_build_id(server), ==, "old");
      g_assert_cmpstr(pumpkin_server_get_installed_build_label(server), ==, "Build 2020-01-02 03:04 UTC");
    } else {
      g_assert_cmpstr(pumpkin_server_get_installed_build_id(server), ==, build);
      g_autofree char *fallback_metadata = NULL;
      if (raw_metadata == NULL) {
        GStatBuf stat_buf;
        g_assert_cmpint(g_stat(bin, &stat_buf), ==, 0);
        fallback_metadata = pumpkin_build_metadata_from_unix(stat_buf.st_mtime);
      }
      const char *expected_metadata = raw_metadata != NULL ? raw_metadata : fallback_metadata;
      g_assert_cmpstr(pumpkin_server_get_installed_build_label(server), ==, expected_metadata);
      g_autoptr(PumpkinServer) reloaded = pumpkin_server_load(pumpkin_server_get_root_dir(server), &error);
      g_assert_no_error(error);
      g_assert_nonnull(reloaded);
      g_assert_cmpstr(pumpkin_server_get_installed_build_label(reloaded), ==, expected_metadata);
      g_assert_true(mode == 4 ? !pumpkin_server_get_running(server) : pumpkin_server_get_running(server));
      g_autofree char *backup = g_strconcat(bin, ".previous", NULL);
      g_autofree char *previous = NULL;
      gsize previous_size = 0;
      g_assert_true(g_file_get_contents(backup, &previous, &previous_size, NULL));
      g_assert_cmpmem(previous, previous_size, old_binary, size);
      if (mode == 3 || mode == 5) g_assert_cmpuint(fixture.requests, ==, requests_before);
    }
    window->user_stop_requested = TRUE;
    pumpkin_server_stop(server);
    wait_for_test_server_exit(server);
    spin(50);
  }
  soup_server_disconnect(http);
  g_bytes_unref(fixture.binary);
  g_print("PASS: failed/invalid downloads leave server running; direct/cache/shared updates retain old binary and restart; manual stop wins\n");
}

static void
test_stop_confirmation(PumpkinWindow *window)
{
  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "Stubborn test server", NULL);
  /* Direct store insertion bypasses the UI's normal server creation wiring. */
  ensure_server_log_handler(window, server);
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_autofree char *executable = NULL;
  gsize executable_size = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_MANAGED_PUMPKIN_BINARY"),
                                   &executable, &executable_size, NULL));
  gsize size = MAX(executable_size, (gsize)MIN_VALID_PUMPKIN_BINARY_BYTES);
  g_autofree char *fixture = g_malloc0(size);
  memcpy(fixture, executable, executable_size);
  g_assert_true(g_file_set_contents(bin, fixture, (gssize)size, NULL));
  g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  g_assert_true(window->current == server);
  g_setenv("SMASHED_TEST_SERVER_MODE", "ignore-stop", TRUE);
  g_assert_true(pumpkin_server_start(server, NULL));
  window->ui_state = UI_STATE_RUNNING;
  g_signal_emit_by_name(window->btn_details_stop, "clicked");
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!pumpkin_server_get_stop_timed_out(server) && g_get_monotonic_time() < deadline) spin(10);
  g_assert_true(pumpkin_server_get_stop_timed_out(server));
  spin(200);
  g_assert_cmpstr(gtk_button_get_label(window->btn_details_stop), ==, "Force Stop...");
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(window->btn_details_stop)));
  g_signal_emit_by_name(window->btn_details_stop, "clicked");
  spin(100);
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  screenshot(GTK_WIDGET(window), "/tmp/smashed-force-stop.png");
  adw_dialog_close(dialog);
  spin(200);
  g_assert_true(pumpkin_server_get_running(server));
  g_signal_emit_by_name(window->btn_details_stop, "clicked");
  spin(100);
  dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  g_signal_emit_by_name(dialog, "response", "force");
  adw_dialog_close(dialog);
  wait_for_test_server_exit(server);
  g_setenv("SMASHED_TEST_SERVER_MODE", "normal", TRUE);
  g_print("PASS: shutdown timeout exposes Force Stop; cancelling retains server; only explicit confirmation terminates\n");
}

static void write_map_fixture(const char *directory, const char *nonce, double x)
{
  g_autofree char *live = g_build_filename(directory, "plugins", "data", "map-bridge", "live.json", NULL);
  g_autofree char *json = g_strdup_printf("{\"version\":1,\"request\":\"%s\",\"updated_at_ms\":%" G_GINT64_FORMAT
    ",\"worlds\":[{\"name\":\"Map Fixture\",\"dimension\":\"minecraft:overworld\","
    "\"loaded_chunks\":[{\"x\":0,\"z\":0}],\"border\":{\"x\":0,\"z\":0,\"diameter\":100}}],"
    "\"players\":[{\"id\":\"05c49c2f-e216-4216-8665-e7cb33009b51\",\"name\":\"Alex\","
    "\"world\":\"Map Fixture\",\"dimension\":\"minecraft:overworld\",\"x\":%g,\"y\":64,\"z\":8,\"yaw\":90,\"ping\":51}]}"
    , nonce, g_get_real_time() / 1000, x);
  g_assert_true(g_file_set_contents(live, json, -1, NULL));
}

static void test_map_moderation_reason(PumpkinWindow *window, GtkWidget *popover,
                                       const char *data, const char *nonce,
                                       const char *type, const char *input, const char *expected)
{
  g_print("Checking minimap %s with %s reason\n", type, *input != '\0' ? "supplied" : "empty");
  write_map_fixture(data, nonce, 25); spin(250);
  GtkWidget *canvas = gtk_widget_get_parent(popover);
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(canvas);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_GESTURE_CLICK(controller))
      g_signal_emit_by_name(controller, "released", 1, gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0);
  }
  spin(100);
  g_signal_emit_by_name(find_button(popover, g_strcmp0(type, "ban") == 0 ? "Ban..." : "Kick..."), "clicked");
  spin(100);
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "map-reason");
  g_assert_nonnull(entry);
  gtk_editable_set_text(GTK_EDITABLE(entry), input);
  g_signal_emit_by_name(find_button(GTK_WIDGET(dialog), g_strcmp0(type, "ban") == 0 ? "Ban Player" : "Kick Player"), "clicked");
  spin(350);
  g_autofree char *request_path = g_build_filename(data, "plugins", "data", "map-bridge", "request.json", NULL);
  g_autofree char *json = NULL;
  g_assert_true(g_file_get_contents(request_path, &json, NULL, NULL));
  cJSON *request = cJSON_Parse(json);
  cJSON *action = cJSON_GetObjectItemCaseSensitive(request, "action");
  g_assert_nonnull(action);
  g_assert_cmpstr(cJSON_GetObjectItemCaseSensitive(action, "type")->valuestring, ==, type);
  g_assert_cmpstr(cJSON_GetObjectItemCaseSensitive(action, "reason")->valuestring, ==, expected);
  /* Acknowledge the simulated action; never moderate a real player or server. */
  g_autofree char *live_path = g_build_filename(data, "plugins", "data", "map-bridge", "live.json", NULL);
  g_autofree char *live_json = NULL;
  g_assert_true(g_file_get_contents(live_path, &live_json, NULL, NULL));
  cJSON *live = cJSON_Parse(live_json);
  cJSON *result = cJSON_AddObjectToObject(live, "action_result");
  cJSON_AddStringToObject(result, "id", cJSON_GetObjectItemCaseSensitive(action, "id")->valuestring);
  cJSON_AddBoolToObject(result, "ok", TRUE);
  cJSON_AddStringToObject(result, "message", "Action completed");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(live, "updated_at_ms"), (double)(g_get_real_time() / 1000));
  char *reply = cJSON_PrintUnformatted(live);
  g_assert_true(g_file_set_contents(live_path, reply, -1, NULL));
  cJSON_free(reply); cJSON_Delete(live); cJSON_Delete(request);
  gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  gboolean acknowledged = FALSE;
  while (!acknowledged && g_get_monotonic_time() < deadline) {
    spin(20);
    g_clear_pointer(&json, g_free);
    g_assert_true(g_file_get_contents(request_path, &json, NULL, NULL));
    request = cJSON_Parse(json);
    acknowledged = cJSON_GetObjectItemCaseSensitive(request, "action") == NULL;
    cJSON_Delete(request);
  }
  g_assert_true(acknowledged);
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
}

static void test_worlds_map_access(PumpkinWindow *window)
{
  g_assert_false(pumpkin_config_get_minimap_enabled(window->config));
  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "World Access Fixture", NULL);
  g_autofree char *data = pumpkin_server_get_data_dir(server);
  g_autofree char *world = world_fixture(data, "Lobby");
  g_autofree char *config = g_build_filename(data, "pumpkin.toml", NULL);
  g_assert_true(g_file_set_contents(config, "tps = 20.0\ndefault_level_name = 'Lobby'\n", -1, NULL));
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  gtk_window_set_default_size(GTK_WINDOW(window), 1040, 720);
  gtk_window_present(GTK_WINDOW(window));
  adw_view_stack_set_visible_child_name(window->view_stack, "details");
  adw_view_stack_set_visible_child_name(window->details_stack, "worlds");
  refresh_world_list(window);
  gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  while (world_row(window, "Lobby") == NULL && g_get_monotonic_time() < deadline) spin(20);
  g_assert_nonnull(world_row(window, "Lobby"));
  g_assert_nonnull(find_label(GTK_WIDGET(window->details_sidebar), "Minimap"));
  GtkWidget *map = adw_view_stack_get_child_by_name(window->details_stack, "minimap");
  g_assert_nonnull(map);
  adw_view_stack_set_visible_child_name(window->details_stack, "minimap");
  spin(100);
  AdwDialog *consent = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(consent));
  GtkWidget *enable = find_button(map, "Enable Minimap");
  GtkWidget *canvas = find_widget(map, GTK_TYPE_DRAWING_AREA);
  g_assert_nonnull(enable); g_assert_nonnull(canvas);
  g_assert_true(gtk_widget_get_mapped(enable));
  g_assert_false(gtk_widget_get_mapped(canvas));
  g_assert_true(gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(gtk_widget_get_display(map)), "find-location-symbolic"));
  g_signal_emit_by_name(enable, "clicked");
  g_signal_emit_by_name(find_button(GTK_WIDGET(consent), "Agree and Enable"), "clicked");
  spin(100);
  g_assert_true(pumpkin_config_get_minimap_enabled(window->config));
  g_assert_true(gtk_widget_get_mapped(canvas));
  g_assert_false(gtk_widget_get_mapped(enable));
  PumpkinConfig *saved = pumpkin_config_load(NULL);
  g_assert_true(pumpkin_config_get_minimap_enabled(saved));
  pumpkin_config_free(saved);
  pumpkin_window_minimap_set_enabled(window, FALSE);
  adw_view_stack_set_visible_child_name(window->details_stack, "minimap");
  spin(600);
  g_assert_false(pumpkin_config_get_minimap_enabled(window->config));
  g_assert_true(gtk_widget_get_mapped(enable));
  g_assert_false(gtk_widget_get_mapped(canvas));
  g_assert_nonnull(find_label(GTK_WIDGET(window->details_sidebar), "Minimap"));
  g_autofree char *bridge = g_build_filename(data, "plugins", "data", "map-bridge", NULL);
  g_assert_false(g_file_test(bridge, G_FILE_TEST_EXISTS));
  g_assert_false(pumpkin_server_get_running(server));
  gtk_window_destroy(GTK_WINDOW(window)); spin(100);
  g_print("PASS: float-containing world configuration loads; Minimap stays discoverable, enables explicitly, persists and stays inactive when disabled\n");
}

static void test_minimap_only(PumpkinWindow *window)
{
  g_assert_false(pumpkin_config_get_minimap_enabled(window->config));
  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "Map Fixture", NULL);
  g_autofree char *data = pumpkin_server_get_data_dir(server);
  g_autofree char *region_dir = g_build_filename(data, "worlds", "Map Fixture", "region", NULL);
  g_assert_cmpint(g_mkdir_with_parents(region_dir, 0700), ==, 0);
  g_autofree char *region = g_build_filename(region_dir, "r.0.0.mca", NULL);
  guint8 chunks[12288] = {0}; chunks[2] = 2; chunks[3] = 1; chunks[4099] = 10;
  g_assert_true(g_file_set_contents(region, (const char *)chunks, sizeof(chunks), NULL));
  const char *sample = g_getenv("SMASHED_TERRAIN_SAMPLE");
  if (sample != NULL) {
    g_autofree char *source_path = g_build_filename(sample, "r.0.0.mca", NULL);
    g_autoptr(GFile) source = g_file_new_for_path(source_path), destination = g_file_new_for_path(region);
    g_assert_true(g_file_copy(source, destination, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL));
  }
  g_autofree char *bridge = g_build_filename(data, "plugins", "data", "map-bridge", NULL);
  g_assert_cmpint(g_mkdir_with_parents(bridge, 0700), ==, 0);
  g_autofree char *nether_dir = g_build_filename(data, "worlds", "Map Fixture", "DIM-1", "region", NULL);
  g_assert_cmpint(g_mkdir_with_parents(nether_dir, 0700), ==, 0);
  g_autofree char *plugins = pumpkin_server_get_plugins_dir(server);
  g_autofree char *bridge_binary = g_build_filename(plugins, PUMPKIN_MAP_BRIDGE_FILENAME, NULL);
  g_autoptr(GBytes) bundled = g_resources_lookup_data(PUMPKIN_MAP_BRIDGE_RESOURCE, 0, NULL);
  gsize bridge_size; const char *bridge_bytes = g_bytes_get_data(bundled, &bridge_size);
  const char *previous_sample = g_getenv("SMASHED_PREVIOUS_BRIDGE_SAMPLE");
  g_autofree char *previous_bytes = NULL;
  gsize previous_size = 0;
  if (previous_sample != NULL)
    g_assert_true(g_file_get_contents(previous_sample, &previous_bytes, &previous_size, NULL));
  g_assert_true(g_file_set_contents(bridge_binary, bridge_bytes, (gssize)bridge_size, NULL));
  if (previous_bytes != NULL)
    g_assert_true(g_file_set_contents(bridge_binary, previous_bytes, (gssize)previous_size, NULL));
  g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
    (const guint8 *)(previous_bytes != NULL ? previous_bytes : bridge_bytes),
    previous_bytes != NULL ? previous_size : bridge_size);
  g_autofree char *cache_path = g_build_filename(plugins, "permission_cache.json", NULL);
  g_autofree char *denied = g_strdup_printf("{\"entries\":{\"%s\":{\"permissions_requested\":[\"fs.write.data\"],\"approved\":false}}}", hash);
  g_assert_true(g_file_set_contents(cache_path, denied, -1, NULL));
  g_autofree char *ordinary_plugin = g_build_filename(plugins, "SamplePlugin.wasm", NULL);
  g_autofree char *legacy_disabled = g_build_filename(plugins, "smashed-map-bridge.wasm.disabled", NULL);
  g_assert_true(g_file_set_contents(ordinary_plugin, "fixture", -1, NULL));
  g_assert_true(g_file_set_contents(legacy_disabled, bridge_bytes, (gssize)bridge_size, NULL));
  if (sample == NULL) {
    g_autoptr(GPtrArray) saved = pumpkin_map_scan_regions(data, NULL, NULL);
    PumpkinMapWorld *world = g_ptr_array_index(saved, 0);
    PumpkinMapTile *tile = g_ptr_array_index(world->tiles, 0);
    cairo_surface_t *pixels = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
    cairo_t *cr = cairo_create(pixels); cairo_set_source_rgb(cr, .3, .6, .4); cairo_paint(cr); cairo_destroy(cr);
    pumpkin_map_cache_write(region_dir, 0, 0, tile->revisions[0], pixels); cairo_surface_destroy(pixels);
  }
  g_autofree char *binary = pumpkin_server_get_bin_path(server), *fixture = NULL;
  gsize bytes = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_MANAGED_PUMPKIN_BINARY"), &fixture, &bytes, NULL));
  if (bytes < MIN_VALID_PUMPKIN_BINARY_BYTES) {
    fixture = g_realloc(fixture, MIN_VALID_PUMPKIN_BINARY_BYTES);
    memset(fixture + bytes, 0, MIN_VALID_PUMPKIN_BINARY_BYTES - bytes);
    bytes = MIN_VALID_PUMPKIN_BINARY_BYTES;
  }
  g_assert_true(g_file_set_contents(binary, fixture, (gssize)bytes, NULL));
  g_assert_cmpint(g_chmod(binary, 0700), ==, 0);
  ensure_server_log_handler(window, server);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  g_assert_nonnull(gtk_list_box_get_row_at_index(window->plugin_list, 0));
  g_assert_null(gtk_list_box_get_row_at_index(window->plugin_list, 1));
  g_print("PASS: current and legacy internal bridges are hidden; ordinary plugins remain listed\n");
  g_assert_cmpint(g_remove(legacy_disabled), ==, 0);
  pumpkin_window_minimap_set_enabled(window, TRUE);
  gtk_window_set_default_size(GTK_WINDOW(window), 1040, 720);
  gtk_window_present(GTK_WINDOW(window));
  adw_view_stack_set_visible_child_name(window->view_stack, "details");
  adw_view_stack_set_visible_child_name(window->details_stack, "minimap");
  spin(200);
  AdwDialog *consent = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(consent));
  g_assert_false(pumpkin_config_get_minimap_consent(window->config));
  g_assert_false(pumpkin_config_get_minimap_enabled(window->config));
  g_signal_emit_by_name(find_button(GTK_WIDGET(consent), "Cancel"), "clicked"); spin(200);
  g_assert_false(pumpkin_config_get_minimap_consent(window->config));
  if (previous_bytes != NULL)
    g_assert_cmpint(pumpkin_map_bridge_access(data, bundled, NULL), ==, PUMPKIN_MAP_BRIDGE_OUTDATED);
  adw_view_stack_set_visible_child_name(window->details_stack, "minimap"); spin(150);
  consent = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(consent));
  g_signal_emit_by_name(find_button(GTK_WIDGET(consent), "Agree and Enable"), "clicked"); spin(650);
  g_assert_true(pumpkin_config_get_minimap_consent(window->config));
  g_assert_true(pumpkin_config_get_minimap_enabled(window->config));
  g_assert_cmpint(pumpkin_map_bridge_access(data, bundled, NULL), ==, PUMPKIN_MAP_BRIDGE_DENIED);
  GtkWidget *map = adw_view_stack_get_child_by_name(window->details_stack, "minimap");
  GtkWidget *approve = find_button(map, "Allow Data Access...");
  g_assert_nonnull(approve); g_assert_true(gtk_widget_get_visible(approve));
  g_assert_true(gtk_widget_get_sensitive(approve));
  g_signal_emit_by_name(approve, "clicked"); spin(150);
  AdwDialog *permission_dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(permission_dialog));
  g_signal_emit_by_name(find_button(GTK_WIDGET(permission_dialog), "Cancel"), "clicked"); spin(200);
  g_assert_cmpint(pumpkin_map_bridge_access(data, bundled, NULL), ==, PUMPKIN_MAP_BRIDGE_DENIED);
  g_signal_emit_by_name(approve, "clicked"); spin(150);
  permission_dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(permission_dialog));
  g_signal_emit_by_name(find_button(GTK_WIDGET(permission_dialog), "Allow Data Access"), "clicked"); spin(350);
  gint64 approval_deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  while (pumpkin_server_get_maintenance(server) && g_get_monotonic_time() < approval_deadline) spin(20);
  g_assert_false(pumpkin_server_get_maintenance(server));
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_cmpint(pumpkin_map_bridge_access(data, bundled, NULL), ==, PUMPKIN_MAP_BRIDGE_APPROVED);
  pumpkin_window_minimap_update(window);
  GtkWidget *banner = find_widget(map, ADW_TYPE_BANNER);
  g_assert_nonnull(banner);
  g_assert_false(adw_banner_get_revealed(ADW_BANNER(banner)));
  /* Restore an old artifact only in this private stopped fixture. Starting must
   * upgrade it through the common hook without overriding the new hash's approval. */
  if (previous_bytes != NULL)
    g_assert_true(g_file_set_contents(bridge_binary, previous_bytes, (gssize)previous_size, NULL));
  g_autoptr(GError) start_error = NULL;
  gboolean started = pumpkin_server_start(server, &start_error);
  g_assert_no_error(start_error); g_assert_true(started);
  g_assert_cmpint(pumpkin_map_bridge_access(data, bundled, NULL), ==, PUMPKIN_MAP_BRIDGE_APPROVED);
  if (previous_bytes != NULL) g_print("PASS: automatic stopped update and common start hook preserve explicit decisions\n");
  g_autofree char *request_path = g_build_filename(bridge, "request.json", NULL);
  gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  while (!g_file_test(request_path, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) spin(20);
  g_autofree char *request = NULL;
  g_assert_true(g_file_get_contents(request_path, &request, NULL, NULL));
  cJSON *request_doc = cJSON_Parse(request);
  g_assert_nonnull(request_doc);
  g_autofree char *nonce = g_strdup(cJSON_GetObjectItem(request_doc, "request")->valuestring);
  cJSON_Delete(request_doc);
  write_map_fixture(data, nonce, 8); spin(650);
  GtkWidget *canvas = find_widget(map, GTK_TYPE_DRAWING_AREA);
  g_assert_nonnull(find_label(GTK_WIDGET(window->details_sidebar), "Minimap"));
  const char *sidebar_titles[] = {"Console", "Stats", "Plugins", "Players", "Minimap", "Worlds", "Logs", "Settings"};
  for (guint i = 0; i < G_N_ELEMENTS(sidebar_titles); i++) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(window->details_sidebar, (int)i);
    g_assert_nonnull(find_label(GTK_WIDGET(row), sidebar_titles[i]));
  }
  graphene_rect_t canvas_bounds, sidebar_bounds, content_bounds;
  GtkWidget *details = GTK_WIDGET(window->details_page_box);
  GtkWidget *sidebar_shell = gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(window->details_sidebar)));
  while (!gtk_widget_has_css_class(sidebar_shell, "details-navigation-shell"))
    sidebar_shell = gtk_widget_get_parent(sidebar_shell);
  g_assert_true(gtk_widget_compute_bounds(canvas, details, &canvas_bounds));
  g_assert_true(gtk_widget_compute_bounds(sidebar_shell, details, &sidebar_bounds));
  g_assert_true(gtk_widget_compute_bounds(GTK_WIDGET(window->details_stack), details, &content_bounds));
  g_assert_cmpfloat_with_epsilon(canvas_bounds.origin.x, content_bounds.origin.x, 1);
  g_assert_cmpfloat_with_epsilon(canvas_bounds.size.width, content_bounds.size.width, 1);
  g_assert_cmpfloat_with_epsilon(canvas_bounds.origin.x - sidebar_bounds.origin.x - sidebar_bounds.size.width,
    gtk_widget_get_margin_end(details), 1);
  g_assert_cmpfloat_with_epsilon(canvas_bounds.origin.y + canvas_bounds.size.height,
    sidebar_bounds.origin.y + sidebar_bounds.size.height, 1);
  g_assert_cmpfloat_with_epsilon(canvas_bounds.origin.y + canvas_bounds.size.height,
    gtk_widget_get_height(details), 1);
  g_assert_false(gtk_widget_get_visible(find_label(map, "Live / 1 online")));
  GtkWidget *world_choice = find_widget(map, GTK_TYPE_DROP_DOWN);
  g_assert_cmpuint(g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(world_choice))), ==, 1);
  g_assert_false(gtk_widget_get_visible(world_choice));
  g_assert_nonnull(find_button(map, "Overworld"));
  /* A newly saved dimension is discovered by its existing empty-folder watch. */
  g_autofree char *nether_region = g_build_filename(nether_dir, "r.0.0.mca", NULL);
  gint64 saved_at = g_get_monotonic_time();
  g_assert_true(g_file_set_contents(nether_region, (const char *)chunks, sizeof(chunks), NULL));
  while (g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(world_choice))) != 2 &&
         g_get_monotonic_time() - saved_at < 1500000) spin(20);
  g_assert_cmpuint(g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(world_choice))), ==, 2);
  g_assert_nonnull(find_button(map, "Nether"));
  g_print("PASS: new saved chunks/dimension discovered in %.0f ms\n", (g_get_monotonic_time() - saved_at) / 1000.0);
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(canvas);
  GtkEventController *click = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_GESTURE_CLICK(controller)) { click = controller; break; }
    g_object_unref(controller);
  }
  g_assert_nonnull(click);
  write_map_fixture(data, nonce, 8); spin(200);
  g_signal_emit_by_name(click, "released", 1, gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0);
  g_object_unref(click); spin(150);
  GtkWidget *popover = find_widget(canvas, GTK_TYPE_POPOVER);
  g_assert_nonnull(popover); g_assert_true(gtk_widget_get_visible(popover));
  g_assert_nonnull(find_label(popover, "Alex"));
  g_assert_nonnull(find_label(popover, "51 ms ping\nX 8.0   Y 64.0   Z 8.0"));
  g_signal_emit_by_name(find_button(popover, "Follow Player"), "clicked");
  write_map_fixture(data, nonce, 24); spin(650);
  GtkWidget *stop = find_tooltip(map, "Stop following player");
  g_assert_true(gtk_widget_get_visible(stop));
  g_assert_nonnull(find_label(popover, "51 ms ping\nX 24.0   Y 64.0   Z 8.0"));
  GtkWidget *rate_title = find_label(GTK_WIDGET(window), "Player update rate");
  g_assert_nonnull(rate_title);
  GtkWidget *rate_row = gtk_widget_get_ancestor(rate_title, ADW_TYPE_ACTION_ROW);
  g_assert_true(gtk_widget_get_ancestor(rate_row, ADW_TYPE_PREFERENCES_GROUP) == GTK_WIDGET(window->minimap_settings_group));
  g_assert_cmpstr(adw_preferences_group_get_title(window->minimap_settings_group), ==, "Minimap");
  GtkWidget *rate = find_widget(rate_row, GTK_TYPE_SCALE);
  g_assert_nonnull(rate); gtk_range_set_value(GTK_RANGE(rate), 20); spin(120);
  g_assert_cmpuint(pumpkin_config_get_minimap_refresh_hz(window->config), ==, 20);
  g_clear_pointer(&request, g_free); g_assert_true(g_file_get_contents(request_path, &request, NULL, NULL));
  request_doc = cJSON_Parse(request);
  g_assert_cmpint(cJSON_GetObjectItem(request_doc, "refresh_hz")->valueint, ==, 20); cJSON_Delete(request_doc);
  write_map_fixture(data, nonce, 25); spin(120);
  g_assert_nonnull(find_label(popover, "51 ms ping\nX 25.0   Y 64.0   Z 8.0"));
  screenshot(GTK_WIDGET(window), "/tmp/smashed-minimap.png");
  test_map_moderation_reason(window, popover, data, nonce, "kick", "", "");
  test_map_moderation_reason(window, popover, data, nonce, "ban", "   ", "");
  test_map_moderation_reason(window, popover, data, nonce, "kick", "  Repeated spam  ", "Repeated spam");
  g_print("PASS: aligned map/sidebar edges, navigation order and optional kick/ban reasons without branding\n");
  /* Four choices stay on the map; a fifth switches to the compact dropdown. */
  const char *extra[] = {"worlds/Map Fixture/DIM1/region", "worlds/Lobby/region", "worlds/BedWars 1/region"};
  for (guint i = 0; i < G_N_ELEMENTS(extra); i++) {
    g_autofree char *directory = g_build_filename(data, extra[i], NULL);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    g_autofree char *file = g_build_filename(directory, "r.0.0.mca", NULL);
    g_assert_true(g_file_set_contents(file, (const char *)chunks, sizeof(chunks), NULL));
    gint64 until = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(world_choice))) != i + 3 &&
           g_get_monotonic_time() < until) spin(20);
    g_assert_cmpuint(g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(world_choice))), ==, i + 3);
    g_assert_cmpint(gtk_widget_get_visible(world_choice), ==, i == 2);
  }
  g_assert_cmpint(gtk_widget_get_width(world_choice), <, gtk_widget_get_width(canvas) / 2);
  pumpkin_window_minimap_set_enabled(window, FALSE);
  g_autofree char *parked = g_strconcat(bridge_binary, ".disabled", NULL);
  g_autofree char *unloaded = g_build_filename(data, "bridge-unloaded-marker", NULL);
  deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  while (!g_file_test(unloaded, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) spin(20);
  g_assert_true(g_file_test(unloaded, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(bridge_binary, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(parked, G_FILE_TEST_EXISTS));
  g_clear_pointer(&request, g_free); gsize request_size = 0;
  g_assert_true(g_file_get_contents(request_path, &request, &request_size, NULL));
  g_assert_cmpuint(request_size, ==, 0);
  spin(200);
  g_clear_pointer(&request, g_free);
  g_assert_true(g_file_get_contents(request_path, &request, &request_size, NULL));
  g_assert_cmpuint(request_size, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(world_choice))), ==, 1);
  g_assert_false(gtk_widget_get_sensitive(rate));
  g_assert_nonnull(find_button(map, "Enable Minimap"));
  g_assert_false(pumpkin_config_get_minimap_enabled(window->config));
  pumpkin_window_minimap_set_enabled(window, TRUE); spin(400);
  g_autofree char *loaded = g_build_filename(data, "bridge-loaded-marker", NULL);
  g_assert_true(g_file_test(loaded, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(bridge_binary, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(parked, G_FILE_TEST_EXISTS));
  g_clear_pointer(&request, g_free);
  g_assert_true(g_file_get_contents(request_path, &request, NULL, NULL));
  request_doc = cJSON_Parse(request); g_assert_nonnull(request_doc);
  g_assert_cmpstr(cJSON_GetObjectItem(request_doc, "request")->valuestring, !=, nonce);
  cJSON_Delete(request_doc);
  g_print("PASS: minimap settings group, no stopped-server hint, bridge unload/reload and revoked polling lease\n");
  g_assert_true(pumpkin_config_get_minimap_enabled(window->config));
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  PumpkinConfig *saved_settings = pumpkin_config_load(NULL);
  g_assert_true(pumpkin_config_get_minimap_consent(saved_settings));
  g_assert_cmpuint(pumpkin_config_get_minimap_refresh_hz(saved_settings), ==, 20);
  pumpkin_config_free(saved_settings);
  if (sample == NULL) {
    GtkWidget *terrain_status = find_label(map, "Terrain / 1 chunk");
    g_assert_nonnull(terrain_status);
    g_assert_false(gtk_widget_get_visible(terrain_status));
  }
  pumpkin_server_stop(server); wait_for_test_server_exit(server);
  gtk_window_destroy(GTK_WINDOW(window)); spin(100);
  g_print("PASS: explicit/cancelled bridge approval, fast discovery, terrain retention, nonce handshake, player selection, tracking and cleanup\n");
}

static void test_release_channels_only(PumpkinWindow *window)
{
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_release_channel), ==, PUMPKIN_RELEASE_NIGHTLY);
  g_assert_cmpint(pumpkin_config_get_release_channel(window->config), ==, PUMPKIN_RELEASE_NIGHTLY);
  g_autoptr(PumpkinServer) managed = pumpkin_server_store_add(window->store, "Release test", NULL);
  g_autoptr(PumpkinServer) custom = pumpkin_server_store_add(window->store, "Custom source test", NULL);
  pumpkin_server_set_download_url(custom, "https://example.org/custom-pumpkin");
  pumpkin_server_save(custom, NULL);
  window->latest_url = g_strdup("https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/obsolete");
  window->latest_build_id = g_strdup("obsolete-build");
  window->latest_build_label = g_strdup("obsolete-label");
  guint generation = window->latest_resolve_generation;
  adw_combo_row_set_selected(window->row_release_channel, PUMPKIN_RELEASE_STABLE);
  g_assert_cmpuint(window->latest_resolve_generation, >, generation);
  g_assert_null(window->latest_url);
  g_assert_null(window->latest_build_id);
  g_assert_null(window->latest_build_label);
  g_autofree char *stable = pumpkin_release_default_url(PUMPKIN_RELEASE_STABLE);
  g_assert_cmpstr(pumpkin_server_get_download_url(managed), ==, stable);
  g_assert_cmpstr(pumpkin_server_get_download_url(custom), ==, "https://example.org/custom-pumpkin");
  g_assert_cmpstr(pumpkin_config_get_default_download_url(window->config), ==, stable);
  PumpkinConfig *saved = pumpkin_config_load(NULL);
  g_assert_cmpint(pumpkin_config_get_release_channel(saved), ==, PUMPKIN_RELEASE_STABLE);
  pumpkin_config_free(saved);

  /* Cancelled Nightly callbacks must not overwrite a later Stable request. */
  adw_combo_row_set_selected(window->row_release_channel, PUMPKIN_RELEASE_NIGHTLY);
  adw_combo_row_set_selected(window->row_release_channel, PUMPKIN_RELEASE_STABLE);
  GCancellable *active_request = window->latest_resolve_cancellable;
  spin(100);
  if (window->latest_resolve_in_flight) {
    g_assert_true(window->latest_resolve_cancellable == active_request);
  } else {
    g_assert_true(window->latest_error != NULL ||
      (window->latest_build_label != NULL && g_str_has_prefix(window->latest_build_label, "Stable ")));
  }
  g_assert_cmpint(pumpkin_config_get_release_channel(window->config), ==, PUMPKIN_RELEASE_STABLE);
  g_assert_cmpstr(pumpkin_server_get_download_url(managed), ==, stable);
  g_assert_cmpstr(pumpkin_server_get_download_url(custom), ==, "https://example.org/custom-pumpkin");

  /* A channel change cannot interfere with an automatic update countdown. */
  window->auto_update_server = g_object_ref(managed);
  adw_combo_row_set_selected(window->row_release_channel, PUMPKIN_RELEASE_NIGHTLY);
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_release_channel), ==, PUMPKIN_RELEASE_STABLE);
  g_assert_cmpint(pumpkin_config_get_release_channel(window->config), ==, PUMPKIN_RELEASE_STABLE);
  g_clear_object(&window->auto_update_server);

  pumpkin_config_set_default_download_url(window->config, "https://example.org/custom-default");
  gtk_editable_set_text(GTK_EDITABLE(window->entry_download_url), "https://example.org/custom-default");
  adw_combo_row_set_selected(window->row_release_channel, PUMPKIN_RELEASE_NIGHTLY);
  g_assert_cmpstr(pumpkin_config_get_default_download_url(window->config), ==, "https://example.org/custom-default");
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(window->entry_download_url)), ==, "https://example.org/custom-default");

  GtkWidget *refresh = gtk_button_new_with_label("Up to date");
  g_object_ref_sink(refresh);
  pumpkin_refresh_button_setup(GTK_BUTTON(refresh));
  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(refresh));
  pumpkin_refresh_button_set_label(GTK_BUTTON(refresh), "Check failed");
  g_assert_true(child == gtk_button_get_child(GTK_BUTTON(refresh)));
  g_assert_nonnull(find_label(refresh, "Check failed"));
  g_assert_true(pumpkin_refresh_button_begin(GTK_BUTTON(refresh)));
  pumpkin_refresh_button_set_label(GTK_BUTTON(refresh), "Checking...");
  g_assert_true(child == gtk_button_get_child(GTK_BUTTON(refresh)));
  g_assert_false(gtk_widget_get_can_target(refresh));
  g_object_unref(refresh);

  gtk_window_destroy(GTK_WINDOW(window));
  spin(650);
  g_print("PASS: Nightly default, persisted Stable selection, request cancellation, busy guard, custom URLs and animated badge labels\n");
}

static GtkWidget *find_css_class(GtkWidget *root, const char *css_class)
{
  if (gtk_widget_has_css_class(root, css_class)) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_css_class(child, css_class);
    if (found != NULL) return found;
  }
  return NULL;
}

static GtkWidget *overview_row_for(PumpkinWindow *window, PumpkinServer *server)
{
  queue_overview_refresh(window, TRUE);
  spin(30);
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(window->overview_list)); row != NULL;
       row = gtk_widget_get_next_sibling(row)) {
    if (g_object_get_data(G_OBJECT(row), "server") == server) return row;
  }
  g_error("Missing overview row for the test server");
}

static void test_i18n_boundaries_only(PumpkinWindow *window)
{
  /* The update regressions use a local HTTP fixture, not a live release response. */
  ++window->latest_resolve_generation;
  if (window->latest_resolve_cancellable != NULL) g_cancellable_cancel(window->latest_resolve_cancellable);
  g_clear_object(&window->latest_resolve_cancellable);
  window->latest_resolve_in_flight = FALSE;
  if (window->latest_poll_id != 0) {
    g_source_remove(window->latest_poll_id);
    window->latest_poll_id = 0;
  }
  g_clear_pointer(&window->latest_error, g_free);
  g_clear_pointer(&window->latest_url, g_free);
  window->latest_url = g_strdup("https://example.org/test-managed-build");
  g_clear_pointer(&window->latest_build_id, g_free);
  g_clear_pointer(&window->latest_build_label, g_free);
  window->latest_build_label = g_strdup("Build 2026-10-12 03:04 UTC");

  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "I18n boundaries", NULL);
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_autofree char *executable = NULL;
  gsize executable_size = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_MANAGED_PUMPKIN_BINARY"),
                                   &executable, &executable_size, NULL));
  gsize size = MAX(executable_size, (gsize)MIN_VALID_PUMPKIN_BINARY_BYTES);
  g_autofree char *binary = g_malloc0(size);
  memcpy(binary, executable, executable_size);
  g_assert_true(g_file_set_contents(bin, binary, (gssize)size, NULL));
  g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  test_console_polling(window, server);

  /* These opaque stored labels deliberately collide with UI status wording.
   * Installation must depend on the binary, not English or translated text. */
  const char *status_labels[] = {"Not installed", "Nicht installiert"};
  for (guint i = 0; i < G_N_ELEMENTS(status_labels); ++i) {
    pumpkin_server_set_installed_build_label(server, status_labels[i]);
    GtkWidget *row = overview_row_for(window, server);
    g_assert_nonnull(find_css_class(row, "update-button"));
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->btn_details_install)));
  }

  /* Optional legacy Build prefix is metadata grammar, not translated output.
   * The same build remains current for every date/time presentation setting. */
  pumpkin_server_set_installed_build_label(server, "Build 2026-10-12 03:04 UTC");
  g_free(window->latest_build_label);
  window->latest_build_label = g_strdup("2026-10-12 03:04 UTC");
  for (guint date = 0; date < 3; ++date) {
    for (guint time = 0; time < 2; ++time) {
      pumpkin_config_set_date_format(window->config, (PumpkinDateFormat)date);
      pumpkin_config_set_time_format(window->config, (PumpkinTimeFormat)time);
      g_assert_null(find_css_class(overview_row_for(window, server), "update-button"));
      g_assert_cmpstr(pumpkin_server_get_installed_build_label(server), ==, "Build 2026-10-12 03:04 UTC");
    }
  }
  pumpkin_server_set_installed_build_id(server, "old-build");
  window->latest_build_id = g_strdup("new-build");
  g_assert_nonnull(find_css_class(overview_row_for(window, server), "update-button"));
  g_clear_pointer(&window->latest_build_id, g_free);
  g_free(window->latest_build_label);
  window->latest_build_label = g_strdup("Build 2026-10-12 03:05 UTC");
  g_assert_nonnull(find_css_class(overview_row_for(window, server), "update-button"));

  /* Metadata alone cannot make a missing binary installed. */
  g_assert_cmpint(g_remove(bin), ==, 0);
  g_assert_null(find_css_class(overview_row_for(window, server), "update-button"));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window->btn_details_install)));
  g_print("PASS: installation is independent of status wording; raw metadata and build IDs drive update decisions\n");

  test_update_pipeline(window);
  gtk_window_destroy(GTK_WINDOW(window));
  spin(650);
  g_print("PASS: i18n boundary regressions use stable widget identities and retain raw persisted build metadata\n");
}

static GtkWidget *find_label_containing(GtkWidget *root, const char *text)
{
  if (GTK_IS_LABEL(root) && strstr(gtk_label_get_text(GTK_LABEL(root)), text) != NULL) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_label_containing(child, text);
    if (found != NULL) return found;
  }
  return NULL;
}

static void test_i18n_loading_only(PumpkinWindow *window)
{
  /* Keep this test independent of remote release metadata and actual servers. */
  ++window->latest_resolve_generation;
  if (window->latest_resolve_cancellable != NULL) g_cancellable_cancel(window->latest_resolve_cancellable);
  g_clear_object(&window->latest_resolve_cancellable);
  window->latest_resolve_in_flight = FALSE;
  if (window->latest_poll_id != 0) {
    g_source_remove(window->latest_poll_id);
    window->latest_poll_id = 0;
  }
  g_clear_pointer(&window->latest_error, g_free);
  g_clear_pointer(&window->latest_url, g_free);
  window->latest_url = g_strdup("https://example.org/test-managed-build");
  g_clear_pointer(&window->latest_build_id, g_free);
  g_clear_pointer(&window->latest_build_label, g_free);
  window->latest_build_label = g_strdup("Build 2026-10-12 03:04 UTC");

  /* Look up widgets and pages by stable identity, then assert their presentation. */
  GtkWidget *overview = adw_view_stack_get_child_by_name(window->view_stack, "overview");
  g_assert_nonnull(overview);
  AdwViewStackPage *page = adw_view_stack_get_page(window->view_stack, overview);
  g_assert_cmpstr(adw_view_stack_page_get_title(page), ==, "Übersicht");
  g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(window)), ==, "Smashed Pumpkin");
  g_assert_cmpstr(gtk_button_get_label(window->btn_details_install), ==, "Installieren");
  g_assert_cmpstr(setlocale(LC_NUMERIC, NULL), ==, "C");
  g_assert_cmpstr(localeconv()->decimal_point, ==, ".");

  pumpkin_config_set_detailed_overview_cards(window->config, TRUE);
  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "I18n loading", NULL);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  g_assert_nonnull(find_label_containing(overview_row_for(window, server), "Nicht installiert"));
  pumpkin_window_update_server_actions(window, FALSE, FALSE, FALSE, FALSE);
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window->btn_details_install)));
  g_assert_cmpstr(gtk_button_get_label(window->btn_details_stop), ==, "Anhalten");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(window->btn_details_stop)), ==,
                 "Daten speichern und den Server anhalten");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(window->btn_details_update)), ==,
                 "Den installierten Server aktualisieren");

  /* A padded fake binary supplies installation state without running a process. */
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_autofree char *executable = NULL;
  gsize executable_size = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_MANAGED_PUMPKIN_BINARY"),
                                   &executable, &executable_size, NULL));
  gsize size = MAX(executable_size, (gsize)MIN_VALID_PUMPKIN_BINARY_BYTES);
  g_autofree char *binary = g_malloc0(size);
  memcpy(binary, executable, executable_size);
  g_assert_true(g_file_set_contents(bin, binary, (gssize)size, NULL));
  g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
  pumpkin_server_set_installed_build_label(server, "Build 2026-10-12 03:04 UTC");
  pumpkin_config_set_date_format(window->config, PUMPKIN_DATE_FORMAT_YMD);
  pumpkin_config_set_time_format(window->config, PUMPKIN_TIME_FORMAT_24H);
  GtkWidget *row = overview_row_for(window, server);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(window->row_language)), ==, "Sprache");
  g_assert_cmpstr(gtk_string_list_get_string(GTK_STRING_LIST(adw_combo_row_get_model(window->row_language)), 0), ==, "Systemsprache");
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(window->row_language_restart)), ==, "Sprachänderung gespeichert");
  g_assert_nonnull(find_label_containing(row, "Erstellt 2026-10-12 03:04"));
  g_assert_null(find_css_class(row, "update-button"));
  g_assert_cmpstr(pumpkin_server_get_installed_build_label(server), ==, "Build 2026-10-12 03:04 UTC");
  g_assert_cmpstr(pumpkin_server_get_name(server), ==, "I18n loading");
  pumpkin_server_set_installed_build_id(server, "old-build");
  window->latest_build_id = g_strdup("new-build");
  g_assert_nonnull(find_css_class(overview_row_for(window, server), "update-button"));
  g_assert_false(pumpkin_server_get_running(server));

  gtk_window_destroy(GTK_WINDOW(window));
  spin(650);
  g_print("PASS: GTK template and dynamic C translations load; raw build identities, branding and numeric wire formats stay stable\n");
}

static void test_i18n_chinese_only(PumpkinWindow *window)
{
  /* This exercises the installed-catalog discovery path, not the test fixture. */
  ++window->latest_resolve_generation;
  if (window->latest_resolve_cancellable != NULL) g_cancellable_cancel(window->latest_resolve_cancellable);
  g_clear_object(&window->latest_resolve_cancellable);
  window->latest_resolve_in_flight = FALSE;
  if (window->latest_poll_id != 0) {
    g_source_remove(window->latest_poll_id);
    window->latest_poll_id = 0;
  }
  g_clear_pointer(&window->latest_error, g_free);
  g_clear_pointer(&window->latest_url, g_free);
  g_clear_pointer(&window->latest_build_id, g_free);
  g_clear_pointer(&window->latest_build_label, g_free);
  window->latest_url = g_strdup("https://example.org/test-managed-build");
  window->latest_build_label = g_strdup("Build 2026-10-12 03:04 UTC");

  g_assert_true(pumpkin_language_is_valid("zh_CN"));
  g_assert_cmpstr(pumpkin_language_name(pumpkin_language_index("zh_CN")), ==, "简体中文");
  g_assert_cmpstr(pumpkin_config_get_language(window->config), ==, "zh_CN");
  g_assert_cmpstr(pumpkin_i18n_get_language(), ==, "zh_CN");
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_language), ==, pumpkin_language_index("zh_CN"));

  GtkWidget *overview = adw_view_stack_get_child_by_name(window->view_stack, "overview");
  g_assert_nonnull(overview);
  AdwViewStackPage *page = adw_view_stack_get_page(window->view_stack, overview);
  g_assert_cmpstr(adw_view_stack_page_get_title(page), ==, "总览");
  g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(window)), ==, "Smashed Pumpkin");
  g_assert_cmpstr(gtk_button_get_label(window->btn_details_install), ==, "安装");
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(window->row_language)), ==, "语言");
  g_assert_cmpstr(gtk_string_list_get_string(GTK_STRING_LIST(adw_combo_row_get_model(window->row_language)),
                  pumpkin_language_index("system")), ==, "跟随系统");
  g_assert_cmpstr(setlocale(LC_NUMERIC, NULL), ==, "C");
  g_assert_cmpstr(localeconv()->decimal_point, ==, ".");

  pumpkin_config_set_detailed_overview_cards(window->config, TRUE);
  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "I18n loading", NULL);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  g_assert_nonnull(find_label_containing(overview_row_for(window, server), "尚未安装"));
  pumpkin_window_update_server_actions(window, FALSE, FALSE, FALSE, FALSE);
  g_assert_cmpstr(gtk_button_get_label(window->btn_details_stop), ==, "停止");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(window->btn_details_stop)), ==,
                  "保存数据并停止服务器");
  GtkWidget *console = adw_view_stack_get_child_by_name(window->details_stack, "console");
  g_assert_nonnull(console);
  g_assert_cmpstr(adw_view_stack_page_get_title(adw_view_stack_get_page(window->details_stack, console)),
                  ==, "控制台");

  /* Exercise real count forms and long-long printf formats in the production
   * catalog, including zero. These display formats must not be truncated by
   * extraction at an adjacent GLib integer-format macro. */
  for (gulong count = 0; count <= 2; count++) {
    const char *format = g_dngettext(GETTEXT_PACKAGE, "Terrain / %u chunk", "Terrain / %u chunks", count);
    g_assert_cmpstr(format, !=, "Terrain / %u chunk");
    g_assert_cmpstr(format, !=, "Terrain / %u chunks");
    g_assert_cmpstr(format, ==, g_dngettext(GETTEXT_PACKAGE, "Terrain / %u chunk", "Terrain / %u chunks", 0));
    g_autofree char *summary = g_strdup_printf(format, (guint)count);
    g_autofree char *number = g_strdup_printf("%lu", count);
    g_assert_nonnull(strstr(summary, number));
  }
  g_assert_cmpstr(_("%llud %lluh"), !=, "%llud %lluh");
  g_autofree char *duration = g_strdup_printf(_("%llud %lluh"), 42ULL, 7ULL);
  g_assert_nonnull(strstr(duration, "42"));
  g_assert_nonnull(strstr(duration, "7"));

  /* A fake executable tests dynamic metadata presentation without starting a server. */
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_autofree char *executable = NULL;
  gsize executable_size = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_MANAGED_PUMPKIN_BINARY"),
                                   &executable, &executable_size, NULL));
  gsize size = MAX(executable_size, (gsize)MIN_VALID_PUMPKIN_BINARY_BYTES);
  g_autofree char *binary = g_malloc0(size);
  memcpy(binary, executable, executable_size);
  g_assert_true(g_file_set_contents(bin, binary, (gssize)size, NULL));
  g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
  pumpkin_server_set_installed_build_label(server, "Build 2026-10-12 03:04 UTC");
  pumpkin_server_set_installed_build_id(server, "old-build");
  pumpkin_config_set_date_format(window->config, PUMPKIN_DATE_FORMAT_YMD);
  pumpkin_config_set_time_format(window->config, PUMPKIN_TIME_FORMAT_24H);
  g_assert_nonnull(find_label_containing(overview_row_for(window, server), "构建版本"));
  g_assert_cmpstr(pumpkin_server_get_installed_build_label(server), ==,
                  "Build 2026-10-12 03:04 UTC");
  window->latest_build_id = g_strdup("new-build");
  g_assert_nonnull(find_css_class(overview_row_for(window, server), "update-button"));
  g_assert_false(pumpkin_server_get_running(server));

  /* Run the raw-log/typed-event regressions in the Chinese session as well as
   * the English baseline. Signals are fixtures; no real process is started. */
  test_console_polling(window, server);
  g_assert_cmpstr(pumpkin_server_get_name(server), ==, "I18n loading");
  g_assert_cmpstr(pumpkin_server_get_installed_build_label(server), ==,
                  "Build 2026-10-12 03:04 UTC");
  g_assert_cmpstr(pumpkin_server_get_installed_build_id(server), ==, "old-build");
  g_assert_cmpstr(pumpkin_i18n_get_language(), ==, "zh_CN");
  g_assert_cmpstr(setlocale(LC_NUMERIC, NULL), ==, "C");
  g_assert_false(pumpkin_server_get_running(server));

  gtk_window_destroy(GTK_WINDOW(window));
  spin(650);
  g_print("PASS: production Simplified Chinese catalog translates dynamic UI, counts and duration formats without changing raw metadata, branding, numeric formats or log/event handling\n");
}

static void test_language_settings_only(PumpkinWindow *window)
{
  ++window->latest_resolve_generation;
  if (window->latest_resolve_cancellable != NULL) g_cancellable_cancel(window->latest_resolve_cancellable);
  g_clear_object(&window->latest_resolve_cancellable);
  window->latest_resolve_in_flight = FALSE;
  g_assert_cmpstr(pumpkin_config_get_language(window->config), ==, "en");
  g_assert_cmpstr(pumpkin_i18n_get_language(), ==, "en");
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_language), ==, pumpkin_language_index("en"));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->row_language_restart)));
  g_assert_cmpuint(g_list_model_get_n_items(adw_combo_row_get_model(window->row_language)), ==, pumpkin_language_count());
  g_assert_true(pumpkin_language_is_valid("zh_TW"));
  g_assert_cmpstr(gtk_string_list_get_string(GTK_STRING_LIST(adw_combo_row_get_model(window->row_language)),
                  pumpkin_language_index("zh_TW")), ==, "Chinese (Traditional)");
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("de"));
  g_assert_true(window->settings_dirty);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("en"));
  g_assert_false(window->settings_dirty);

  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "Language settings", NULL);
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_autofree char *binary = NULL;
  gsize size = 0;
  g_assert_true(g_file_get_contents(g_getenv("FAKE_MANAGED_PUMPKIN_BINARY"), &binary, &size, NULL));
  g_assert_true(g_file_set_contents(bin, binary, (gssize)size, NULL));
  g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
  pumpkin_server_set_auto_restart(server, FALSE);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  g_assert_true(pumpkin_server_start(server, NULL));
  spin(200);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("zh_TW"));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  PumpkinConfig *saved = pumpkin_config_load(NULL);
  g_assert_cmpstr(pumpkin_config_get_language(saved), ==, "zh_TW");
  pumpkin_config_free(saved);
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window->row_language_restart)));
  g_assert_false(window->settings_dirty);
  g_assert_true(pumpkin_server_get_running(server));
  g_assert_cmpstr(pumpkin_i18n_get_language(), ==, "en");
  g_assert_cmpstr(_("Overview"), ==, "Overview");
  g_assert_cmpstr(setlocale(LC_NUMERIC, NULL), ==, "C");
  update_settings_form(window);
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_language), ==, pumpkin_language_index("zh_TW"));
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("system"));
  gtk_drop_down_set_selected(window->drop_date_format, PUMPKIN_DATE_FORMAT_YMD);
  PumpkinTimeFormat time_format = pumpkin_config_get_time_format(window->config) == PUMPKIN_TIME_FORMAT_24H ?
    PUMPKIN_TIME_FORMAT_12H : PUMPKIN_TIME_FORMAT_24H;
  gboolean detailed_cards = !pumpkin_config_get_detailed_overview_cards(window->config);
  gtk_drop_down_set_selected(window->drop_time_format, time_format);
  gtk_switch_set_active(window->switch_detailed_overview_cards, detailed_cards);
  g_signal_emit_by_name(window->btn_save_settings, "clicked");
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_true(pumpkin_server_get_running(server));
  saved = pumpkin_config_load(NULL);
  g_assert_cmpstr(pumpkin_config_get_language(saved), ==, "system");
  g_assert_cmpint(pumpkin_config_get_date_format(saved), ==, PUMPKIN_DATE_FORMAT_YMD);
  g_assert_cmpint(pumpkin_config_get_time_format(saved), ==, time_format);
  g_assert_cmpint(pumpkin_config_get_detailed_overview_cards(saved), ==, detailed_cards);
  g_assert_false(window->settings_dirty);
  pumpkin_config_free(saved);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("en"));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->row_language_restart)));

  /* Saving a language must not mark an unrelated server draft as saved. */
  int saved_port = pumpkin_server_get_port(server);
  g_autofree char *draft_port = g_strdup_printf("%d", saved_port + 1);
  gtk_editable_set_text(GTK_EDITABLE(window->entry_server_port), draft_port);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("de"));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  g_assert_cmpint(pumpkin_server_get_port(server), ==, saved_port);
  g_assert_true(window->settings_dirty);
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(window->btn_save_settings)));
  update_settings_form(window);
  g_assert_false(window->settings_dirty);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("en"));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");

  /* A failed write must not claim success or discard the pending selection. */
  g_autofree char *path = g_build_filename(g_get_user_config_dir(), "smashed-pumpkin", "config.ini", NULL);
  g_autofree char *original = NULL;
  g_assert_true(g_file_get_contents(path, &original, NULL, NULL));
  g_assert_cmpint(g_remove(path), ==, 0);
  g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("de"));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  g_assert_cmpstr(pumpkin_config_get_language(window->config), ==, "en");
  g_assert_true(window->settings_dirty);
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->row_language_restart)));
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_DIR));
  g_signal_emit_by_name(window->btn_save_settings, "clicked"); /* Also protect the server settings Save path. */
  g_assert_true(pumpkin_server_get_running(server));
  g_assert_cmpstr(pumpkin_config_get_language(window->config), ==, "en");
  g_assert_true(window->settings_dirty);
  g_assert_cmpint(g_rmdir(path), ==, 0);
  g_assert_true(g_file_set_contents(path, original, -1, NULL));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  g_assert_cmpstr(pumpkin_config_get_language(window->config), ==, "de");
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window->row_language_restart)));
  g_assert_false(window->settings_dirty);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("zh_CN"));
  update_settings_form(window);
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_language), ==, pumpkin_language_index("de"));

  /* Unsupported IDs must survive saves of unrelated preferences in either form. */
  g_autoptr(GKeyFile) future_config = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(future_config, path, G_KEY_FILE_NONE, NULL));
  g_key_file_set_string(future_config, "behavior", "language", "future-language");
  g_autofree char *future_data = g_key_file_to_data(future_config, NULL, NULL);
  g_assert_true(g_file_set_contents(path, future_data, -1, NULL));
  pumpkin_config_free(window->config);
  window->config = pumpkin_config_load(NULL);
  update_settings_form(window);
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_language), ==, pumpkin_language_index("en"));
  gtk_drop_down_set_selected(window->drop_date_format, PUMPKIN_DATE_FORMAT_DMY);
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  saved = pumpkin_config_load(NULL);
  g_assert_cmpstr(pumpkin_config_get_language(saved), ==, "future-language");
  pumpkin_config_free(saved);
  gtk_switch_set_active(window->switch_detailed_overview_cards, !detailed_cards);
  g_signal_emit_by_name(window->btn_save_settings, "clicked");
  saved = pumpkin_config_load(NULL);
  g_assert_cmpstr(pumpkin_config_get_language(saved), ==, "future-language");
  pumpkin_config_free(saved);
  adw_combo_row_set_selected(window->row_language, pumpkin_language_index("de"));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");

  /* Reset chooses system, but remains unsaved until explicitly saving. */
  g_signal_emit_by_name(window->btn_reset_general_settings, "clicked");
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_nonnull(dialog);
  g_signal_emit_by_name(dialog, "response", "reset");
  adw_dialog_force_close(dialog);
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_language), ==, pumpkin_language_index("system"));
  g_assert_cmpstr(pumpkin_config_get_language(window->config), ==, "de");
  g_assert_true(window->settings_dirty);
  g_assert_true(pumpkin_server_get_running(server));
  g_assert_cmpstr(pumpkin_i18n_get_language(), ==, "en");
  pumpkin_server_stop(server);
  wait_for_test_server_exit(server);
  gtk_window_destroy(GTK_WINDOW(window));
  spin(650);
  g_print("PASS: language selection saves stable IDs, supports reset/discard, handles write errors and never restarts a running server\n");
}

int main(void)
{
  gtk_disable_setlocale();
  pumpkin_i18n_startup();
  adw_init();
  smashed_pumpkin_register_resource();
  g_autoptr(AdwApplication) app = adw_application_new("dev.rotstein.SmashedPumpkin.UIRegression", G_APPLICATION_NON_UNIQUE);
  g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
  PumpkinWindow *window = PUMPKIN_WINDOW(pumpkin_window_new(app));
  if (g_strcmp0(g_getenv("SMASHED_UI_TEST_SCOPE"), "language-settings") == 0) {
    test_language_settings_only(window); return 0;
  }
  if (g_strcmp0(g_getenv("SMASHED_UI_TEST_SCOPE"), "i18n-loading") == 0) {
    test_i18n_loading_only(window); return 0;
  }
  if (g_strcmp0(g_getenv("SMASHED_UI_TEST_SCOPE"), "i18n-chinese") == 0) {
    test_i18n_chinese_only(window); return 0;
  }
  if (g_strcmp0(g_getenv("SMASHED_UI_TEST_SCOPE"), "i18n-boundaries") == 0) {
    test_i18n_boundaries_only(window); return 0;
  }
  if (g_strcmp0(g_getenv("SMASHED_UI_TEST_SCOPE"), "releases") == 0) {
    test_release_channels_only(window); return 0;
  }
  if (g_strcmp0(g_getenv("SMASHED_UI_TEST_SCOPE"), "worlds-map-access") == 0) {
    test_worlds_map_access(window); return 0;
  }
  if (g_strcmp0(g_getenv("SMASHED_UI_TEST_SCOPE"), "minimap") == 0) {
    test_minimap_only(window); return 0;
  }
  g_assert_false(adw_switch_row_get_active(window->row_start_expanded));
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_start_mode), ==, PUMPKIN_START_MAXIMIZED);
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_mode)));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_expanded)));
  gtk_switch_set_active(window->switch_autostart_on_boot, TRUE);
  gtk_switch_set_active(window->switch_start_minimized, TRUE);
  adw_switch_row_set_active(window->row_start_expanded, TRUE);
  g_assert_true(gtk_switch_get_active(window->switch_start_minimized));
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(window->switch_start_minimized)));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_expanded)));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_mode)));
  gtk_switch_set_active(window->switch_start_minimized, FALSE);
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_expanded)));
  adw_combo_row_set_selected(window->row_start_mode, PUMPKIN_START_FULLSCREEN);
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_monitor)));
  if (g_list_model_get_n_items(adw_combo_row_get_model(window->row_start_monitor)) > 1) {
    adw_combo_row_set_selected(window->row_start_monitor, 1);
  }
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  PumpkinConfig *saved = pumpkin_config_load(NULL);
  g_assert_true(pumpkin_config_get_start_expanded(saved));
  g_assert_cmpint(pumpkin_config_get_start_mode(saved), ==, PUMPKIN_START_FULLSCREEN);
  g_assert_false(pumpkin_config_get_start_minimized(saved));
  pumpkin_config_free(saved);
  gtk_switch_set_active(window->switch_autostart_on_boot, FALSE);
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_expanded)));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_mode)));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(window->row_start_monitor)));
  g_signal_emit_by_name(window->btn_save_general_settings, "clicked");
  g_print("PASS: settings defaults, startup dependencies and retained preferences\n");

  g_autoptr(PumpkinServer) server = pumpkin_server_store_add(window->store, "Test Pumpkin Server", NULL);
  g_autofree char *plugins = pumpkin_server_get_plugins_dir(server);
  g_mkdir_with_parents(plugins, 0700);
  g_autofree char *file = g_build_filename(plugins, "ExamplePlugin.wasm", NULL);
  g_assert_true(g_file_set_contents(file, "\0asm\1\0\0\0", 8, NULL));
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  adw_view_stack_set_visible_child_name(window->view_stack, "details");
  gtk_window_set_default_size(GTK_WINDOW(window), 1180, 860);
  gtk_window_present(GTK_WINDOW(window));
  spin(500);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  g_assert_true(window->current == server);
  adw_combo_row_set_selected(window->row_start_mode, PUMPKIN_START_MAXIMIZED);
  g_signal_emit_by_name(window->btn_save_settings, "clicked");
  saved = pumpkin_config_load(NULL);
  g_assert_true(pumpkin_config_get_start_expanded(saved));
  g_assert_cmpint(pumpkin_config_get_start_mode(saved), ==, PUMPKIN_START_MAXIMIZED);
  pumpkin_config_free(saved);
  adw_combo_row_set_selected(window->row_start_mode, PUMPKIN_START_FULLSCREEN);
  spin(200);
  pumpkin_window_select_server(window, pumpkin_server_get_id(server));
  adw_view_stack_set_visible_child_name(window->view_stack, "details");

  test_players(window, server);
  if (g_getenv("SMASHED_PRIVATE_X11_POINTER") != NULL) {
    g_print("PASS: real XTest pointer clicks traverse GTK's modal event boundary and cancel safely\n");
  }
  test_worlds(window, server);
  test_reload_controls(window);
  test_logs(window, server);
  test_console_polling(window, server);
  test_console_layout(window, server);
  adw_view_stack_set_visible_child_name(window->details_stack, "settings");
  screenshot(GTK_WIDGET(window), "/tmp/smashed-startup-settings.png");
  adw_view_stack_set_visible_child_name(window->details_stack, "plugins");
  adw_view_stack_set_visible_child_name(window->plugins_stack, "installed_plugins");
  spin(300);
  GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_index(window->plugin_list, 0));
  g_assert_nonnull(row);
  GtkWidget *menu = find_widget(row, GTK_TYPE_MENU_BUTTON);
  g_assert_nonnull(menu);
  g_assert_cmpstr(gtk_menu_button_get_icon_name(GTK_MENU_BUTTON(menu)), ==, "view-more-symbolic");
  g_assert_true(gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(gdk_display_get_default()), "view-more-symbolic"));
  gtk_menu_button_popup(GTK_MENU_BUTTON(menu));
  screenshot(GTK_WIDGET(window), "/tmp/smashed-plugin-actions.png");
  GtkWidget *remove = find_button(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu))), "Remove plugin…");
  g_assert_nonnull(remove);
  g_signal_emit_by_name(remove, "clicked");
  spin(100);
  AdwDialog *confirmation = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(confirmation));
  adw_dialog_close(confirmation);
  spin(150);
  g_assert_true(g_file_test(file, G_FILE_TEST_EXISTS));
  g_print("PASS: installed plugin has an available three-dot action menu, not a trash icon\n");

  adw_view_stack_set_visible_child_name(window->details_stack, "stats");
  spin(200);
  if (window->stats_refresh_id != 0) {
    g_source_remove(window->stats_refresh_id);
    window->stats_refresh_id = 0;
  }
  window->stats_count = STATS_SAMPLES;
  window->stats_index = 0;
  for (int i = 0; i < STATS_SAMPLES; i++) {
    window->stats_cpu[i] = 35 + 15 * sin(i * 0.13);
    window->stats_ram_mb[i] = 512 + i * 0.7;
    window->stats_disk_mb[i] = 19.7 + 0.2 * sin(i * 0.1);
    window->stats_players[i] = 5;
    window->stats_time_usec[i] = g_get_monotonic_time() - (STATS_SAMPLES - 1 - i) * G_USEC_PER_SEC;
  }
  GtkDrawingArea *areas[] = {window->stats_graph_usage, window->stats_graph_ram, window->stats_graph_disk};
  /* Real pointer events from the developer's desktop must not compete with
   * the deterministic synthetic hover events below. */
  for (int metric = 0; metric < 3; metric++) {
    g_autoptr(GtkEventController) motion = motion_for(gtk_widget_get_parent(GTK_WIDGET(areas[metric])));
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_NONE);
  }
  for (int metric = 0; metric < 3; metric++) {
    GtkWidget *area = GTK_WIDGET(areas[metric]);
    g_autoptr(GtkEventController) motion = motion_for(gtk_widget_get_parent(area));
    double x = gtk_widget_get_width(area) - 25;
    double y = MIN(100, gtk_widget_get_height(area) - 25);
    g_assert_cmpfloat(y, >, 12);
    g_signal_emit_by_name(motion, "motion", x, y);
    spin(60);
    double reference_x = 0, reference_y = 0;
    int reference_width = 0, reference_height = 0;
    for (int i = 0; i < 100; i++) {
      g_signal_emit_by_name(motion, "motion", x, y);
      spin(2);
      g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window->stats_hover_label)));
      double tx, ty;
      gtk_fixed_get_child_position(window->stats_hover_layer, GTK_WIDGET(window->stats_hover_label), &tx, &ty);
      int minimum, width, height;
      gtk_widget_measure(GTK_WIDGET(window->stats_hover_label), GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &width, NULL, NULL);
      gtk_widget_measure(GTK_WIDGET(window->stats_hover_label), GTK_ORIENTATION_VERTICAL, width, &minimum, &height, NULL, NULL);
      if (i == 0) { reference_x = tx; reference_y = ty; reference_width = width; reference_height = height; }
      g_assert_cmpfloat(tx, ==, reference_x);
      g_assert_cmpfloat(ty, ==, reference_y);
      g_assert_cmpint(width, ==, reference_width);
      g_assert_cmpint(height, ==, reference_height);
      g_assert_cmpint(gtk_widget_get_margin_start(GTK_WIDGET(window->stats_hover_label)), ==, 0);
      g_assert_cmpint(gtk_widget_get_margin_top(GTK_WIDGET(window->stats_hover_label)), ==, 0);
    }
    g_signal_emit_by_name(motion, "leave");
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window->stats_hover_label)));
  }
  g_autoptr(GtkEventController) cpu_motion = motion_for(gtk_widget_get_parent(GTK_WIDGET(areas[0])));
  g_signal_emit_by_name(cpu_motion, "motion", gtk_widget_get_width(GTK_WIDGET(areas[0])) - 80.0, 100.0);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-stats-hover.png");
  g_print("PASS: 300 repeated hover motions across CPU, RAM and TPS retain identical position and size\n");

  adw_view_stack_set_visible_child_name(window->details_stack, "plugins");
  adw_view_stack_set_visible_child_name(window->plugins_stack, "marketplace_plugins");
  gint64 deadline = g_get_monotonic_time() + 25 * G_USEC_PER_SEC;
  while (window->marketplace_loading && g_get_monotonic_time() < deadline) spin(10);
  g_assert_false(window->marketplace_loading);
  g_assert_false(window->marketplace_load_failed);
  row = GTK_WIDGET(gtk_list_box_get_row_at_index(window->marketplace_list, 0));
  g_assert_nonnull(find_label(row, "UI Fixture Plugin"));
  GtkWidget *open = find_widget(row, GTK_TYPE_BUTTON);
  g_assert_nonnull(open);
  g_signal_emit_by_name(open, "clicked");
  spin(250);
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_nonnull(dialog);
  int min_width, min_height;
  gtk_widget_get_size_request(GTK_WIDGET(dialog), &min_width, &min_height);
  g_assert_cmpint(min_width, >=, 300);
  g_assert_cmpint(min_height, >=, 280);
  screenshot(GTK_WIDGET(window), "/tmp/smashed-plugin-details.png");
  adw_dialog_close(dialog);
  g_print("PASS: local Marketplace fixture details have explicit minimum size\n");

  pumpkin_config_set_start_expanded(window->config, TRUE);
  pumpkin_config_set_autostart_on_boot(window->config, FALSE);
  pumpkin_window_apply_startup(GTK_WINDOW(window), window->config);
  spin(200);
  g_assert_false(gtk_window_is_maximized(GTK_WINDOW(window)));
  pumpkin_config_set_autostart_on_boot(window->config, TRUE);
  pumpkin_config_set_start_minimized(window->config, TRUE);
  pumpkin_window_apply_startup(GTK_WINDOW(window), window->config);
  spin(200);
  g_assert_false(gtk_window_is_maximized(GTK_WINDOW(window)));
  pumpkin_config_set_start_minimized(window->config, FALSE);
  pumpkin_config_set_start_mode(window->config, PUMPKIN_START_MAXIMIZED);
  pumpkin_config_set_start_monitor(window->config, "connector:not-attached");
  pumpkin_window_apply_startup(GTK_WINDOW(window), window->config);
  spin(200);
  g_assert_true(gtk_window_is_maximized(GTK_WINDOW(window)));
  gtk_window_unmaximize(GTK_WINDOW(window));
  spin(200);
  pumpkin_config_set_start_mode(window->config, PUMPKIN_START_FULLSCREEN);
  pumpkin_window_apply_startup(GTK_WINDOW(window), window->config);
  spin(200);
  g_assert_true(gtk_window_is_fullscreen(GTK_WINDOW(window)));
  gtk_window_unfullscreen(GTK_WINDOW(window));
  g_print("PASS: startup maximized/fullscreen requests and absent-monitor fallback\n");
  GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());
  g_autoptr(GdkMonitor) chosen = g_list_model_get_item(monitors, g_list_model_get_n_items(monitors) - 1);
  g_autofree char *chosen_id = pumpkin_monitor_id(chosen);
  pumpkin_config_set_start_monitor(window->config, chosen_id);
  for (int mode = 0; mode < 2; mode++) {
    GtkWindow *startup = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(startup, 900, 650);
    pumpkin_config_set_start_mode(window->config, (PumpkinStartMode)mode);
    pumpkin_window_apply_startup(startup, window->config);
    gtk_window_present(startup);
    spin(300);
    g_assert_true(mode == 0 ? gtk_window_is_maximized(startup) : gtk_window_is_fullscreen(startup));
    GdkMonitor *actual = gdk_display_get_monitor_at_surface(gdk_display_get_default(), gtk_native_get_surface(GTK_NATIVE(startup)));
    g_autofree char *actual_id = pumpkin_monitor_id(actual);
    g_print("Startup %s: requested %s, actual %s\n", mode == 0 ? "maximized" : "fullscreen", chosen_id, actual_id);
    if (mode == 0 && pumpkin_startup_can_place_maximized(gdk_display_get_default())) {
      g_assert_cmpstr(actual_id, ==, chosen_id);
    }
    if (mode == 1) {
      g_assert_cmpstr(actual_id, ==, chosen_id);
      g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(GTK_WIDGET(startup));
      gboolean activated = FALSE;
      for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
        g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, i);
        if (!GTK_IS_SHORTCUT_CONTROLLER(controller)) continue;
        GListModel *shortcuts = G_LIST_MODEL(controller);
        for (guint j = 0; j < g_list_model_get_n_items(shortcuts); j++) {
          g_autoptr(GtkShortcut) shortcut = g_list_model_get_item(shortcuts, j);
          GtkShortcutTrigger *trigger = gtk_shortcut_get_trigger(shortcut);
          if (!GTK_IS_KEYVAL_TRIGGER(trigger) || gtk_keyval_trigger_get_keyval(GTK_KEYVAL_TRIGGER(trigger)) != GDK_KEY_F11) continue;
          activated = gtk_shortcut_action_activate(gtk_shortcut_get_action(shortcut), GTK_SHORTCUT_ACTION_EXCLUSIVE,
                                                   GTK_WIDGET(startup), NULL);
        }
      }
      g_assert_true(activated);
      spin(100);
      g_assert_false(gtk_window_is_fullscreen(startup));
    }
    gtk_window_destroy(startup);
  }
  g_print("PASS: pre-presentation startup modes, selected fullscreen monitor and F11 callback\n");
  g_signal_emit_by_name(window->btn_reset_general_settings, "clicked");
  spin(100);
  AdwDialog *reset = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(reset));
  g_signal_emit_by_name(reset, "response", "reset");
  adw_dialog_close(reset);
  spin(100);
  g_assert_false(adw_switch_row_get_active(window->row_start_expanded));
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_start_mode), ==, PUMPKIN_START_MAXIMIZED);
  g_assert_cmpuint(adw_combo_row_get_selected(window->row_start_monitor), ==, 0);
  g_print("PASS: both save paths and reset to disabled / maximized / system default\n");
  test_update_pipeline(window);
  test_stop_confirmation(window);
  gtk_window_destroy(GTK_WINDOW(window));
  return 0;
}
