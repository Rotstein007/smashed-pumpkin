#include "window.h"

#include "app-config.h"
#include "download.h"
#include "server-store.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

struct _PumpkinWindow {
  AdwApplicationWindow parent_instance;

  AdwViewStack *view_stack;
  AdwViewStack *details_stack;

  GtkListBox *server_list;
  GtkTextView *log_view;
  GtkListBox *overview_list;
  char *latest_url;

  GtkButton *btn_add_server;
  GtkButton *btn_remove_server;
  GtkButton *btn_add_server_overview;
  GtkButton *btn_import_server;
  GtkButton *btn_import_server_overview;
  GtkButton *btn_open_plugins;
  GtkButton *btn_open_players;
  GtkButton *btn_open_worlds;
  GtkButton *btn_save_settings;
  GtkEntry *entry_plugin_url;
  GtkButton *btn_install_plugin;

  GtkListBox *plugin_list;
  GtkListBox *world_list;
  GtkListBox *player_list;
  GtkListBox *log_files_list;
  GtkTextView *log_file_view;
  GtkDropDown *log_filter;
  GtkDropDown *log_level_filter;
  GtkEntry *log_search;
  GtkButton *btn_open_logs;
  GtkLabel *label_sys_cpu;
  GtkLabel *label_sys_ram;
  GtkLabel *label_srv_cpu;
  GtkLabel *label_srv_ram;
  GtkBox *stats_row;

  GtkLabel *details_title;
  GtkImage *details_server_icon;
  GtkButton *btn_details_back;
  GtkButton *btn_details_start;
  GtkButton *btn_details_stop;
  GtkButton *btn_details_restart;
  GtkButton *btn_details_install;
  GtkButton *btn_details_update;
  GtkButton *btn_details_check_updates;
  GtkSpinner *download_spinner;
  GtkEntry *entry_command;
  GtkButton *btn_send_command;
  GtkButton *btn_console_copy;
  GtkButton *btn_console_clear;
  GtkButton *btn_open_server_root;
  GtkLabel *details_error;
  GtkRevealer *details_error_revealer;
  GtkLabel *details_status;
  GtkRevealer *details_status_revealer;
  guint status_timeout_id;
  GtkProgressBar *download_progress;
  GtkRevealer *download_progress_revealer;
  guint restart_delay_id;
  guint start_delay_id;
  guint action_cooldown_id;
  gboolean action_cooldown;
  GHashTable *download_progress_state;
  gboolean restart_requested;
  guint stats_refresh_id;
  unsigned long long last_total_jiffies;
  unsigned long long last_idle_jiffies;
  unsigned long long last_proc_jiffies;
  long clk_tck;

  int ui_state;

  GtkEntry *entry_server_name;
  GtkEntry *entry_download_url;
  GtkButton *btn_choose_icon;
  GtkButton *btn_reset_icon;
  GtkEntry *entry_server_port;
  GtkEntry *entry_max_players;
  GtkSwitch *switch_auto_restart;
  GtkEntry *entry_auto_restart_delay;
  GtkEntry *entry_rcon_host;
  GtkEntry *entry_rcon_port;
  GtkPasswordEntry *entry_rcon_password;
  GtkSwitch *switch_use_cache;
  gboolean settings_dirty;
  gboolean settings_loading;
  gboolean settings_guard;
  char *pending_details_page;
  char *pending_view_page;
  PumpkinServer *pending_server;
  char *current_log_path;

  PumpkinServerStore *store;
  PumpkinServer *current;
  PumpkinConfig *config;
  guint players_refresh_id;
  GHashTable *live_player_names;
  GHashTable *console_buffers;
};

G_DEFINE_FINAL_TYPE(PumpkinWindow, pumpkin_window, ADW_TYPE_APPLICATION_WINDOW)

enum {
  UI_STATE_IDLE = 0,
  UI_STATE_STARTING,
  UI_STATE_RUNNING,
  UI_STATE_STOPPING,
  UI_STATE_RESTARTING,
  UI_STATE_ERROR
};

typedef struct {
  PumpkinWindow *self;
  PumpkinServer *server;
} RestartContext;

typedef struct DownloadContext {
  PumpkinWindow *self;
  PumpkinServer *server;
  char *used_url;
  char *dest_path;
  char *tmp_path;
  char *server_bin;
  gboolean use_cache;
} DownloadContext;

typedef struct {
  goffset current;
  goffset total;
  gboolean active;
  GtkWidget *overview_bar;
} DownloadProgressState;

static void start_download_for_server(PumpkinWindow *self, PumpkinServer *server, const char *url);
static void on_download_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_overview_update_clicked(GtkButton *button, gpointer user_data);
static void on_overview_settings_clicked(GtkButton *button, gpointer user_data);
static void on_overview_remove_clicked(GtkButton *button, PumpkinWindow *self);
static void on_overview_remove_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static void on_add_server_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static void on_add_server_entry_activate(GtkEntry *entry, PumpkinWindow *self);
static void on_import_server(GtkButton *button, PumpkinWindow *self);
static void on_import_server_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_details_back(GtkButton *button, PumpkinWindow *self);
static void on_details_start(GtkButton *button, PumpkinWindow *self);
static void on_details_stop(GtkButton *button, PumpkinWindow *self);
static void on_details_restart(GtkButton *button, PumpkinWindow *self);
static void on_download_progress(goffset current, goffset total, gpointer user_data);
static void on_details_install(GtkButton *button, PumpkinWindow *self);
static void on_details_update(GtkButton *button, PumpkinWindow *self);
static void on_details_check_updates(GtkButton *button, PumpkinWindow *self);
static void update_settings_form(PumpkinWindow *self);
static void on_save_settings(GtkButton *button, PumpkinWindow *self);
static void on_console_copy(GtkButton *button, PumpkinWindow *self);
static void on_console_clear(GtkButton *button, PumpkinWindow *self);
static void on_settings_changed(GtkEditable *editable, PumpkinWindow *self);
static void on_settings_switch_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void on_details_stack_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void on_settings_leave_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static void on_send_command(GtkButton *button, PumpkinWindow *self);
static void on_choose_icon(GtkButton *button, PumpkinWindow *self);
static void on_reset_icon(GtkButton *button, PumpkinWindow *self);
static void select_server(PumpkinWindow *self, PumpkinServer *server);
static void refresh_overview_list(PumpkinWindow *self);
static GtkWidget *create_server_row(PumpkinServer *server);
static GtkWidget *create_server_icon_widget(PumpkinServer *server);
static void update_details(PumpkinWindow *self);
static void on_world_delete_clicked(GtkButton *button, PumpkinWindow *self);
static void refresh_world_list(PumpkinWindow *self);
static void refresh_player_list(PumpkinWindow *self);
static gboolean refresh_players_tick(gpointer user_data);
static void on_player_row_activated(GtkListBox *box, GtkListBoxRow *row, PumpkinWindow *self);
static void on_player_action_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static void update_live_player_names(PumpkinWindow *self, const char *line);
static void refresh_log_files(PumpkinWindow *self);
static void on_log_filter_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void on_log_level_filter_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void on_log_search_changed(GtkEditable *editable, PumpkinWindow *self);
static void on_log_file_activated(GtkListBox *box, GtkListBoxRow *row, PumpkinWindow *self);
static void on_open_logs(GtkButton *button, PumpkinWindow *self);
static gboolean restart_after_delay(gpointer data);
static gboolean start_after_delay(gpointer data);
static gboolean action_cooldown_cb(gpointer data);
static gboolean hide_status_cb(gpointer data);
static gboolean update_stats_tick(gpointer data);
static void on_install_plugin(GtkButton *button, PumpkinWindow *self);
static void on_open_server_root(GtkButton *button, PumpkinWindow *self);
static gboolean restart_after_delay(gpointer data);

static void
apply_compact_button(GtkWidget *button)
{
  if (button == NULL) {
    return;
  }
  gtk_widget_set_size_request(button, -1, 24);
}

static void
set_details_error(PumpkinWindow *self, const char *message)
{
  if (self->details_error == NULL) {
    return;
  }
  if (message == NULL || *message == '\0') {
    gtk_label_set_text(self->details_error, "");
    if (self->details_error_revealer != NULL) {
      gtk_revealer_set_reveal_child(self->details_error_revealer, FALSE);
    }
    return;
  }

  gtk_label_set_text(self->details_error, message);
  if (self->details_error_revealer != NULL) {
    gtk_revealer_set_reveal_child(self->details_error_revealer, TRUE);
  }
}

static void
set_details_status(PumpkinWindow *self, const char *message, guint timeout_seconds)
{
  if (self->details_status == NULL) {
    return;
  }
  if (self->status_timeout_id != 0) {
    g_source_remove(self->status_timeout_id);
    self->status_timeout_id = 0;
  }
  if (message == NULL || *message == '\0') {
    gtk_label_set_text(self->details_status, "");
    if (self->details_status_revealer != NULL) {
      gtk_revealer_set_reveal_child(self->details_status_revealer, FALSE);
    }
    return;
  }

  gtk_label_set_text(self->details_status, message);
  if (self->details_status_revealer != NULL) {
    gtk_revealer_set_reveal_child(self->details_status_revealer, TRUE);
  }
  if (timeout_seconds > 0) {
    self->status_timeout_id = g_timeout_add_seconds(timeout_seconds, hide_status_cb, g_object_ref(self));
  }
}

static void
set_details_status_for_server(PumpkinWindow *self, PumpkinServer *server, const char *message, guint timeout_seconds)
{
  if (self->current != NULL && self->current == server) {
    set_details_status(self, message, timeout_seconds);
  }
}

static gboolean
hide_status_cb(gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  self->status_timeout_id = 0;
  set_details_status(self, NULL, 0);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static gboolean
log_line_matches_level(const char *line, int level_index)
{
  if (level_index <= 0 || line == NULL) {
    return TRUE;
  }
  g_autofree char *lower = g_ascii_strdown(line, -1);
  if (level_index == 1) {
    return strstr(lower, "info") != NULL;
  }
  if (level_index == 2) {
    return strstr(lower, "warn") != NULL;
  }
  if (level_index == 3) {
    return strstr(lower, "error") != NULL;
  }
  return TRUE;
}

static void
mark_settings_dirty(PumpkinWindow *self)
{
  if (self->settings_loading) {
    return;
  }
  self->settings_dirty = TRUE;
}

static void
discard_settings_changes(PumpkinWindow *self)
{
  self->settings_dirty = FALSE;
  update_settings_form(self);
}

static void
on_settings_changed(GtkEditable *editable, PumpkinWindow *self)
{
  (void)editable;
  mark_settings_dirty(self);
}

static void
on_settings_switch_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self)
{
  (void)object;
  (void)pspec;
  mark_settings_dirty(self);
}

static void
confirm_leave_settings(PumpkinWindow *self, const char *target_details, const char *target_view)
{
  if (self->settings_guard) {
    return;
  }
  g_free(self->pending_details_page);
  g_free(self->pending_view_page);
  self->pending_details_page = target_details != NULL ? g_strdup(target_details) : NULL;
  self->pending_view_page = target_view != NULL ? g_strdup(target_view) : NULL;

  AdwDialog *dialog = adw_alert_dialog_new("Unsaved changes", "Save changes before leaving Settings?");
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_response(alert, "cancel", "Cancel");
  adw_alert_dialog_add_response(alert, "discard", "Discard");
  adw_alert_dialog_add_response(alert, "save", "Save");
  adw_alert_dialog_set_default_response(alert, "save");
  adw_alert_dialog_set_close_response(alert, "cancel");
  adw_alert_dialog_set_response_appearance(alert, "discard", ADW_RESPONSE_DESTRUCTIVE);
  g_object_set_data(G_OBJECT(dialog), "window", self);
  adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_settings_leave_confirmed, self);
}

static void
on_settings_leave_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  PumpkinWindow *self = g_object_get_data(G_OBJECT(dialog), "window");
  if (self == NULL) {
    return;
  }
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
  if (response == NULL || g_strcmp0(response, "cancel") == 0) {
    self->settings_guard = TRUE;
    adw_view_stack_set_visible_child_name(self->details_stack, "settings");
    self->settings_guard = FALSE;
    return;
  }

  if (g_strcmp0(response, "save") == 0) {
    on_save_settings(NULL, self);
    self->settings_dirty = FALSE;
  } else if (g_strcmp0(response, "discard") == 0) {
    discard_settings_changes(self);
  }

  if (self->pending_server != NULL) {
    PumpkinServer *server = self->pending_server;
    self->pending_server = NULL;
    self->settings_guard = TRUE;
    select_server(self, server);
    GtkListBoxRow *row = NULL;
    if (self->server_list != NULL) {
      GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->server_list));
      while (child != NULL) {
        GtkListBoxRow *candidate = GTK_LIST_BOX_ROW(child);
        PumpkinServer *row_server = g_object_get_data(G_OBJECT(candidate), "server");
        if (row_server == server) {
          row = candidate;
          break;
        }
        child = gtk_widget_get_next_sibling(child);
      }
      if (row != NULL) {
        gtk_list_box_select_row(self->server_list, row);
      }
    }
    self->settings_guard = FALSE;
    g_object_unref(server);
  }

  if (self->pending_details_page != NULL) {
    self->settings_guard = TRUE;
    adw_view_stack_set_visible_child_name(self->details_stack, self->pending_details_page);
    self->settings_guard = FALSE;
  }
  if (self->pending_view_page != NULL) {
    adw_view_stack_set_visible_child_name(self->view_stack, self->pending_view_page);
  }
}

static void
on_details_stack_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self)
{
  (void)object;
  (void)pspec;
  if (self->settings_guard || self->details_stack == NULL) {
    return;
  }
  const char *page = adw_view_stack_get_visible_child_name(self->details_stack);
  if (page == NULL) {
    return;
  }
  if (self->settings_dirty && g_strcmp0(page, "settings") != 0) {
    self->settings_guard = TRUE;
    adw_view_stack_set_visible_child_name(self->details_stack, "settings");
    self->settings_guard = FALSE;
    confirm_leave_settings(self, page, NULL);
  }
}

static void
download_progress_state_free(DownloadProgressState *state)
{
  g_free(state);
}

static DownloadProgressState *
get_download_progress_state(PumpkinWindow *self, PumpkinServer *server, gboolean create)
{
  if (self->download_progress_state == NULL || server == NULL) {
    return NULL;
  }
  DownloadProgressState *state = g_hash_table_lookup(self->download_progress_state, server);
  if (state == NULL && create) {
    state = g_new0(DownloadProgressState, 1);
    g_hash_table_insert(self->download_progress_state, g_object_ref(server), state);
  }
  return state;
}

static void
update_progress_bar(GtkWidget *bar, goffset current, goffset total)
{
  if (bar == NULL) {
    return;
  }
  if (total > 0) {
    double frac = (double)current / (double)total;
    if (frac < 0.0) {
      frac = 0.0;
    } else if (frac > 1.0) {
      frac = 1.0;
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), frac);
    g_autofree char *cur = g_format_size_full(current, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *tot = g_format_size_full(total, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *text = g_strdup_printf("%s / %s", cur, tot);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar), text);
  } else {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(bar));
    g_autofree char *cur = g_format_size_full(current, G_FORMAT_SIZE_IEC_UNITS);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar), cur);
  }
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
}

static void
set_download_progress_for_server(PumpkinWindow *self, PumpkinServer *server, goffset current, goffset total)
{
  if (self->current == NULL || self->current != server || self->download_progress == NULL) {
    return;
  }

  if (self->download_progress_revealer != NULL) {
    gtk_revealer_set_reveal_child(self->download_progress_revealer, TRUE);
  }

  update_progress_bar(GTK_WIDGET(self->download_progress), current, total);
}

static void
on_console_copy(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->log_view == NULL) {
    return;
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_view);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (text == NULL) {
    return;
  }
  GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(self)));
  gdk_clipboard_set_text(clipboard, text);
}

static void
on_console_clear(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->log_view == NULL) {
    return;
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_view);
  gtk_text_buffer_set_text(buffer, "", -1);
}

static void
on_download_progress(goffset current, goffset total, gpointer user_data)
{
  DownloadContext *ctx = user_data;
  if (ctx == NULL) {
    return;
  }
  DownloadProgressState *state = get_download_progress_state(ctx->self, ctx->server, TRUE);
  if (state != NULL) {
    state->current = current;
    state->total = total;
    state->active = TRUE;
    if (state->overview_bar != NULL) {
      update_progress_bar(state->overview_bar, current, total);
    }
  }
  set_download_progress_for_server(ctx->self, ctx->server, current, total);
}

static void
append_console_line(PumpkinWindow *self, PumpkinServer *server, const char *line)
{
  if (self->log_view == NULL || server == NULL || line == NULL) {
    return;
  }

  GtkTextBuffer *buffer = g_hash_table_lookup(self->console_buffers, server);
  if (buffer == NULL) {
    buffer = gtk_text_buffer_new(NULL);
    g_hash_table_insert(self->console_buffers, g_object_ref(server), buffer);
  }

  if (self->current == server && gtk_text_view_get_buffer(self->log_view) != buffer) {
    gtk_text_view_set_buffer(self->log_view, buffer);
  }

  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_insert(buffer, &end, line, -1);
  gtk_text_buffer_insert(buffer, &end, "\n", -1);
  GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "log-end");
  if (mark == NULL) {
    mark = gtk_text_buffer_create_mark(buffer, "log-end", &end, FALSE);
  } else {
    gtk_text_buffer_move_mark(buffer, mark, &end);
  }
  gtk_text_view_scroll_to_mark(self->log_view, mark, 0.0, TRUE, 0.0, 1.0);
}

static void
on_log_line(PumpkinServer *server, const char *line, PumpkinWindow *self)
{
  append_console_line(self, server, line);
  update_live_player_names(self, line);
  if (line != NULL && g_strcmp0(line, "Server process exited") == 0) {
    if (self->current == server && !self->restart_requested) {
      self->ui_state = UI_STATE_IDLE;
      update_details(self);
      refresh_overview_list(self);
    }
  }
}

static void
append_log(PumpkinWindow *self, const char *line)
{
  if (self->current != NULL) {
    append_console_line(self, self->current, line);
    return;
  }

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_view);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_insert(buffer, &end, line, -1);
  gtk_text_buffer_insert(buffer, &end, "\n", -1);
}

static void
append_log_for_server(PumpkinWindow *self, PumpkinServer *server, const char *line)
{
  if (server != NULL) {
    append_console_line(self, server, line);
    return;
  }
  append_log(self, line);
}

static void
clear_list_box(GtkListBox *box)
{
  if (box == NULL) {
    return;
  }
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(box));
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(box, child);
    child = next;
  }
}

static gboolean
try_mtime(const char *path, time_t *out_mtime)
{
  GStatBuf st;
  if (path == NULL) {
    return FALSE;
  }
  if (g_stat(path, &st) != 0) {
    return FALSE;
  }
  *out_mtime = st.st_mtime;
  return TRUE;
}

static time_t
player_last_seen_mtime(const char *players_dir, const char *world_players, const char *entry, const char *token)
{
  time_t latest = 0;
  const char *candidates[6] = {0};
  g_autofree char *token_dat = NULL;
  g_autofree char *entry_path_players = NULL;
  g_autofree char *entry_path_world = NULL;
  g_autofree char *token_path_players = NULL;
  g_autofree char *token_path_world = NULL;
  g_autofree char *token_dat_players = NULL;
  g_autofree char *token_dat_world = NULL;

  if (token != NULL && *token != '\0') {
    token_dat = g_strconcat(token, ".dat", NULL);
  }

  if (players_dir != NULL) {
    if (entry != NULL && *entry != '\0') {
      entry_path_players = g_build_filename(players_dir, entry, NULL);
    }
    if (token != NULL && *token != '\0') {
      token_path_players = g_build_filename(players_dir, token, NULL);
    }
    if (token_dat != NULL) {
      token_dat_players = g_build_filename(players_dir, token_dat, NULL);
    }
  }
  if (world_players != NULL) {
    if (entry != NULL && *entry != '\0') {
      entry_path_world = g_build_filename(world_players, entry, NULL);
    }
    if (token != NULL && *token != '\0') {
      token_path_world = g_build_filename(world_players, token, NULL);
    }
    if (token_dat != NULL) {
      token_dat_world = g_build_filename(world_players, token_dat, NULL);
    }
  }

  candidates[0] = entry_path_players;
  candidates[1] = entry_path_world;
  candidates[2] = token_path_players;
  candidates[3] = token_path_world;
  candidates[4] = token_dat_players;
  candidates[5] = token_dat_world;

  for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
    time_t mtime = 0;
    if (try_mtime(candidates[i], &mtime)) {
      if (mtime > latest) {
        latest = mtime;
      }
    }
  }

  return latest;
}

static char *
format_last_seen(time_t mtime)
{
  if (mtime <= 0) {
    return g_strdup("Unbekannt");
  }
  time_t now = time(NULL);
  if (now < mtime) {
    now = mtime;
  }
  guint64 diff = (guint64)(now - mtime);

  if (diff < 60) {
    guint64 n = diff;
    return g_strdup_printf("vor %" G_GUINT64_FORMAT " Sekunde%s", n, n == 1 ? "" : "n");
  }
  if (diff < 3600) {
    guint64 n = diff / 60;
    return g_strdup_printf("vor %" G_GUINT64_FORMAT " Minute%s", n, n == 1 ? "" : "n");
  }
  if (diff < 86400) {
    guint64 n = diff / 3600;
    return g_strdup_printf("vor %" G_GUINT64_FORMAT " Stunde%s", n, n == 1 ? "" : "n");
  }
  if (diff < 31536000) {
    guint64 n = diff / 86400;
    return g_strdup_printf("vor %" G_GUINT64_FORMAT " Tag%s", n, n == 1 ? "" : "en");
  }
  guint64 n = diff / 31536000;
  return g_strdup_printf("vor %" G_GUINT64_FORMAT " Jahr%s", n, n == 1 ? "" : "en");
}

static char *
get_server_version(PumpkinServer *server)
{
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  if (!g_file_test(bin, G_FILE_TEST_EXISTS)) {
    return g_strdup("Not installed");
  }

  return g_strdup("Installed");
}

static guint64
dir_size_bytes(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir == NULL) {
    return 0;
  }

  guint64 total = 0;
  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child = g_build_filename(path, entry, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
      total += dir_size_bytes(child);
    } else {
      GStatBuf st;
      if (g_stat(child, &st) == 0) {
        total += (guint64)st.st_size;
      }
    }
  }
  g_dir_close(dir);
  return total;
}

static char *
get_server_size(PumpkinServer *server)
{
  const char *root = pumpkin_server_get_root_dir(server);
  if (root == NULL) {
    return g_strdup("Unknown");
  }
  guint64 bytes = dir_size_bytes(root);
  return g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS);
}

static int
get_player_count(PumpkinServer *server)
{
  g_autofree char *players_dir = pumpkin_server_get_players_dir(server);
  GDir *dir = g_dir_open(players_dir, 0, NULL);
  if (dir == NULL) {
    return 0;
  }

  int count = 0;
  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child = g_build_filename(players_dir, entry, NULL);
    if (!g_file_test(child, G_FILE_TEST_IS_DIR)) {
      count++;
    }
  }
  g_dir_close(dir);
  return count;
}

static gboolean
server_name_exists(PumpkinWindow *self, const char *name)
{
  if (name == NULL || *name == '\0') {
    return TRUE;
  }

  guint n = g_list_model_get_n_items(pumpkin_server_store_get_model(self->store));
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(pumpkin_server_store_get_model(self->store), i);
    const char *existing = pumpkin_server_get_name(server);
    gboolean match = (g_ascii_strcasecmp(existing, name) == 0);
    g_object_unref(server);
    if (match) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_uuid_string(const char *text)
{
  if (text == NULL) {
    return FALSE;
  }
  int len = (int)strlen(text);
  if (len != 36) {
    return FALSE;
  }
  for (int i = 0; i < len; i++) {
    char c = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return FALSE;
      }
      continue;
    }
    if (!g_ascii_isxdigit(c)) {
      return FALSE;
    }
  }
  return TRUE;
}

static void
add_name_uuid_pairs(GHashTable *map, const char *contents)
{
  if (contents == NULL) {
    return;
  }

  g_autoptr(GRegex) regex = g_regex_new(
    "\"uuid\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"name\"\\s*:\\s*\"([^\"]+)\"|\"name\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"uuid\"\\s*:\\s*\"([^\"]+)\"",
    G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, contents, 0, &match)) {
    return;
  }

  while (g_match_info_matches(match)) {
    g_autofree char *uuid1 = g_match_info_fetch(match, 1);
    g_autofree char *name1 = g_match_info_fetch(match, 2);
    g_autofree char *name2 = g_match_info_fetch(match, 3);
    g_autofree char *uuid2 = g_match_info_fetch(match, 4);

    if (uuid1 != NULL && name1 != NULL) {
      g_hash_table_replace(map, g_strdup(uuid1), g_strdup(name1));
    } else if (uuid2 != NULL && name2 != NULL) {
      g_hash_table_replace(map, g_strdup(uuid2), g_strdup(name2));
    }

    g_match_info_next(match, NULL);
  }
}

static void
load_player_name_map(GHashTable *map, PumpkinServer *server)
{
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  const char *files[] = {
    "usercache.json",
    "whitelist.json",
    "ops.json",
    "banned-players.json",
    NULL
  };

  for (int i = 0; files[i] != NULL; i++) {
    g_autofree char *path = g_build_filename(data_dir, "data", files[i], NULL);
    g_autofree char *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
      add_name_uuid_pairs(map, contents);
    }
  }
}

static gboolean
try_create_server(PumpkinWindow *self, const char *name)
{
  g_autofree char *trimmed = g_strdup(name ? name : "");
  g_strstrip(trimmed);
  if (trimmed[0] == '\0') {
    append_log(self, "Server name cannot be empty.");
    return FALSE;
  }
  if (server_name_exists(self, trimmed)) {
    append_log(self, "Server name already exists.");
    return FALSE;
  }

  g_autoptr(GError) error = NULL;
  PumpkinServer *server = pumpkin_server_store_add(self->store, trimmed, &error);
  if (server == NULL) {
    append_log(self, error->message);
    return FALSE;
  }

  if (self->config != NULL) {
    const char *url = pumpkin_config_get_default_download_url(self->config);
    if (url != NULL && *url != '\0') {
      pumpkin_server_set_download_url(server, url);
      pumpkin_server_save(server, NULL);
    }
  }

  GtkWidget *row = create_server_row(server);
  gtk_list_box_append(self->server_list, row);
  gtk_list_box_select_row(self->server_list, GTK_LIST_BOX_ROW(row));
  refresh_overview_list(self);
  g_object_unref(server);
  return TRUE;
}

static void
on_overview_update_clicked(GtkButton *button, gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  PumpkinServer *server = g_object_get_data(G_OBJECT(button), "server");
  if (server == NULL || self->latest_url == NULL) {
    return;
  }

  start_download_for_server(self, server, self->latest_url);
}

static void
on_overview_settings_clicked(GtkButton *button, gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  PumpkinServer *server = g_object_get_data(G_OBJECT(button), "server");
  if (server == NULL) {
    return;
  }

  select_server(self, server);
  adw_view_stack_set_visible_child_name(self->view_stack, "details");
}

static void
on_overview_install_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  PumpkinServer *server = g_object_get_data(G_OBJECT(dialog), "server");
  PumpkinWindow *self = g_object_get_data(G_OBJECT(dialog), "window");
  if (server == NULL || self == NULL) {
    return;
  }

  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
  if (g_strcmp0(response, "overwrite") == 0) {
    const char *url = self->latest_url != NULL ? self->latest_url : pumpkin_server_get_download_url(server);
    start_download_for_server(self, server, url);
  }
}

static void
refresh_overview_list(PumpkinWindow *self)
{
  if (self->overview_list == NULL) {
    return;
  }

  clear_list_box(self->overview_list);

  guint n = g_list_model_get_n_items(pumpkin_server_store_get_model(self->store));
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(pumpkin_server_store_get_model(self->store), i);

    g_autofree char *version = get_server_version(server);
    g_autofree char *size = get_server_size(server);
    int players = get_player_count(server);
    gboolean installed = g_strcmp0(version, "Not installed") != 0;
    const char *installed_url = pumpkin_server_get_installed_url(server);
    gboolean update_available = (installed && self->latest_url != NULL &&
                                 (installed_url == NULL || g_strcmp0(self->latest_url, installed_url) != 0));

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *card = gtk_frame_new(NULL);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, "overview-card");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    GtkWidget *icon = create_server_icon_widget(server);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title = gtk_label_new(pumpkin_server_get_name(server));
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_add_css_class(title, "title-4");

    g_autofree char *subtitle = g_strdup_printf("Version: %s · Players: %d · Size: %s",
                                                 version,
                                                 players,
                                                 size);
    GtkWidget *sub = gtk_label_new(subtitle);
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0);

    gtk_box_append(GTK_BOX(vbox), title);
    gtk_box_append(GTK_BOX(vbox), sub);
    gtk_widget_set_hexpand(vbox, TRUE);

    DownloadProgressState *state = get_download_progress_state(self, server, FALSE);
    if (state != NULL && state->active) {
      GtkWidget *bar = gtk_progress_bar_new();
      gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
      update_progress_bar(bar, state->current, state->total);
      gtk_widget_set_hexpand(bar, TRUE);
      gtk_box_append(GTK_BOX(vbox), bar);
      state->overview_bar = bar;
      g_object_add_weak_pointer(G_OBJECT(bar), (gpointer *)&state->overview_bar);
    }

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    if (update_available) {
      GtkWidget *btn_update = gtk_button_new_with_label("Update");
      gtk_widget_add_css_class(btn_update, "card-button");
      gtk_widget_add_css_class(btn_update, "update-button");
      g_object_set_data_full(G_OBJECT(btn_update), "server", g_object_ref(server), g_object_unref);
      g_signal_connect(btn_update, "clicked", G_CALLBACK(on_overview_update_clicked), self);
      gtk_box_append(GTK_BOX(btn_box), btn_update);
    }

    GtkWidget *btn_settings = gtk_button_new_with_label("Details");
    gtk_widget_add_css_class(btn_settings, "card-button");
    g_object_set_data_full(G_OBJECT(btn_settings), "server", g_object_ref(server), g_object_unref);
    g_signal_connect(btn_settings, "clicked", G_CALLBACK(on_overview_settings_clicked), self);
    gtk_box_append(GTK_BOX(btn_box), btn_settings);

    GtkWidget *btn_remove = gtk_button_new_with_label("Remove");
    gtk_widget_add_css_class(btn_remove, "card-button");
    gtk_widget_add_css_class(btn_remove, "destructive-action");
    g_object_set_data_full(G_OBJECT(btn_remove), "server", g_object_ref(server), g_object_unref);
    g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_overview_remove_clicked), self);
    gtk_box_append(GTK_BOX(btn_box), btn_remove);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), vbox);
    gtk_box_append(GTK_BOX(box), btn_box);

    gtk_frame_set_child(GTK_FRAME(card), box);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    g_object_set_data_full(G_OBJECT(row), "server", g_object_ref(server), g_object_unref);
    gtk_list_box_append(self->overview_list, row);

    g_object_unref(server);
  }
}

static gboolean
on_plugin_toggle_state(GtkSwitch *sw, gboolean state, gpointer user_data)
{
  (void)user_data;
  const char *file = g_object_get_data(G_OBJECT(sw), "plugin-file");
  const char *dir_path = g_object_get_data(G_OBJECT(sw), "plugin-dir");

  if (file == NULL || dir_path == NULL) {
    return FALSE;
  }

  g_autofree char *from = g_build_filename(dir_path, file, NULL);
  g_autofree char *to = NULL;

  if (state) {
    if (g_str_has_suffix(file, ".disabled")) {
      g_autofree char *base = g_strndup(file, strlen(file) - strlen(".disabled"));
      to = g_build_filename(dir_path, base, NULL);
    }
  } else {
    if (!g_str_has_suffix(file, ".disabled")) {
      g_autofree char *disabled = g_strconcat(file, ".disabled", NULL);
      to = g_build_filename(dir_path, disabled, NULL);
    }
  }

  if (to != NULL) {
    g_rename(from, to);
  }

  return FALSE;
}

static char *
version_from_filename(const char *name)
{
  if (name == NULL) {
    return NULL;
  }

  const char *base = name;
  g_autofree char *cleaned = NULL;
  if (g_str_has_suffix(base, ".disabled")) {
    cleaned = g_strndup(base, strlen(base) - strlen(".disabled"));
    base = cleaned;
  }

  const char *dash = strrchr(base, '-');
  if (dash == NULL || dash[1] == '\0') {
    return NULL;
  }

  const char *ver = dash + 1;
  gboolean has_digit = FALSE;
  for (const char *p = ver; *p != '\0'; p++) {
    if (g_ascii_isdigit(*p)) {
      has_digit = TRUE;
      continue;
    }
    if (*p == '.' || *p == '_' || *p == 'v') {
      continue;
    }
    return NULL;
  }

  if (!has_digit) {
    return NULL;
  }

  return g_strdup(ver);
}

static char *
version_from_sidecar(const char *plugins_dir, const char *file_name)
{
  if (plugins_dir == NULL || file_name == NULL) {
    return NULL;
  }

  g_autofree char *clean = g_strdup(file_name);
  if (g_str_has_suffix(clean, ".disabled")) {
    clean[strlen(clean) - strlen(".disabled")] = '\0';
  }

  g_autofree char *toml = g_strconcat(clean, ".toml", NULL);
  g_autofree char *path = g_build_filename(plugins_dir, toml, NULL);
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_clear_pointer(&path, g_free);
    path = g_build_filename(plugins_dir, "plugin.toml", NULL);
  }

  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    return NULL;
  }

  g_autofree char *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL)) {
    return NULL;
  }

  char *pos = strstr(contents, "version");
  if (pos == NULL) {
    return NULL;
  }
  char *quote = strchr(pos, '"');
  if (quote == NULL) {
    return NULL;
  }
  char *end = strchr(quote + 1, '"');
  if (end == NULL) {
    return NULL;
  }

  return g_strndup(quote + 1, end - quote - 1);
}

static char *
plugin_version_label(const char *plugins_dir, const char *file_name)
{
  g_autofree char *sidecar = version_from_sidecar(plugins_dir, file_name);
  if (sidecar != NULL) {
    return sidecar;
  }
  return version_from_filename(file_name);
}

static void
update_overview(PumpkinWindow *self)
{
  refresh_overview_list(self);
}

static gboolean
use_download_cache(PumpkinWindow *self)
{
  if (self->config == NULL) {
    return FALSE;
  }
  return pumpkin_config_get_use_cache(self->config);
}

static char *
cache_dir_for_config(PumpkinWindow *self)
{
  const char *base = NULL;
  if (self->config != NULL) {
    base = pumpkin_config_get_base_dir(self->config);
  }
  if (base == NULL || *base == '\0') {
    base = g_get_home_dir();
  }
  return g_build_filename(base, ".pumpkin", "versions", NULL);
}

static char *
cache_path_for_url(PumpkinWindow *self, const char *url)
{
  if (url == NULL || *url == '\0') {
    return NULL;
  }
  g_autofree char *cache_dir = cache_dir_for_config(self);
  g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
  g_autofree char *version_dir = g_build_filename(cache_dir, hash, NULL);
  g_mkdir_with_parents(version_dir, 0755);
  return g_build_filename(version_dir, "pumpkin", NULL);
}

static gboolean
copy_binary(const char *src, const char *dest, GError **error)
{
  g_autoptr(GFile) src_file = g_file_new_for_path(src);
  g_autoptr(GFile) dest_file = g_file_new_for_path(dest);
  if (!g_file_copy(src_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error)) {
    return FALSE;
  }
  g_chmod(dest, 0755);
  return TRUE;
}

static void
set_download_busy(PumpkinWindow *self, gboolean busy)
{
  if (self->download_spinner == NULL) {
    return;
  }
  gtk_widget_set_visible(GTK_WIDGET(self->download_spinner), busy);
  if (busy) {
    gtk_spinner_start(self->download_spinner);
  } else {
    gtk_spinner_stop(self->download_spinner);
  }
  if (self->download_progress_revealer != NULL && !busy) {
    gtk_revealer_set_reveal_child(self->download_progress_revealer, FALSE);
  }
}

static void
update_details(PumpkinWindow *self)
{
  if (self->details_title == NULL) {
    return;
  }

  if (self->current == NULL) {
    gtk_label_set_text(self->details_title, "No server selected");
    if (self->details_server_icon != NULL) {
      gtk_image_set_from_icon_name(self->details_server_icon, "network-server-symbolic");
    }
    set_details_error(self, NULL);
    set_details_status(self, NULL, 0);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_start), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_stop), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_restart), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_install), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_update), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_details_update), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_check_updates), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_send_command), FALSE);
    return;
  }

  gboolean running = pumpkin_server_get_running(self->current);
  g_autofree char *bin = pumpkin_server_get_bin_path(self->current);
  gboolean installed = g_file_test(bin, G_FILE_TEST_EXISTS);
  const char *installed_url = pumpkin_server_get_installed_url(self->current);
  gboolean update_available = (installed && self->latest_url != NULL &&
                               (installed_url == NULL || g_strcmp0(self->latest_url, installed_url) != 0));

  if (running && (self->ui_state == UI_STATE_IDLE || self->ui_state == UI_STATE_ERROR)) {
    self->ui_state = UI_STATE_RUNNING;
  } else if (!running && (self->ui_state == UI_STATE_RUNNING || self->ui_state == UI_STATE_STOPPING) &&
             !self->restart_requested) {
    self->ui_state = UI_STATE_IDLE;
  }

  gtk_label_set_text(self->details_title, pumpkin_server_get_name(self->current));
  if (self->details_server_icon != NULL) {
    g_autofree char *data_dir = pumpkin_server_get_data_dir(self->current);
    g_autofree char *icon_path = g_build_filename(data_dir, "server-icon.png", NULL);
    if (g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
      g_autoptr(GError) error = NULL;
      g_autoptr(GdkTexture) texture = NULL;
      g_autoptr(GFile) file = g_file_new_for_path(icon_path);
      texture = gdk_texture_new_from_file(file, &error);
      if (texture != NULL) {
        gtk_image_set_from_paintable(self->details_server_icon, GDK_PAINTABLE(texture));
        gtk_image_set_pixel_size(self->details_server_icon, 32);
      } else {
        gtk_image_set_from_icon_name(self->details_server_icon, "network-server-symbolic");
        gtk_image_set_pixel_size(self->details_server_icon, 32);
      }
    } else {
      gtk_image_set_from_icon_name(self->details_server_icon, "network-server-symbolic");
      gtk_image_set_pixel_size(self->details_server_icon, 32);
    }
  }
  set_details_error(self, NULL);

  gboolean busy = (self->ui_state == UI_STATE_STARTING ||
                   self->ui_state == UI_STATE_STOPPING ||
                   self->ui_state == UI_STATE_RESTARTING);
  gboolean can_start = installed && !running && !busy && !self->action_cooldown &&
                       (self->ui_state == UI_STATE_IDLE || self->ui_state == UI_STATE_ERROR);
  gboolean can_stop = running && self->ui_state == UI_STATE_RUNNING && !self->action_cooldown;
  gboolean can_restart = running && self->ui_state == UI_STATE_RUNNING &&
                         self->restart_delay_id == 0 && !self->action_cooldown;

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_start), can_start);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_stop), can_stop);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_restart), can_restart);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_install), !running && !busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_update), update_available && !running && !busy);
  gtk_widget_set_visible(GTK_WIDGET(self->btn_details_update), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_check_updates), !running && !busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_send_command), running && self->ui_state == UI_STATE_RUNNING);
  if (self->btn_console_copy != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_console_copy), self->current != NULL);
  }
  if (self->btn_console_clear != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_console_clear), self->current != NULL);
  }

  if (running) {
    if (self->players_refresh_id == 0) {
      self->players_refresh_id = g_timeout_add_seconds(5, refresh_players_tick, self);
    }
  } else if (self->players_refresh_id != 0) {
    g_source_remove(self->players_refresh_id);
    self->players_refresh_id = 0;
  }
}

static void
set_action_cooldown(PumpkinWindow *self, guint seconds)
{
  if (self->action_cooldown_id != 0) {
    g_source_remove(self->action_cooldown_id);
    self->action_cooldown_id = 0;
  }
  self->action_cooldown = TRUE;
  self->action_cooldown_id = g_timeout_add_seconds(seconds, action_cooldown_cb, g_object_ref(self));
}

static gboolean
action_cooldown_cb(gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  self->action_cooldown_id = 0;
  self->action_cooldown = FALSE;
  update_details(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static gboolean
restart_after_delay(gpointer data)
{
  RestartContext *ctx = data;
  PumpkinWindow *self = ctx->self;
  PumpkinServer *server = ctx->server;
  self->restart_delay_id = 0;
  self->restart_requested = FALSE;
  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_start(server, &error)) {
    append_log(self, error->message);
    if (self->current == server) {
      set_details_error(self, error->message);
      self->ui_state = UI_STATE_ERROR;
    }
  } else if (self->current == server) {
    self->ui_state = UI_STATE_RUNNING;
  }

  if (self->current == server) {
    update_details(self);
  }
  refresh_overview_list(self);
  if (self->current == server) {
    refresh_world_list(self);
    refresh_player_list(self);
    refresh_log_files(self);
  }

  g_object_unref(ctx->server);
  g_object_unref(ctx->self);
  g_free(ctx);
  return G_SOURCE_REMOVE;
}

static gboolean
start_after_delay(gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  self->start_delay_id = 0;
  if (self->current != NULL && pumpkin_server_get_running(self->current) &&
      self->ui_state == UI_STATE_STARTING) {
    self->ui_state = UI_STATE_RUNNING;
  }
  update_details(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static gboolean
read_system_cpu(unsigned long long *total, unsigned long long *idle)
{
  g_autofree char *contents = NULL;
  if (!g_file_get_contents("/proc/stat", &contents, NULL, NULL)) {
    return FALSE;
  }

  char *line_end = strchr(contents, '\n');
  if (line_end != NULL) {
    *line_end = '\0';
  }

  unsigned long long user = 0, nice = 0, system = 0, idle_time = 0, iowait = 0;
  unsigned long long irq = 0, softirq = 0, steal = 0;
  if (sscanf(contents, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
             &user, &nice, &system, &idle_time, &iowait, &irq, &softirq, &steal) < 4) {
    return FALSE;
  }

  *idle = idle_time + iowait;
  *total = user + nice + system + idle_time + iowait + irq + softirq + steal;
  return TRUE;
}

static gboolean
read_system_mem(unsigned long long *total_bytes, unsigned long long *avail_bytes)
{
  g_autofree char *contents = NULL;
  if (!g_file_get_contents("/proc/meminfo", &contents, NULL, NULL)) {
    return FALSE;
  }

  unsigned long long total_kb = 0;
  unsigned long long avail_kb = 0;
  char *line = contents;
  while (line != NULL && *line != '\0') {
    char *next = strchr(line, '\n');
    if (next != NULL) {
      *next = '\0';
    }
    if (g_str_has_prefix(line, "MemTotal:")) {
      sscanf(line, "MemTotal: %llu kB", &total_kb);
    } else if (g_str_has_prefix(line, "MemAvailable:")) {
      sscanf(line, "MemAvailable: %llu kB", &avail_kb);
    }
    if (next == NULL) {
      break;
    }
    line = next + 1;
  }

  if (total_kb == 0) {
    return FALSE;
  }
  *total_bytes = total_kb * 1024ULL;
  *avail_bytes = avail_kb * 1024ULL;
  return TRUE;
}

static gboolean
read_process_stats(int pid, unsigned long long *proc_ticks, unsigned long long *rss_bytes)
{
  if (pid <= 0) {
    return FALSE;
  }

  g_autofree char *stat_path = g_strdup_printf("/proc/%d/stat", pid);
  g_autofree char *contents = NULL;
  if (!g_file_get_contents(stat_path, &contents, NULL, NULL)) {
    return FALSE;
  }

  char *paren = strrchr(contents, ')');
  if (paren == NULL) {
    return FALSE;
  }
  char *p = paren + 1;
  while (*p == ' ') {
    p++;
  }

  unsigned long long utime = 0;
  unsigned long long stime = 0;
  int field = 3;
  char *saveptr = NULL;
  char *token = strtok_r(p, " ", &saveptr);
  while (token != NULL) {
    if (field == 14) {
      utime = g_ascii_strtoull(token, NULL, 10);
    } else if (field == 15) {
      stime = g_ascii_strtoull(token, NULL, 10);
      break;
    }
    field++;
    token = strtok_r(NULL, " ", &saveptr);
  }
  *proc_ticks = utime + stime;

  g_autofree char *status_path = g_strdup_printf("/proc/%d/status", pid);
  g_autofree char *status = NULL;
  if (g_file_get_contents(status_path, &status, NULL, NULL)) {
    char *line = status;
    while (line != NULL && *line != '\0') {
      char *next = strchr(line, '\n');
      if (next != NULL) {
        *next = '\0';
      }
      if (g_str_has_prefix(line, "VmRSS:")) {
        unsigned long long kb = 0;
        sscanf(line, "VmRSS: %llu kB", &kb);
        *rss_bytes = kb * 1024ULL;
        break;
      }
      if (next == NULL) {
        break;
      }
      line = next + 1;
    }
  }

  return TRUE;
}

static gboolean
update_stats_tick(gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  if (self->stats_row != NULL) {
    gboolean running = (self->current != NULL && pumpkin_server_get_running(self->current));
    gtk_widget_set_visible(GTK_WIDGET(self->stats_row), running);
  }
  if (self->label_sys_cpu == NULL || self->label_sys_ram == NULL ||
      self->label_srv_cpu == NULL || self->label_srv_ram == NULL) {
    return G_SOURCE_CONTINUE;
  }

  unsigned long long total = 0, idle = 0;
  unsigned long long mem_total = 0, mem_avail = 0;
  double sys_cpu = 0.0;
  unsigned long long prev_total = self->last_total_jiffies;
  unsigned long long prev_idle = self->last_idle_jiffies;

  if (read_system_cpu(&total, &idle)) {
    if (prev_total != 0) {
      unsigned long long delta_total = total - prev_total;
      unsigned long long delta_idle = idle - prev_idle;
      if (delta_total > 0) {
        sys_cpu = (double)(delta_total - delta_idle) / (double)delta_total * 100.0;
      }
    }
    self->last_total_jiffies = total;
    self->last_idle_jiffies = idle;
  }

  if (read_system_mem(&mem_total, &mem_avail)) {
    unsigned long long used = mem_total - mem_avail;
    g_autofree char *used_str = g_format_size_full(used, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *total_str = g_format_size_full(mem_total, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *ram = g_strdup_printf("System RAM: %s / %s", used_str, total_str);
    gtk_label_set_text(self->label_sys_ram, ram);
  }

  g_autofree char *cpu = g_strdup_printf("System CPU: %.1f%%", sys_cpu);
  gtk_label_set_text(self->label_sys_cpu, cpu);

  double proc_cpu = 0.0;
  unsigned long long proc_ticks = 0;
  unsigned long long rss = 0;
  int pid = 0;
  if (self->current != NULL) {
    pid = pumpkin_server_get_pid(self->current);
  }

  if (pid > 0 && read_process_stats(pid, &proc_ticks, &rss) && prev_total != 0) {
    if (self->last_proc_jiffies != 0 && total > prev_total) {
      unsigned long long delta_proc = proc_ticks - self->last_proc_jiffies;
      unsigned long long delta_total = total - prev_total;
      if (delta_total > 0) {
        proc_cpu = (double)delta_proc / (double)delta_total * 100.0;
      }
    }
    self->last_proc_jiffies = proc_ticks;
  } else {
    self->last_proc_jiffies = 0;
  }

  if (pid > 0 && rss > 0) {
    g_autofree char *rss_str = g_format_size_full(rss, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *ram = g_strdup_printf("Pumpkin RAM: %s", rss_str);
    gtk_label_set_text(self->label_srv_ram, ram);
  } else {
    gtk_label_set_text(self->label_srv_ram, "Pumpkin RAM: --");
  }

  if (pid > 0) {
    g_autofree char *pcpu = g_strdup_printf("Pumpkin CPU: %.1f%%", proc_cpu);
    gtk_label_set_text(self->label_srv_cpu, pcpu);
  } else {
    gtk_label_set_text(self->label_srv_cpu, "Pumpkin CPU: --");
  }

  return G_SOURCE_CONTINUE;
}

static GtkWidget *
create_server_row(PumpkinServer *server)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *label = gtk_label_new(pumpkin_server_get_name(server));
  GtkWidget *status = gtk_label_new(pumpkin_server_get_running(server) ? "Running" : "Stopped");

  gtk_widget_set_hexpand(label, TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_xalign(GTK_LABEL(status), 1.0);

  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), status);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

  g_object_set_data_full(G_OBJECT(row), "server", g_object_ref(server), g_object_unref);
  return row;
}

static void
load_server_list(PumpkinWindow *self)
{
  guint n = g_list_model_get_n_items(pumpkin_server_store_get_model(self->store));
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(pumpkin_server_store_get_model(self->store), i);
    GtkWidget *row = create_server_row(server);
    gtk_list_box_append(self->server_list, row);
    g_object_unref(server);
  }

  GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->server_list, 0);
  if (first != NULL) {
    gtk_list_box_select_row(self->server_list, first);
  }
  refresh_overview_list(self);
}

static void
refresh_plugin_list(PumpkinWindow *self)
{
  clear_list_box(self->plugin_list);
  if (self->current == NULL) {
    return;
  }

  g_autofree char *plugins_dir = pumpkin_server_get_plugins_dir(self->current);
  GDir *dir = g_dir_open(plugins_dir, 0, NULL);
  if (dir == NULL) {
    return;
  }

  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    gboolean disabled = g_str_has_suffix(entry, ".disabled");

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(entry);
    GtkWidget *toggle = gtk_switch_new();

    gtk_switch_set_active(GTK_SWITCH(toggle), !disabled);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);

    gtk_box_append(GTK_BOX(box), label);
    g_autofree char *version = plugin_version_label(plugins_dir, entry);
    if (version != NULL) {
      GtkWidget *ver_label = gtk_label_new(version);
      gtk_widget_add_css_class(ver_label, "dim-label");
      gtk_box_append(GTK_BOX(box), ver_label);
    }
    gtk_box_append(GTK_BOX(box), toggle);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    g_object_set_data_full(G_OBJECT(row), "plugin-file", g_strdup(entry), g_free);
    g_object_set_data_full(G_OBJECT(toggle), "plugin-file", g_strdup(entry), g_free);
    g_object_set_data_full(G_OBJECT(toggle), "plugin-dir", g_strdup(plugins_dir), g_free);
    g_signal_connect(toggle, "state-set", G_CALLBACK(on_plugin_toggle_state), NULL);
    gtk_list_box_append(self->plugin_list, row);
  }
  g_dir_close(dir);
}

static void
refresh_world_list(PumpkinWindow *self)
{
  clear_list_box(self->world_list);
  if (self->current == NULL) {
    return;
  }

  gboolean running = pumpkin_server_get_running(self->current);
  g_autofree char *worlds_dir = pumpkin_server_get_worlds_dir(self->current);
  GDir *dir = g_dir_open(worlds_dir, 0, NULL);
  if (dir != NULL) {
    const char *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(worlds_dir, entry, NULL);
      if (!g_file_test(child, G_FILE_TEST_IS_DIR)) {
        continue;
      }

      GtkWidget *row = gtk_list_box_row_new();
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *label = gtk_label_new(entry);
      gtk_label_set_xalign(GTK_LABEL(label), 0.0);
      gtk_widget_set_hexpand(label, TRUE);

      guint64 size_bytes = dir_size_bytes(child);
      g_autofree char *size = g_format_size_full(size_bytes, G_FORMAT_SIZE_IEC_UNITS);
      GtkWidget *size_label = gtk_label_new(size);
      gtk_label_set_xalign(GTK_LABEL(size_label), 1.0);
      gtk_widget_add_css_class(size_label, "dim-label");

      gtk_box_append(GTK_BOX(box), label);
      gtk_box_append(GTK_BOX(box), size_label);

      if (!running) {
        GtkWidget *btn_delete = gtk_button_new_with_label("Delete");
        gtk_widget_add_css_class(btn_delete, "destructive-action");
        g_object_set_data_full(G_OBJECT(btn_delete), "world-path", g_strdup(child), g_free);
        g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_world_delete_clicked), self);
        gtk_box_append(GTK_BOX(box), btn_delete);
      }

      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
      gtk_list_box_append(self->world_list, row);
    }
    g_dir_close(dir);
  }

  g_autofree char *default_world = g_build_filename(pumpkin_server_get_data_dir(self->current), "world", NULL);
  if (g_file_test(default_world, G_FILE_TEST_IS_DIR)) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("world");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);

    guint64 size_bytes = dir_size_bytes(default_world);
    g_autofree char *size = g_format_size_full(size_bytes, G_FORMAT_SIZE_IEC_UNITS);
    GtkWidget *size_label = gtk_label_new(size);
    gtk_label_set_xalign(GTK_LABEL(size_label), 1.0);
    gtk_widget_add_css_class(size_label, "dim-label");

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), size_label);

    if (!running) {
      GtkWidget *btn_delete = gtk_button_new_with_label("Delete");
      gtk_widget_add_css_class(btn_delete, "destructive-action");
      g_object_set_data_full(G_OBJECT(btn_delete), "world-path", g_strdup(default_world), g_free);
      g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_world_delete_clicked), self);
      gtk_box_append(GTK_BOX(box), btn_delete);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(self->world_list, row);
  }
}

static void
refresh_player_list(PumpkinWindow *self)
{
  clear_list_box(self->player_list);
  if (self->current == NULL) {
    return;
  }

  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) name_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  load_player_name_map(name_map, self->current);
  if (self->live_player_names != NULL) {
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->live_player_names);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      const char *k = key;
      const char *v = value;
      g_hash_table_replace(name_map, g_strdup(k), g_strdup(v));
      if (is_uuid_string(k)) {
        g_hash_table_add(seen, g_strdup(k));
      } else {
        g_hash_table_add(seen, g_strdup(v));
      }
    }
  }

  g_autofree char *players_dir = pumpkin_server_get_players_dir(self->current);
  GDir *dir = g_dir_open(players_dir, 0, NULL);
  if (dir != NULL) {
    const char *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(players_dir, entry, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
        continue;
      }
      g_hash_table_add(seen, g_strdup(entry));
    }
    g_dir_close(dir);
  }

  g_autofree char *world_players = g_build_filename(pumpkin_server_get_data_dir(self->current), "world", "playerdata", NULL);
  dir = g_dir_open(world_players, 0, NULL);
  if (dir != NULL) {
    const char *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(world_players, entry, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
        continue;
      }
      g_hash_table_add(seen, g_strdup(entry));
    }
    g_dir_close(dir);
  }

  GHashTableIter iter;
  gpointer key = NULL;
  g_hash_table_iter_init(&iter, seen);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const char *entry = key;
    g_autofree char *token = g_strdup(entry);
    char *dot = strrchr(token, '.');
    if (dot != NULL) {
      *dot = '\0';
    }

    const char *display_name = token;
    const char *uuid = NULL;
    if (is_uuid_string(token)) {
      uuid = token;
      const char *mapped = g_hash_table_lookup(name_map, uuid);
      if (mapped != NULL) {
        display_name = mapped;
      }
    }

    time_t last_seen = player_last_seen_mtime(players_dir, world_players, entry, token);
    g_autofree char *last_seen_label = format_last_seen(last_seen);

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(display_name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget *seen_label = gtk_label_new(last_seen_label);
    gtk_label_set_xalign(GTK_LABEL(seen_label), 1.0);
    gtk_widget_add_css_class(seen_label, "dim-label");
    gtk_box_append(GTK_BOX(box), seen_label);

    if (uuid != NULL && g_strcmp0(display_name, uuid) != 0) {
      GtkWidget *uuid_label = gtk_label_new(uuid);
      gtk_label_set_xalign(GTK_LABEL(uuid_label), 1.0);
      gtk_widget_add_css_class(uuid_label, "dim-label");
      gtk_box_append(GTK_BOX(box), uuid_label);
    }

    g_object_set_data_full(G_OBJECT(row), "player-name", g_strdup(display_name), g_free);
    if (uuid != NULL) {
      g_object_set_data_full(G_OBJECT(row), "player-uuid", g_strdup(uuid), g_free);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(self->player_list, row);
  }
}

static gboolean
refresh_players_tick(gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  if (self->current == NULL || !pumpkin_server_get_running(self->current)) {
    self->players_refresh_id = 0;
    return G_SOURCE_REMOVE;
  }

  refresh_player_list(self);
  return G_SOURCE_CONTINUE;
}

static void
on_player_action_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
  if (response == NULL || g_strcmp0(response, "cancel") == 0) {
    return;
  }

  const char *name = g_object_get_data(G_OBJECT(dialog), "player-name");
  if (self->current == NULL || name == NULL || *name == '\0') {
    append_log(self, "Player name unknown; cannot run command.");
    return;
  }
  if (!pumpkin_server_get_running(self->current)) {
    append_log(self, "Server is not running.");
    return;
  }

  const char *cmd = NULL;
  if (g_strcmp0(response, "kick") == 0) {
    cmd = "kick";
  } else if (g_strcmp0(response, "ban") == 0) {
    cmd = "ban";
  } else if (g_strcmp0(response, "unban") == 0) {
    cmd = "pardon";
  } else if (g_strcmp0(response, "op") == 0) {
    cmd = "op";
  } else if (g_strcmp0(response, "deop") == 0) {
    cmd = "deop";
  }

  if (cmd == NULL) {
    return;
  }

  g_autofree char *command = g_strdup_printf("%s %s", cmd, name);
  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_send_command(self->current, command, &error)) {
    if (error != NULL) {
      append_log(self, error->message);
    }
  }
}

static void
on_player_row_activated(GtkListBox *box, GtkListBoxRow *row, PumpkinWindow *self)
{
  (void)box;
  if (row == NULL) {
    return;
  }

  const char *name = g_object_get_data(G_OBJECT(row), "player-name");
  const char *uuid = g_object_get_data(G_OBJECT(row), "player-uuid");

  const char *title = (name != NULL && *name != '\0') ? name : "Player";
  g_autofree char *body = NULL;
  if (uuid != NULL) {
    body = g_strdup_printf("UUID: %s", uuid);
  }

  AdwDialog *dialog = adw_alert_dialog_new(title, body);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_response(alert, "cancel", "Cancel");
  adw_alert_dialog_add_response(alert, "kick", "Kick");
  adw_alert_dialog_add_response(alert, "deop", "Deop");
  adw_alert_dialog_add_response(alert, "op", "Op");
  adw_alert_dialog_add_response(alert, "ban", "Ban");
  adw_alert_dialog_add_response(alert, "unban", "Unban");
  adw_alert_dialog_set_default_response(alert, "cancel");

  adw_alert_dialog_set_response_appearance(alert, "cancel", ADW_RESPONSE_DEFAULT);
  adw_alert_dialog_set_response_appearance(alert, "ban", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance(alert, "deop", ADW_RESPONSE_DESTRUCTIVE);
  adw_dialog_set_can_close(ADW_DIALOG(dialog), TRUE);

  if (self->current == NULL || !pumpkin_server_get_running(self->current)) {
    adw_alert_dialog_set_response_enabled(alert, "kick", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "ban", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "unban", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "op", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "deop", FALSE);
  }

  if (name != NULL) {
    g_object_set_data_full(G_OBJECT(dialog), "player-name", g_strdup(name), g_free);
  }

  adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_player_action_confirmed, self);
}

static void
update_settings_form(PumpkinWindow *self)
{
  if (self->entry_server_name == NULL || self->entry_download_url == NULL ||
      self->entry_server_port == NULL || self->entry_max_players == NULL ||
      self->entry_rcon_host == NULL || self->entry_rcon_port == NULL ||
      self->entry_rcon_password == NULL) {
    return;
  }
  self->settings_loading = TRUE;
  if (self->current == NULL) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_server_name), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_download_url), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_host), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_port), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_password), "");
    if (self->switch_auto_restart != NULL) {
      gtk_switch_set_active(self->switch_auto_restart, FALSE);
    }
    if (self->entry_auto_restart_delay != NULL) {
      gtk_editable_set_text(GTK_EDITABLE(self->entry_auto_restart_delay), "");
    }
    self->settings_loading = FALSE;
    self->settings_dirty = FALSE;
    return;
  }

  gtk_editable_set_text(GTK_EDITABLE(self->entry_server_name), pumpkin_server_get_name(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_download_url), pumpkin_server_get_download_url(self->current));

  g_autofree char *port = g_strdup_printf("%d", pumpkin_server_get_port(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_server_port), port);

  g_autofree char *max_players = g_strdup_printf("%d", pumpkin_server_get_max_players(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_max_players), max_players);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_host), pumpkin_server_get_rcon_host(self->current));

  if (self->switch_auto_restart != NULL) {
    gtk_switch_set_active(self->switch_auto_restart, pumpkin_server_get_auto_restart(self->current));
  }
  if (self->entry_auto_restart_delay != NULL) {
    g_autofree char *delay = g_strdup_printf("%d", pumpkin_server_get_auto_restart_delay(self->current));
    gtk_editable_set_text(GTK_EDITABLE(self->entry_auto_restart_delay), delay);
  }

  g_autofree char *rcon_port = g_strdup_printf("%d", pumpkin_server_get_rcon_port(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_port), rcon_port);

  const char *password = pumpkin_server_get_rcon_password(self->current);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_password), password ? password : "");
  self->settings_loading = FALSE;
  self->settings_dirty = FALSE;
}

static gboolean
write_scaled_icon_from_pixbuf(GdkPixbuf *src, const char *dest_path, gboolean overwrite)
{
  if (src == NULL || dest_path == NULL) {
    return FALSE;
  }
  if (!overwrite && g_file_test(dest_path, G_FILE_TEST_EXISTS)) {
    return TRUE;
  }

  g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_scale_simple(src, 64, 64, GDK_INTERP_BILINEAR);
  if (scaled == NULL) {
    return FALSE;
  }
  gdk_pixbuf_save(scaled, dest_path, "png", NULL, "compression", "9", NULL);
  return TRUE;
}

static gboolean
write_scaled_icon_from_file(const char *source_path, const char *dest_path, gboolean overwrite)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) src = gdk_pixbuf_new_from_file(source_path, &error);
  return write_scaled_icon_from_pixbuf(src, dest_path, overwrite);
}

static gboolean
write_scaled_icon_from_resource(const char *resource_path, const char *dest_path, gboolean overwrite)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) src = gdk_pixbuf_new_from_resource(resource_path, &error);
  return write_scaled_icon_from_pixbuf(src, dest_path, overwrite);
}

static void
write_server_icon_files(PumpkinServer *server, const char *source_path, gboolean overwrite, gboolean from_resource)
{
  if (server == NULL) {
    return;
  }

  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  g_autofree char *root_dir = g_strdup(pumpkin_server_get_root_dir(server));
  g_autofree char *data_server_icon = g_build_filename(data_dir, "server-icon.png", NULL);
  g_autofree char *data_icon = g_build_filename(data_dir, "icon.png", NULL);
  g_autofree char *root_server_icon = g_build_filename(root_dir, "server-icon.png", NULL);
  g_autofree char *root_icon = g_build_filename(root_dir, "icon.png", NULL);

  if (from_resource) {
    write_scaled_icon_from_resource(source_path, data_server_icon, overwrite);
    write_scaled_icon_from_resource(source_path, data_icon, overwrite);
    write_scaled_icon_from_resource(source_path, root_server_icon, overwrite);
    write_scaled_icon_from_resource(source_path, root_icon, overwrite);
  } else {
    write_scaled_icon_from_file(source_path, data_server_icon, overwrite);
    write_scaled_icon_from_file(source_path, data_icon, overwrite);
    write_scaled_icon_from_file(source_path, root_server_icon, overwrite);
    write_scaled_icon_from_file(source_path, root_icon, overwrite);
  }
}

static void
ensure_default_server_icon(PumpkinServer *server)
{
  const char *res = "/dev/rotstein/SmashedPumpkin/icons/hicolor/512x512/apps/dev.rotstein.SmashedPumpkin.png";
  write_server_icon_files(server, res, FALSE, TRUE);
}

static void
write_default_server_icon(PumpkinServer *server)
{
  const char *res = "/dev/rotstein/SmashedPumpkin/icons/hicolor/512x512/apps/dev.rotstein.SmashedPumpkin.png";
  write_server_icon_files(server, res, TRUE, TRUE);
}

static GtkWidget *
create_server_icon_widget(PumpkinServer *server)
{
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  g_autofree char *icon_path = g_build_filename(data_dir, "server-icon.png", NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkTexture) texture = NULL;

  if (g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
    g_autoptr(GFile) file = g_file_new_for_path(icon_path);
    texture = gdk_texture_new_from_file(file, &error);
  }
  if (texture == NULL) {
    texture = gdk_texture_new_from_resource(
      "/dev/rotstein/SmashedPumpkin/icons/hicolor/512x512/apps/dev.rotstein.SmashedPumpkin.png");
  }

  GtkWidget *image = gtk_image_new_from_paintable(GDK_PAINTABLE(texture));
  gtk_image_set_pixel_size(GTK_IMAGE(image), 48);
  return image;
}

static void
select_server(PumpkinWindow *self, PumpkinServer *server)
{
  if (self->current != NULL) {
    g_signal_handlers_disconnect_by_func(self->current, G_CALLBACK(on_log_line), self);
  }

  self->current = server;
  pumpkin_server_store_set_selected(self->store, server);

  if (self->current != NULL) {
    g_signal_connect(self->current, "log-line", G_CALLBACK(on_log_line), self);
  }

  if (server == NULL) {
    self->ui_state = UI_STATE_IDLE;
  } else {
    self->ui_state = pumpkin_server_get_running(server) ? UI_STATE_RUNNING : UI_STATE_IDLE;
  }

  self->last_proc_jiffies = 0;

  if (self->log_view != NULL) {
    GtkTextBuffer *buffer = NULL;
    if (server != NULL) {
      buffer = g_hash_table_lookup(self->console_buffers, server);
      if (buffer == NULL) {
        buffer = gtk_text_buffer_new(NULL);
        g_hash_table_insert(self->console_buffers, g_object_ref(server), buffer);
      }
    } else {
      buffer = gtk_text_buffer_new(NULL);
    }
    gtk_text_view_set_buffer(self->log_view, buffer);
  }

  update_details(self);
  update_settings_form(self);
  refresh_plugin_list(self);
  refresh_world_list(self);
  refresh_player_list(self);
  refresh_log_files(self);
  if (self->stats_row != NULL) {
    gtk_widget_set_visible(GTK_WIDGET(self->stats_row),
                           server != NULL && pumpkin_server_get_running(server));
  }
  if (server != NULL) {
    ensure_default_server_icon(server);
  }
}

static void
on_server_selected(GtkListBox *box, GtkListBoxRow *row, PumpkinWindow *self)
{
  (void)box;
  if (row == NULL) {
    select_server(self, NULL);
    return;
  }

  PumpkinServer *server = g_object_get_data(G_OBJECT(row), "server");
  if (self->settings_dirty && self->details_stack != NULL) {
    const char *page = adw_view_stack_get_visible_child_name(self->details_stack);
    if (page != NULL && g_strcmp0(page, "settings") == 0 && server != self->current) {
      if (self->pending_server != NULL) {
        g_object_unref(self->pending_server);
      }
      self->pending_server = g_object_ref(server);
      self->settings_guard = TRUE;
      if (self->current != NULL) {
        GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->server_list));
        while (child != NULL) {
          GtkListBoxRow *candidate = GTK_LIST_BOX_ROW(child);
          PumpkinServer *row_server = g_object_get_data(G_OBJECT(candidate), "server");
          if (row_server == self->current) {
            gtk_list_box_select_row(self->server_list, candidate);
            break;
          }
          child = gtk_widget_get_next_sibling(child);
        }
      }
      self->settings_guard = FALSE;
      confirm_leave_settings(self, "settings", NULL);
      return;
    }
  }
  select_server(self, server);
}

static void
on_add_server(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  AdwDialog *dialog = adw_alert_dialog_new("New Server", "Choose a unique server name.");
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_response(alert, "cancel", "Cancel");
  adw_alert_dialog_add_response(alert, "create", "Create");
  adw_alert_dialog_set_default_response(alert, "create");
  adw_alert_dialog_set_close_response(alert, "cancel");

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Server name");
  g_object_set_data(G_OBJECT(entry), "dialog", dialog);
  g_signal_connect(entry, "activate", G_CALLBACK(on_add_server_entry_activate), self);
  adw_alert_dialog_set_extra_child(alert, entry);
  adw_dialog_set_focus(ADW_DIALOG(dialog), entry);
  g_object_set_data(G_OBJECT(dialog), "entry", entry);
  g_object_set_data(G_OBJECT(dialog), "window", self);
  adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_add_server_confirmed, self);
}

static void
on_add_server_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  PumpkinWindow *self = g_object_get_data(G_OBJECT(dialog), "window");
  GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "entry");
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
  if (self == NULL || entry == NULL || g_strcmp0(response, "create") != 0) {
    return;
  }

  const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
  try_create_server(self, name);
}

static void
on_add_server_entry_activate(GtkEntry *entry, PumpkinWindow *self)
{
  AdwDialog *dialog = g_object_get_data(G_OBJECT(entry), "dialog");
  const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (try_create_server(self, name) && dialog != NULL) {
    adw_dialog_close(dialog);
  }
}

static gboolean
copy_tree(const char *src, const char *dest, GError **error)
{
  if (!g_file_test(src, G_FILE_TEST_IS_DIR)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Source is not a directory");
    return FALSE;
  }

  if (g_mkdir_with_parents(dest, 0755) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Failed to create directory %s", dest);
    return FALSE;
  }

  GDir *dir = g_dir_open(src, 0, error);
  if (dir == NULL) {
    return FALSE;
  }

  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child_src = g_build_filename(src, entry, NULL);
    g_autofree char *child_dest = g_build_filename(dest, entry, NULL);
    if (g_file_test(child_src, G_FILE_TEST_IS_DIR)) {
      if (!copy_tree(child_src, child_dest, error)) {
        g_dir_close(dir);
        return FALSE;
      }
    } else {
      g_autoptr(GFile) src_file = g_file_new_for_path(child_src);
      g_autoptr(GFile) dest_file = g_file_new_for_path(child_dest);
      if (!g_file_copy(src_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error)) {
        g_dir_close(dir);
        return FALSE;
      }
    }
  }
  g_dir_close(dir);
  return TRUE;
}

static char *
unique_import_dir(const char *base_dir, const char *name)
{
  g_autofree char *base_name = g_path_get_basename(name);
  g_autofree char *candidate = g_build_filename(base_dir, base_name, NULL);
  if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) {
    return g_strdup(candidate);
  }

  for (guint i = 2; i < 1000; i++) {
    g_autofree char *with_suffix = g_strdup_printf("%s-%u", base_name, i);
    g_autofree char *path = g_build_filename(base_dir, with_suffix, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
      return g_strdup(path);
    }
  }

  g_autofree char *uuid = g_uuid_string_random();
  return g_build_filename(base_dir, uuid, NULL);
}

static gboolean
path_is_within(const char *path, const char *base_dir)
{
  g_autofree char *canon_path = g_canonicalize_filename(path, NULL);
  g_autofree char *canon_base = g_canonicalize_filename(base_dir, NULL);
  if (canon_path == NULL || canon_base == NULL) {
    return FALSE;
  }

  g_autofree char *base_with_sep = NULL;
  if (g_str_has_suffix(canon_base, G_DIR_SEPARATOR_S)) {
    base_with_sep = g_strdup(canon_base);
  } else {
    base_with_sep = g_strconcat(canon_base, G_DIR_SEPARATOR_S, NULL);
  }

  return g_str_has_prefix(canon_path, base_with_sep);
}

static void
on_import_server(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Import Server Folder");
  gtk_file_dialog_select_folder(dialog, GTK_WINDOW(self), NULL, on_import_server_done, self);
  g_object_unref(dialog);
}

static void
on_import_server_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, &error);
  if (folder == NULL) {
    return;
  }

  g_autofree char *path = g_file_get_path(folder);
  if (path == NULL) {
    return;
  }

  const char *base_dir = pumpkin_server_store_get_base_dir(self->store);
  if (base_dir == NULL) {
    append_log(self, "Missing base directory");
    return;
  }

  g_autofree char *target_dir = NULL;
  if (path_is_within(path, base_dir)) {
    target_dir = g_strdup(path);
  } else {
    target_dir = unique_import_dir(base_dir, path);
    g_autoptr(GError) copy_error = NULL;
    if (!copy_tree(path, target_dir, &copy_error)) {
      if (copy_error != NULL) {
        append_log(self, copy_error->message);
      }
      return;
    }
  }

  g_autoptr(GError) import_error = NULL;
  PumpkinServer *server = pumpkin_server_store_import(self->store, target_dir, &import_error);
  if (server == NULL) {
    if (import_error != NULL) {
      append_log(self, import_error->message);
    }
    return;
  }

  g_object_unref(server);
  clear_list_box(self->server_list);
  load_server_list(self);
  update_overview(self);
  refresh_overview_list(self);
}

static void
on_choose_icon_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error);
  if (file == NULL || self->current == NULL) {
    return;
  }

  g_autofree char *path = g_file_get_path(file);
  if (path == NULL) {
    return;
  }

  write_server_icon_files(self->current, path, TRUE, FALSE);
  append_log(self, "Server icon updated");
  refresh_overview_list(self);
}

static void
on_choose_icon(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Choose Server Icon");

  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
  gtk_file_filter_add_mime_type(filter, "image/png");
  gtk_file_filter_set_name(filter, "PNG images");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));

  gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_choose_icon_done, self);
  g_object_unref(dialog);
}

static void
on_reset_icon(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }

  write_default_server_icon(self->current);
  append_log(self, "Server icon reset to default");
  refresh_overview_list(self);
}

static void
on_remove_server(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }

  pumpkin_server_store_remove_selected(self->store);
  clear_list_box(self->server_list);
  load_server_list(self);
  update_overview(self);
  update_details(self);
  refresh_overview_list(self);
}

static void
download_context_free(DownloadContext *ctx)
{
  if (ctx == NULL) {
    return;
  }
  g_clear_object(&ctx->self);
  g_clear_object(&ctx->server);
  g_clear_pointer(&ctx->used_url, g_free);
  g_clear_pointer(&ctx->dest_path, g_free);
  g_clear_pointer(&ctx->tmp_path, g_free);
  g_clear_pointer(&ctx->server_bin, g_free);
  g_free(ctx);
}

static void
start_download_for_server(PumpkinWindow *self, PumpkinServer *server, const char *url)
{
  if (server == NULL) {
    return;
  }

  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  gboolean use_cache = use_download_cache(self);
  g_autofree char *cache_path = use_cache ? cache_path_for_url(self, url) : NULL;

  DownloadProgressState *state = get_download_progress_state(self, server, TRUE);
  if (state != NULL) {
    state->active = TRUE;
    state->current = 0;
    state->total = 0;
  }
  refresh_overview_list(self);

  if (pumpkin_server_get_running(server)) {
    append_log_for_server(self, server, "Stopping server before install/update...");
    pumpkin_server_stop(server);
  }

  set_details_status_for_server(self, server, "Downloading Pumpkin…", 0);
  if (cache_path != NULL && g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
    g_autoptr(GError) copy_error = NULL;
    if (copy_binary(cache_path, bin, &copy_error)) {
      set_download_busy(self, FALSE);
      append_log_for_server(self, server, "Installed from local cache");
      set_details_status_for_server(self, server, "Installed from cache", 3);
      if (state != NULL) {
        state->active = FALSE;
      }
      refresh_overview_list(self);
      pumpkin_server_set_installed_url(server, url);
      pumpkin_server_save(server, NULL);
      update_overview(self);
      refresh_overview_list(self);
      update_details(self);
      return;
    }
    if (copy_error != NULL) {
      append_log_for_server(self, server, copy_error->message);
      set_details_status_for_server(self, server, "Install failed", 5);
    }
    if (state != NULL) {
      state->active = FALSE;
    }
    refresh_overview_list(self);
  }

  g_autofree char *tmp = NULL;
  if (cache_path != NULL) {
    tmp = g_strconcat(cache_path, ".tmp", NULL);
  } else {
    tmp = g_strconcat(bin, ".tmp", NULL);
  }

  DownloadContext *ctx = g_new0(DownloadContext, 1);
  ctx->self = g_object_ref(self);
  ctx->server = g_object_ref(server);
  ctx->used_url = g_strdup(url);
  ctx->dest_path = g_strdup(cache_path != NULL ? cache_path : bin);
  ctx->tmp_path = g_strdup(tmp);
  ctx->server_bin = g_strdup(bin);
  ctx->use_cache = (cache_path != NULL);

  pumpkin_download_file_async(url, tmp, NULL, on_download_progress, ctx, on_download_done, ctx);
  set_download_busy(self, TRUE);
  append_log_for_server(self, server, "Downloading Pumpkin binary...");
}

static void
on_download_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  DownloadContext *ctx = user_data;
  PumpkinWindow *self = ctx->self;
  PumpkinServer *server = ctx->server;
  g_autoptr(GError) error = NULL;

  if (!pumpkin_download_file_finish(res, &error)) {
    append_log_for_server(self, server, error->message);
    set_details_status_for_server(self, server, "Download failed", 5);
    DownloadProgressState *state = get_download_progress_state(self, server, FALSE);
    if (state != NULL) {
      state->active = FALSE;
    }
    refresh_overview_list(self);
    set_download_busy(self, FALSE);
    download_context_free(ctx);
    return;
  }

  if (server == NULL) {
    download_context_free(ctx);
    return;
  }

  if (ctx->tmp_path != NULL && ctx->dest_path != NULL) {
    g_chmod(ctx->tmp_path, 0755);
    if (g_rename(ctx->tmp_path, ctx->dest_path) != 0) {
      g_autofree char *msg = g_strdup_printf("Failed to replace binary: %s", g_strerror(errno));
      append_log_for_server(self, server, msg);
      set_details_status_for_server(self, server, "Download failed", 5);
      DownloadProgressState *state = get_download_progress_state(self, server, FALSE);
      if (state != NULL) {
        state->active = FALSE;
      }
      refresh_overview_list(self);
      set_download_busy(self, FALSE);
      download_context_free(ctx);
      return;
    }
  }

  if (ctx->use_cache && ctx->server_bin != NULL && ctx->dest_path != NULL) {
    g_autoptr(GError) copy_error = NULL;
    if (!copy_binary(ctx->dest_path, ctx->server_bin, &copy_error)) {
      if (copy_error != NULL) {
        append_log_for_server(self, server, copy_error->message);
      } else {
        append_log_for_server(self, server, "Failed to copy cached binary to server");
      }
      set_details_status_for_server(self, server, "Install failed", 5);
      DownloadProgressState *state = get_download_progress_state(self, server, FALSE);
      if (state != NULL) {
        state->active = FALSE;
      }
      refresh_overview_list(self);
      set_download_busy(self, FALSE);
      download_context_free(ctx);
      return;
    }
  }

  set_download_busy(self, FALSE);
  append_log_for_server(self, server, ctx->use_cache ? "Download complete (cached)" : "Download complete");
  set_details_status_for_server(self, server, "Download complete", 3);
  DownloadProgressState *state = get_download_progress_state(self, server, FALSE);
  if (state != NULL) {
    state->active = FALSE;
  }
  refresh_overview_list(self);
  pumpkin_server_set_installed_url(server, ctx->used_url);
  pumpkin_server_save(server, NULL);
  update_overview(self);
  refresh_overview_list(self);
  update_details(self);
  download_context_free(ctx);
}

static void
on_latest_only_resolve_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  g_autoptr(GError) error = NULL;

  g_autofree char *url = pumpkin_resolve_latest_linux_x64_finish(res, NULL, &error);
  if (url == NULL) {
    append_log(self, error->message);
    set_details_error(self, error->message);
    return;
  }

  g_clear_pointer(&self->latest_url, g_free);
  self->latest_url = g_strdup(url);
  refresh_overview_list(self);
  update_details(self);

  if (self->current != NULL) {
    const char *installed_url = pumpkin_server_get_installed_url(self->current);
    g_autofree char *bin = pumpkin_server_get_bin_path(self->current);
    gboolean installed = g_file_test(bin, G_FILE_TEST_EXISTS);
    if (installed && installed_url != NULL && g_strcmp0(self->latest_url, installed_url) == 0) {
      append_log(self, "No updates available");
    } else if (installed) {
      append_log(self, "Update available");
    }
  }
  if (self->config != NULL) {
    pumpkin_config_set_default_download_url(self->config, url);
    pumpkin_config_save(self->config, NULL);
  }
}

static void
open_folder(PumpkinWindow *self, const char *path)
{
  GFile *file = g_file_new_for_path(path);
  GtkFileLauncher *launcher = gtk_file_launcher_new(file);
  gtk_file_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
  g_object_unref(launcher);
  g_object_unref(file);
}

static void
on_overview_remove_clicked(GtkButton *button, PumpkinWindow *self)
{
  PumpkinServer *server = g_object_get_data(G_OBJECT(button), "server");
  if (server == NULL) {
    return;
  }

  g_autofree char *title = g_strdup_printf("Delete \"%s\"?", pumpkin_server_get_name(server));
  AdwDialog *dialog = adw_alert_dialog_new(title, "This permanently deletes the server and its data.");
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_response(alert, "cancel", "Cancel");
  adw_alert_dialog_add_response(alert, "delete", "Delete");
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  adw_alert_dialog_set_response_appearance(alert, "delete", ADW_RESPONSE_DESTRUCTIVE);
  g_object_set_data_full(G_OBJECT(dialog), "server", g_object_ref(server), g_object_unref);
  g_object_set_data(G_OBJECT(dialog), "window", self);
  adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_overview_remove_confirmed, self);
}

static void
on_overview_remove_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  PumpkinServer *server = g_object_get_data(G_OBJECT(dialog), "server");
  PumpkinWindow *self = g_object_get_data(G_OBJECT(dialog), "window");
  if (server == NULL || self == NULL) {
    return;
  }

  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
  if (response == NULL || g_strcmp0(response, "delete") != 0) {
    return;
  }

  pumpkin_server_store_set_selected(self->store, server);
  pumpkin_server_store_remove_selected(self->store);
  if (self->console_buffers != NULL) {
    g_hash_table_remove(self->console_buffers, server);
  }
  if (self->download_progress_state != NULL) {
    g_hash_table_remove(self->download_progress_state, server);
  }
  clear_list_box(self->server_list);
  load_server_list(self);
  update_details(self);
  refresh_overview_list(self);
}

static void
on_details_back(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->details_stack != NULL) {
    const char *page = adw_view_stack_get_visible_child_name(self->details_stack);
    if (self->settings_dirty && page != NULL && g_strcmp0(page, "settings") == 0) {
      confirm_leave_settings(self, "settings", "overview");
      return;
    }
  }
  adw_view_stack_set_visible_child_name(self->view_stack, "overview");
}

static void
on_details_start(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  if (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_STOPPING ||
      self->ui_state == UI_STATE_RESTARTING || self->ui_state == UI_STATE_RUNNING) {
    return;
  }
  self->ui_state = UI_STATE_STARTING;
  set_action_cooldown(self, 2);
  update_details(self);

  int port = pumpkin_server_get_port(self->current);
  if (port <= 0) {
    set_details_error(self, "Invalid server port.");
    self->ui_state = UI_STATE_ERROR;
    return;
  }

  g_autoptr(GSocketListener) listener = g_socket_listener_new();
  g_autoptr(GError) port_error = NULL;
  gboolean port_ok = g_socket_listener_add_inet_port(listener, port, NULL, &port_error);
  if (!port_ok) {
    if (port_error != NULL) {
      g_autofree char *msg = g_strdup_printf("Port %d is in use: %s", port, port_error->message);
      set_details_error(self, msg);
      append_log(self, msg);
    } else {
      g_autofree char *msg = g_strdup_printf("Port %d is in use.", port);
      set_details_error(self, msg);
      append_log(self, msg);
    }
    self->ui_state = UI_STATE_ERROR;
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_start(self->current, &error)) {
    append_log(self, error->message);
    set_details_error(self, error->message);
    self->ui_state = UI_STATE_ERROR;
  } else {
    if (self->start_delay_id != 0) {
      g_source_remove(self->start_delay_id);
      self->start_delay_id = 0;
    }
    self->start_delay_id = g_timeout_add_seconds(2, start_after_delay, g_object_ref(self));
  }
  update_details(self);
  refresh_overview_list(self);
  refresh_world_list(self);
  refresh_player_list(self);
  refresh_log_files(self);
}

static void
on_details_stop(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  if (self->ui_state == UI_STATE_STOPPING || self->ui_state == UI_STATE_RESTARTING ||
      self->ui_state == UI_STATE_STARTING) {
    return;
  }

  self->ui_state = UI_STATE_STOPPING;
  set_action_cooldown(self, 2);
  pumpkin_server_stop(self->current);
  update_details(self);
  refresh_overview_list(self);
  refresh_world_list(self);
  refresh_player_list(self);
  refresh_log_files(self);
}

static void
on_details_restart(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }

  if (self->restart_delay_id != 0) {
    g_source_remove(self->restart_delay_id);
    self->restart_delay_id = 0;
  }

  self->restart_requested = TRUE;
  self->ui_state = UI_STATE_RESTARTING;
  set_action_cooldown(self, 2);

  if (pumpkin_server_get_running(self->current)) {
    pumpkin_server_stop(self->current);
  }

  RestartContext *ctx = g_new0(RestartContext, 1);
  ctx->self = g_object_ref(self);
  ctx->server = g_object_ref(self->current);
  self->restart_delay_id = g_timeout_add_seconds(2, restart_after_delay, ctx);
  update_details(self);
}

static void
on_world_delete_clicked(GtkButton *button, PumpkinWindow *self)
{
  if (self->current == NULL || pumpkin_server_get_running(self->current)) {
    return;
  }

  const char *path = g_object_get_data(G_OBJECT(button), "world-path");
  if (path == NULL) {
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_store_remove_tree(path, &error)) {
    if (error != NULL) {
      append_log(self, error->message);
    }
    return;
  }

  refresh_world_list(self);
}

static void
on_details_install(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  if (pumpkin_server_get_running(self->current)) {
    set_details_error(self, "Stop the server before installing.");
    return;
  }

  g_autofree char *bin = pumpkin_server_get_bin_path(self->current);
  if (g_file_test(bin, G_FILE_TEST_EXISTS)) {
    AdwDialog *dialog = adw_alert_dialog_new("Overwrite existing installation?",
                                             "This will replace the current Pumpkin binary.");
    AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
    adw_alert_dialog_add_response(alert, "cancel", "Cancel");
    adw_alert_dialog_add_response(alert, "overwrite", "Overwrite");
    adw_alert_dialog_set_default_response(alert, "cancel");
    adw_alert_dialog_set_close_response(alert, "cancel");
    g_object_set_data(G_OBJECT(dialog), "server", self->current);
    g_object_set_data(G_OBJECT(dialog), "window", self);
    adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_overview_install_confirmed, self);
    return;
  }

  const char *url = self->latest_url != NULL ? self->latest_url : pumpkin_server_get_download_url(self->current);
  start_download_for_server(self, self->current, url);
}

static void
on_details_update(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL || self->latest_url == NULL) {
    return;
  }
  if (pumpkin_server_get_running(self->current)) {
    set_details_error(self, "Stop the server before updating.");
    return;
  }

  g_autofree char *bin = pumpkin_server_get_bin_path(self->current);
  if (!g_file_test(bin, G_FILE_TEST_EXISTS)) {
    return;
  }

  const char *installed_url = pumpkin_server_get_installed_url(self->current);
  if (installed_url != NULL && g_strcmp0(self->latest_url, installed_url) == 0) {
    return;
  }

  start_download_for_server(self, self->current, self->latest_url);
}

static void
on_details_check_updates(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  if (pumpkin_server_get_running(self->current)) {
    set_details_error(self, "Stop the server before checking for updates.");
    return;
  }
  pumpkin_resolve_latest_linux_x64_async(NULL, on_latest_only_resolve_done, self);
  append_log(self, "Checking for updates...");
}

static void
on_send_command(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }

  const char *cmd = gtk_editable_get_text(GTK_EDITABLE(self->entry_command));
  if (cmd == NULL || *cmd == '\0') {
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_send_command(self->current, cmd, &error)) {
    append_log(self, error->message);
    return;
  }

  gtk_editable_set_text(GTK_EDITABLE(self->entry_command), "");
}

static gboolean
log_matches_filter(GDateTime *mtime, const char *name, int filter_index, const char *query)
{
  if (query != NULL && *query != '\0') {
    if (name == NULL || g_strrstr(g_ascii_strdown(name, -1), g_ascii_strdown(query, -1)) == NULL) {
      return FALSE;
    }
  }

  if (mtime == NULL) {
    return TRUE;
  }

  if (filter_index == 1) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    return g_date_time_get_year(mtime) == g_date_time_get_year(now) &&
           g_date_time_get_day_of_year(mtime) == g_date_time_get_day_of_year(now);
  }

  if (filter_index == 2) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autoptr(GDateTime) week_start = g_date_time_add_days(now, -6);
    return g_date_time_compare(mtime, week_start) >= 0;
  }

  return TRUE;
}

static void
refresh_log_files(PumpkinWindow *self)
{
  if (self->log_files_list == NULL || self->log_file_view == NULL) {
    return;
  }

  clear_list_box(self->log_files_list);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_file_view);
  gtk_text_buffer_set_text(buffer, "", -1);

  if (self->current == NULL) {
    return;
  }

  g_autofree char *logs_dir = pumpkin_server_get_logs_dir(self->current);
  GDir *dir = g_dir_open(logs_dir, 0, NULL);
  if (dir == NULL) {
    return;
  }

  int filter_index = 0;
  if (self->log_filter != NULL) {
    filter_index = gtk_drop_down_get_selected(self->log_filter);
  }

  const char *query = NULL;
  if (self->log_search != NULL) {
    query = gtk_editable_get_text(GTK_EDITABLE(self->log_search));
  }

  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *path = g_build_filename(logs_dir, entry, NULL);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
      continue;
    }

    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 0, NULL, NULL);
    g_autoptr(GDateTime) mtime = NULL;
    if (info != NULL) {
      guint64 mtime_unix = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
      mtime = g_date_time_new_from_unix_local((gint64)mtime_unix);
    }

    if (!log_matches_filter(mtime, entry, filter_index, query)) {
      continue;
    }

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(entry);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);

    if (mtime != NULL) {
      g_autofree char *when = g_date_time_format(mtime, "%Y-%m-%d %H:%M");
      GtkWidget *time_label = gtk_label_new(when);
      gtk_widget_add_css_class(time_label, "dim-label");
      gtk_box_append(GTK_BOX(box), time_label);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "log-path", g_strdup(path), g_free);
    gtk_list_box_append(self->log_files_list, row);
  }

  g_dir_close(dir);
}

typedef struct {
  PumpkinWindow *self;
  char *dest_path;
  char *tmp_path;
} PluginDownloadContext;

static void
plugin_download_context_free(PluginDownloadContext *ctx)
{
  if (ctx == NULL) {
    return;
  }
  g_clear_object(&ctx->self);
  g_clear_pointer(&ctx->dest_path, g_free);
  g_clear_pointer(&ctx->tmp_path, g_free);
  g_free(ctx);
}

static void
on_plugin_download_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  PluginDownloadContext *ctx = user_data;
  PumpkinWindow *self = ctx->self;
  g_autoptr(GError) error = NULL;

  if (!pumpkin_download_file_finish(res, &error)) {
    append_log(self, error->message);
    plugin_download_context_free(ctx);
    return;
  }

  if (ctx->tmp_path != NULL && ctx->dest_path != NULL) {
    if (g_rename(ctx->tmp_path, ctx->dest_path) != 0) {
      g_autofree char *msg = g_strdup_printf("Failed to install plugin: %s", g_strerror(errno));
      append_log(self, msg);
      plugin_download_context_free(ctx);
      return;
    }
  }

  append_log(self, "Plugin downloaded");
  refresh_plugin_list(self);
  plugin_download_context_free(ctx);
}

static void
on_install_plugin(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL || self->entry_plugin_url == NULL) {
    return;
  }

  const char *url = gtk_editable_get_text(GTK_EDITABLE(self->entry_plugin_url));
  if (url == NULL || *url == '\0') {
    append_log(self, "Plugin URL is empty.");
    return;
  }

  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  const char *path = uri != NULL ? g_uri_get_path(uri) : NULL;
  if (path == NULL || *path == '\0') {
    append_log(self, "Invalid plugin URL.");
    return;
  }

  g_autofree char *filename = g_path_get_basename(path);
  if (filename == NULL || *filename == '\0') {
    append_log(self, "Invalid plugin URL.");
    return;
  }

  g_autofree char *plugins_dir = pumpkin_server_get_plugins_dir(self->current);
  g_autofree char *dest = g_build_filename(plugins_dir, filename, NULL);
  g_autofree char *tmp = g_strconcat(dest, ".tmp", NULL);

  PluginDownloadContext *ctx = g_new0(PluginDownloadContext, 1);
  ctx->self = g_object_ref(self);
  ctx->dest_path = g_strdup(dest);
  ctx->tmp_path = g_strdup(tmp);

  pumpkin_download_file_async(url, tmp, NULL, NULL, NULL, on_plugin_download_done, ctx);
  append_log(self, "Downloading plugin...");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_plugin_url), "");
}

static void
on_log_filter_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self)
{
  (void)object;
  (void)pspec;
  refresh_log_files(self);
}

static void
on_log_level_filter_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self)
{
  (void)object;
  (void)pspec;
  if (self->current_log_path != NULL) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_file_view);
    gtk_text_buffer_set_text(buffer, "", -1);
    GtkListBoxRow *row = NULL;
    if (self->log_files_list != NULL) {
      GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->log_files_list));
      while (child != NULL) {
        GtkListBoxRow *candidate = GTK_LIST_BOX_ROW(child);
        const char *path = g_object_get_data(G_OBJECT(candidate), "log-path");
        if (path != NULL && g_strcmp0(path, self->current_log_path) == 0) {
          row = candidate;
          break;
        }
        child = gtk_widget_get_next_sibling(child);
      }
    }
    if (row != NULL) {
      on_log_file_activated(self->log_files_list, row, self);
    }
  }
}

static void
on_log_search_changed(GtkEditable *editable, PumpkinWindow *self)
{
  (void)editable;
  refresh_log_files(self);
}

static void
on_log_file_activated(GtkListBox *box, GtkListBoxRow *row, PumpkinWindow *self)
{
  (void)box;
  if (row == NULL || self->log_file_view == NULL) {
    return;
  }

  const char *path = g_object_get_data(G_OBJECT(row), "log-path");
  if (path == NULL) {
    return;
  }

  g_free(self->current_log_path);
  self->current_log_path = g_strdup(path);

  g_autoptr(GError) error = NULL;
  g_autofree char *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, &error)) {
    return;
  }

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_file_view);
  gtk_text_buffer_set_text(buffer, "", -1);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  int level_index = 0;
  if (self->log_level_filter != NULL) {
    level_index = gtk_drop_down_get_selected(self->log_level_filter);
  }
  char *saveptr = NULL;
  char *line = strtok_r(contents, "\n", &saveptr);
  while (line != NULL) {
    if (log_line_matches_level(line, level_index)) {
      gtk_text_buffer_insert(buffer, &end, line, -1);
      gtk_text_buffer_insert(buffer, &end, "\n", -1);
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }

  GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "file-end");
  if (mark == NULL) {
    mark = gtk_text_buffer_create_mark(buffer, "file-end", &end, FALSE);
  } else {
    gtk_text_buffer_move_mark(buffer, mark, &end);
  }
  gtk_text_view_scroll_to_mark(self->log_file_view, mark, 0.0, TRUE, 0.0, 1.0);
}

static void
on_open_plugins(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  g_autofree char *dir = pumpkin_server_get_plugins_dir(self->current);
  open_folder(self, dir);
}

static void
on_open_logs(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  g_autofree char *dir = pumpkin_server_get_logs_dir(self->current);
  open_folder(self, dir);
}

static void
on_open_server_root(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  const char *root = pumpkin_server_get_root_dir(self->current);
  if (root != NULL) {
    open_folder(self, root);
  }
}

static void
on_open_players(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  g_autofree char *dir = pumpkin_server_get_players_dir(self->current);
  open_folder(self, dir);
}

static void
on_open_worlds(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  g_autofree char *default_world = g_build_filename(pumpkin_server_get_data_dir(self->current), "world", NULL);
  if (g_file_test(default_world, G_FILE_TEST_IS_DIR)) {
    open_folder(self, default_world);
    return;
  }
  g_autofree char *dir = pumpkin_server_get_worlds_dir(self->current);
  open_folder(self, dir);
}

static void
on_save_settings(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    if (self->config != NULL && self->switch_use_cache != NULL) {
      pumpkin_config_set_use_cache(self->config, gtk_switch_get_active(self->switch_use_cache));
      pumpkin_config_save(self->config, NULL);
      append_log(self, "Settings saved");
    }
    return;
  }

  pumpkin_server_set_name(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_server_name)));
  pumpkin_server_set_download_url(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_download_url)));
  pumpkin_server_set_port(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_server_port))));
  pumpkin_server_set_max_players(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_players))));
  if (self->switch_auto_restart != NULL) {
    pumpkin_server_set_auto_restart(self->current, gtk_switch_get_active(self->switch_auto_restart));
  }
  if (self->entry_auto_restart_delay != NULL) {
    pumpkin_server_set_auto_restart_delay(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_auto_restart_delay))));
  }
  pumpkin_server_set_rcon_host(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_host)));
  pumpkin_server_set_rcon_port(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_port))));
  pumpkin_server_set_rcon_password(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_password)));

  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_save(self->current, &error)) {
    append_log(self, error->message);
  } else {
    append_log(self, "Settings saved");
  }

  if (self->config != NULL) {
    const char *download_url = gtk_editable_get_text(GTK_EDITABLE(self->entry_download_url));
    pumpkin_config_set_default_download_url(self->config, download_url);
    if (self->switch_use_cache != NULL) {
      pumpkin_config_set_use_cache(self->config, gtk_switch_get_active(self->switch_use_cache));
    }
    pumpkin_config_save(self->config, NULL);
  }

  self->settings_dirty = FALSE;
  clear_list_box(self->server_list);
  load_server_list(self);
  update_overview(self);
}

static void
pumpkin_window_init(PumpkinWindow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  adw_view_stack_set_visible_child_name(self->view_stack, "overview");

  self->store = pumpkin_server_store_new();
  load_server_list(self);

  g_signal_connect(self->server_list, "row-selected", G_CALLBACK(on_server_selected), self);
  g_signal_connect(self->btn_add_server, "clicked", G_CALLBACK(on_add_server), self);
  g_signal_connect(self->btn_add_server_overview, "clicked", G_CALLBACK(on_add_server), self);
  g_signal_connect(self->btn_import_server, "clicked", G_CALLBACK(on_import_server), self);
  g_signal_connect(self->btn_import_server_overview, "clicked", G_CALLBACK(on_import_server), self);
  g_signal_connect(self->btn_remove_server, "clicked", G_CALLBACK(on_remove_server), self);
  g_signal_connect(self->btn_details_back, "clicked", G_CALLBACK(on_details_back), self);
  g_signal_connect(self->btn_details_start, "clicked", G_CALLBACK(on_details_start), self);
  g_signal_connect(self->btn_details_stop, "clicked", G_CALLBACK(on_details_stop), self);
  g_signal_connect(self->btn_details_restart, "clicked", G_CALLBACK(on_details_restart), self);
  g_signal_connect(self->btn_details_install, "clicked", G_CALLBACK(on_details_install), self);
  g_signal_connect(self->btn_details_update, "clicked", G_CALLBACK(on_details_update), self);
  g_signal_connect(self->btn_details_check_updates, "clicked", G_CALLBACK(on_details_check_updates), self);
  g_signal_connect(self->btn_send_command, "clicked", G_CALLBACK(on_send_command), self);
  g_signal_connect(self->entry_command, "activate", G_CALLBACK(on_send_command), self);
  g_signal_connect(self->btn_choose_icon, "clicked", G_CALLBACK(on_choose_icon), self);
  g_signal_connect(self->btn_reset_icon, "clicked", G_CALLBACK(on_reset_icon), self);
  g_signal_connect(self->player_list, "row-activated", G_CALLBACK(on_player_row_activated), self);
  g_signal_connect(self->btn_open_plugins, "clicked", G_CALLBACK(on_open_plugins), self);
  g_signal_connect(self->btn_open_players, "clicked", G_CALLBACK(on_open_players), self);
  g_signal_connect(self->btn_open_worlds, "clicked", G_CALLBACK(on_open_worlds), self);
  g_signal_connect(self->btn_save_settings, "clicked", G_CALLBACK(on_save_settings), self);
  if (self->details_stack != NULL) {
    g_signal_connect(self->details_stack, "notify::visible-child-name",
                     G_CALLBACK(on_details_stack_changed), self);
  }
  if (self->entry_server_name != NULL) {
    g_signal_connect(self->entry_server_name, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_download_url != NULL) {
    g_signal_connect(self->entry_download_url, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_server_port != NULL) {
    g_signal_connect(self->entry_server_port, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_max_players != NULL) {
    g_signal_connect(self->entry_max_players, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_rcon_host != NULL) {
    g_signal_connect(self->entry_rcon_host, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_rcon_port != NULL) {
    g_signal_connect(self->entry_rcon_port, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_rcon_password != NULL) {
    g_signal_connect(self->entry_rcon_password, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_auto_restart_delay != NULL) {
    g_signal_connect(self->entry_auto_restart_delay, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->switch_auto_restart != NULL) {
    g_signal_connect(self->switch_auto_restart, "notify::active",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->switch_use_cache != NULL) {
    g_signal_connect(self->switch_use_cache, "notify::active",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->btn_install_plugin != NULL) {
    g_signal_connect(self->btn_install_plugin, "clicked", G_CALLBACK(on_install_plugin), self);
  }
  if (self->btn_open_server_root != NULL) {
    g_signal_connect(self->btn_open_server_root, "clicked", G_CALLBACK(on_open_server_root), self);
  }
  if (self->log_filter != NULL) {
    g_signal_connect(self->log_filter, "notify::selected", G_CALLBACK(on_log_filter_changed), self);
  }
  if (self->log_search != NULL) {
    g_signal_connect(self->log_search, "changed", G_CALLBACK(on_log_search_changed), self);
  }
  if (self->log_level_filter != NULL) {
    g_signal_connect(self->log_level_filter, "notify::selected", G_CALLBACK(on_log_level_filter_changed), self);
  }
  if (self->log_files_list != NULL) {
    g_signal_connect(self->log_files_list, "row-activated", G_CALLBACK(on_log_file_activated), self);
  }
  if (self->btn_open_logs != NULL) {
    g_signal_connect(self->btn_open_logs, "clicked", G_CALLBACK(on_open_logs), self);
  }
  if (self->btn_console_copy != NULL) {
    g_signal_connect(self->btn_console_copy, "clicked", G_CALLBACK(on_console_copy), self);
  }
  if (self->btn_console_clear != NULL) {
    g_signal_connect(self->btn_console_clear, "clicked", G_CALLBACK(on_console_clear), self);
  }

  self->config = pumpkin_config_load(NULL);
  self->live_player_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->console_buffers = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, g_object_unref);
  self->download_progress_state = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                        g_object_unref, (GDestroyNotify)download_progress_state_free);
  if (self->config != NULL) {
    const char *url = pumpkin_config_get_default_download_url(self->config);
    if (url != NULL) {
      guint n = g_list_model_get_n_items(pumpkin_server_store_get_model(self->store));
      for (guint i = 0; i < n; i++) {
        PumpkinServer *server = g_list_model_get_item(pumpkin_server_store_get_model(self->store), i);
        pumpkin_server_set_download_url(server, url);
        pumpkin_server_save(server, NULL);
        g_object_unref(server);
      }
    }
    if (self->switch_use_cache != NULL) {
      gtk_switch_set_active(self->switch_use_cache, pumpkin_config_get_use_cache(self->config));
    }
  }

  pumpkin_resolve_latest_linux_x64_async(NULL, on_latest_only_resolve_done, self);

  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/dev/rotstein/SmashedPumpkin/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_start), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_stop), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_restart), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_install), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_update), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->btn_details_update), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_check_updates), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_send_command), FALSE);

  apply_compact_button(GTK_WIDGET(self->btn_details_check_updates));
  apply_compact_button(GTK_WIDGET(self->btn_details_install));
  apply_compact_button(GTK_WIDGET(self->btn_details_update));
  apply_compact_button(GTK_WIDGET(self->btn_details_start));
  apply_compact_button(GTK_WIDGET(self->btn_details_stop));
  apply_compact_button(GTK_WIDGET(self->btn_details_restart));

  self->clk_tck = sysconf(_SC_CLK_TCK);
  self->stats_refresh_id = g_timeout_add(500, update_stats_tick, self);
}

static void
pumpkin_window_dispose(GObject *object)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(object);
  if (self->config != NULL) {
    pumpkin_config_free(self->config);
    self->config = NULL;
  }
  if (self->live_player_names != NULL) {
    g_hash_table_destroy(self->live_player_names);
    self->live_player_names = NULL;
  }
  if (self->console_buffers != NULL) {
    g_hash_table_destroy(self->console_buffers);
    self->console_buffers = NULL;
  }
  if (self->download_progress_state != NULL) {
    g_hash_table_destroy(self->download_progress_state);
    self->download_progress_state = NULL;
  }
  g_clear_pointer(&self->pending_details_page, g_free);
  g_clear_pointer(&self->pending_view_page, g_free);
  g_clear_pointer(&self->current_log_path, g_free);
  if (self->pending_server != NULL) {
    g_object_unref(self->pending_server);
    self->pending_server = NULL;
  }
  if (self->stats_refresh_id != 0) {
    g_source_remove(self->stats_refresh_id);
    self->stats_refresh_id = 0;
  }
  if (self->status_timeout_id != 0) {
    g_source_remove(self->status_timeout_id);
    self->status_timeout_id = 0;
  }
  if (self->download_progress_revealer != NULL) {
    gtk_revealer_set_reveal_child(self->download_progress_revealer, FALSE);
  }
  if (self->players_refresh_id != 0) {
    g_source_remove(self->players_refresh_id);
    self->players_refresh_id = 0;
  }
  if (self->restart_delay_id != 0) {
    g_source_remove(self->restart_delay_id);
    self->restart_delay_id = 0;
  }
  if (self->start_delay_id != 0) {
    g_source_remove(self->start_delay_id);
    self->start_delay_id = 0;
  }
  if (self->action_cooldown_id != 0) {
    g_source_remove(self->action_cooldown_id);
    self->action_cooldown_id = 0;
  }
  G_OBJECT_CLASS(pumpkin_window_parent_class)->dispose(object);
}

static void
update_live_player_names(PumpkinWindow *self, const char *line)
{
  if (self->live_player_names == NULL || line == NULL) {
    return;
  }

  if (strstr(line, "UUID: ") != NULL) {
    const char *uuid_pos = strstr(line, "UUID: ");
    const char *name_pos = strstr(line, "name=");
    if (uuid_pos != NULL && name_pos != NULL) {
      uuid_pos += strlen("UUID: ");
      name_pos += strlen("name=");
      const char *uuid_end = uuid_pos;
      while (*uuid_end != '\0' && !g_ascii_isspace(*uuid_end)) {
        uuid_end++;
      }
      const char *name_end = name_pos;
      while (*name_end != '\0' && *name_end != ' ' && *name_end != ',' && *name_end != ')') {
        name_end++;
      }
      if (uuid_end > uuid_pos && name_end > name_pos) {
        g_autofree char *uuid = g_strndup(uuid_pos, (gsize)(uuid_end - uuid_pos));
        g_autofree char *name = g_strndup(name_pos, (gsize)(name_end - name_pos));
        g_hash_table_replace(self->live_player_names, g_strdup(uuid), g_strdup(name));
        return;
      }
    }
  }

  g_autoptr(GRegex) join_regex = g_regex_new("\\b([A-Za-z0-9_]+) joined the game\\b",
                                             G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) join_match = NULL;
  if (g_regex_match(join_regex, line, 0, &join_match)) {
    g_autofree char *name = g_match_info_fetch(join_match, 1);
    if (name != NULL && *name != '\0') {
      g_hash_table_replace(self->live_player_names, g_strdup(name), g_strdup(name));
      return;
    }
  }

  g_autoptr(GRegex) left_regex = g_regex_new("\\b([A-Za-z0-9_]+) left the game\\b",
                                             G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) left_match = NULL;
  if (g_regex_match(left_regex, line, 0, &left_match)) {
    g_autofree char *name = g_match_info_fetch(left_match, 1);
    if (name != NULL && *name != '\0') {
      g_hash_table_remove(self->live_player_names, name);
      return;
    }
  }

  const char *start = strchr(line, '<');
  const char *end = start != NULL ? strchr(start + 1, '>') : NULL;
  if (start == NULL || end == NULL || end <= start + 1) {
    return;
  }

  g_autofree char *name = g_strndup(start + 1, (gsize)(end - start - 1));
  if (name[0] == '\0') {
    return;
  }

  g_hash_table_replace(self->live_player_names, g_strdup(name), g_strdup(name));
}

static void
pumpkin_window_class_init(PumpkinWindowClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);
  GObjectClass *object_class = G_OBJECT_CLASS(class);

  object_class->dispose = pumpkin_window_dispose;

  gtk_widget_class_set_template_from_resource(widget_class, "/dev/rotstein/SmashedPumpkin/ui/window.ui");

  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, view_stack);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, server_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_view);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, overview_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_add_server);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_add_server_overview);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_import_server);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_import_server_overview);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_plugin_url);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_install_plugin);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_remove_server);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_open_plugins);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_open_players);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_open_worlds);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_save_settings);

  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, plugin_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, world_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, player_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_files_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_file_view);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_filter);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_level_filter);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_search);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_open_logs);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_sys_cpu);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_sys_ram);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_srv_cpu);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_srv_ram);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, stats_row);

  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_title);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_server_icon);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_stack);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_error_revealer);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_error);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_status_revealer);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_status);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, download_progress_revealer);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, download_progress);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_details_back);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_details_start);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_details_stop);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_details_restart);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_details_install);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_details_update);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_details_check_updates);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, download_spinner);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_command);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_send_command);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_console_copy);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_console_clear);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_open_server_root);

  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_server_name);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_download_url);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_choose_icon);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_reset_icon);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_server_port);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_max_players);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_auto_restart);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_auto_restart_delay);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_host);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_port);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_password);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_use_cache);
}

GtkWindow *
pumpkin_window_new(AdwApplication *app)
{
  return GTK_WINDOW(g_object_new(PUMPKIN_TYPE_WINDOW, "application", app, NULL));
}
