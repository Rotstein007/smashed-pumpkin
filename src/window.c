#include "window.h"

#include "app-config.h"
#include "download.h"
#include "server-store.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#if defined(G_OS_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <libproc.h>
#endif
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#if !defined(G_OS_WIN32)
#include <unistd.h>
#endif
#include <time.h>

#define STATS_SAMPLE_SECONDS 1
#define STATS_HISTORY_SECONDS 180
#define STATS_SAMPLES (STATS_HISTORY_SECONDS / STATS_SAMPLE_SECONDS)
#define QUERY_STALE_SECONDS 5

struct _PumpkinWindow {
  AdwApplicationWindow parent_instance;

  AdwViewStack *view_stack;
  AdwViewStack *details_stack;
  AdwViewSwitcher *details_switcher;

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
  GtkBox *plugin_drop_hint;
  GtkBox *world_drop_hint;
  GtkListBox *whitelist_list;
  GtkListBox *banned_list;
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
  GtkDrawingArea *stats_graph_usage;
  GtkDrawingArea *stats_graph_players;
  GtkDrawingArea *stats_graph_disk;
  GtkLabel *label_stats_cpu;
  GtkLabel *label_stats_ram;
  GtkLabel *label_stats_disk;
  GtkLabel *label_stats_players;
  GtkRevealer *console_warning_revealer;
  GtkLabel *console_warning_label;
  GtkLabel *label_resource_limits;

  GtkLabel *details_title;
  GtkImage *details_server_icon;
  GtkButton *btn_details_back;
  GtkButton *btn_details_start;
  GtkButton *btn_details_stop;
  GtkButton *btn_details_restart;
  GtkButton *btn_details_install;
  GtkButton *btn_details_update;
  GtkButton *btn_details_check_updates;
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
  GHashTable *download_progress_state;
  gboolean restart_requested;
  gboolean user_stop_requested;
  gboolean restart_pending;
  guint stats_refresh_id;
  unsigned long long last_total_jiffies;
  unsigned long long last_idle_jiffies;
  unsigned long long last_proc_jiffies;
  int last_proc_pid;
  long clk_tck;
  double stats_cpu[STATS_SAMPLES];
  double stats_ram_mb[STATS_SAMPLES];
  double stats_disk_mb[STATS_SAMPLES];
  double stats_players[STATS_SAMPLES];
  int stats_index;
  int stats_count;
  double last_tps;
  gboolean last_tps_valid;
  gboolean tps_enabled;
  int query_players;
  int query_max_players;
  gint64 query_updated_at;
  gboolean query_valid;
  gboolean query_in_flight;

  int ui_state;

  GtkEntry *entry_server_name;
  GtkEntry *entry_download_url;
  GtkButton *btn_choose_icon;
  GtkButton *btn_reset_icon;
  GtkEntry *entry_server_port;
  GtkEntry *entry_bedrock_port;
  GtkEntry *entry_max_players;
  GtkEntry *entry_max_cpu_cores;
  GtkEntry *entry_max_ram_mb;
  GtkLabel *label_java_port_hint;
  GtkLabel *label_bedrock_port_hint;
  GtkLabel *label_max_players_hint;
  GtkLabel *label_max_cpu_hint;
  GtkLabel *label_max_ram_hint;
  GtkSwitch *switch_auto_restart;
  GtkEntry *entry_auto_restart_delay;
  GtkEntry *entry_rcon_host;
  GtkEntry *entry_rcon_port;
  GtkPasswordEntry *entry_rcon_password;
  GtkSwitch *switch_use_cache;
  GtkSwitch *switch_run_in_background;
  GtkLabel *label_rcon_host_hint;
  GtkLabel *label_rcon_port_hint;
  gboolean settings_dirty;
  gboolean settings_loading;
  gboolean settings_guard;
  gboolean settings_invalid;
  gboolean background_hold;
  char *last_details_page;
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

typedef struct {
  PumpkinWindow *self;
  char *src_path;
  char *dest_dir;
  char *dest_path;
} PluginOverwriteContext;

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

typedef struct {
  PumpkinWindow *self;
  PumpkinServer *server;
  char *host;
  int port;
} QueryPlayersContext;

typedef struct {
  int players;
  int max_players;
  gboolean ok;
} QueryResult;

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
static void update_save_button(PumpkinWindow *self);
static void update_save_button(PumpkinWindow *self);
static gboolean widget_has_label(GtkWidget *widget, const char *label);
static void disable_players_tab(PumpkinWindow *self);
static void get_system_limits(int *max_cores, int *max_ram_mb);
static void validate_settings_limits(PumpkinWindow *self);
static int parse_limit_entry(GtkEntry *entry, int max_value);
static gboolean parse_tps_from_line(const char *line, double *out);
static void on_settings_leave_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static gboolean on_window_close_request(GtkWindow *window, gpointer user_data);
static void on_window_visible_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
static gboolean query_minecraft_players(const char *host, int port, int *out_players, int *out_max_players);
static void on_send_command(GtkButton *button, PumpkinWindow *self);
static void on_choose_icon(GtkButton *button, PumpkinWindow *self);
static void on_reset_icon(GtkButton *button, PumpkinWindow *self);
static void on_plugin_delete_clicked(GtkButton *button, PumpkinWindow *self);
static void on_plugin_overwrite_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static void select_server(PumpkinWindow *self, PumpkinServer *server);
static void refresh_overview_list(PumpkinWindow *self);
static GtkWidget *create_server_row(PumpkinServer *server);
static GtkWidget *create_server_icon_widget(PumpkinServer *server);
static void update_details(PumpkinWindow *self);
static void refresh_plugin_list(PumpkinWindow *self);
static void on_world_delete_clicked(GtkButton *button, PumpkinWindow *self);
static void refresh_world_list(PumpkinWindow *self);
static void refresh_player_list(PumpkinWindow *self);
static void refresh_whitelist_list(PumpkinWindow *self);
static void refresh_banned_list(PumpkinWindow *self);
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
static gboolean on_plugins_drop(GtkDropTarget *target, const GValue *value, double x, double y, PumpkinWindow *self);
static gboolean on_worlds_drop(GtkDropTarget *target, const GValue *value, double x, double y, PumpkinWindow *self);
static GdkDragAction on_plugins_drop_enter(GtkDropTarget *target, double x, double y, PumpkinWindow *self);
static void on_plugins_drop_leave(GtkDropTarget *target, PumpkinWindow *self);
static GdkDragAction on_worlds_drop_enter(GtkDropTarget *target, double x, double y, PumpkinWindow *self);
static void on_worlds_drop_leave(GtkDropTarget *target, PumpkinWindow *self);
static void set_console_warning(PumpkinWindow *self, const char *message, gboolean visible);
static char *strip_ansi(const char *line);
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

static const char *
get_entry_text(GtkEntry *entry)
{
  if (entry == NULL) {
    return NULL;
  }
  return gtk_editable_get_text(GTK_EDITABLE(entry));
}

static int
get_entry_int_value(GtkEntry *entry)
{
  if (entry == NULL) {
    return 0;
  }
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (text == NULL || *text == '\0') {
    return 0;
  }
  return atoi(text);
}

static gboolean
strings_equal(const char *a, const char *b)
{
  const char *left = a != NULL ? a : "";
  const char *right = b != NULL ? b : "";
  return g_strcmp0(left, right) == 0;
}

static gboolean
entry_matches_string(GtkEntry *entry, const char *value)
{
  if (entry == NULL) {
    return TRUE;
  }
  return strings_equal(get_entry_text(entry), value);
}

static gboolean
entry_matches_int(GtkEntry *entry, int value)
{
  if (entry == NULL) {
    return TRUE;
  }
  return get_entry_int_value(entry) == value;
}

static gboolean
global_switches_match(PumpkinWindow *self)
{
  if (self->config == NULL) {
    return TRUE;
  }
  if (self->switch_use_cache != NULL &&
      pumpkin_config_get_use_cache(self->config) != gtk_switch_get_active(self->switch_use_cache)) {
    return FALSE;
  }
  if (self->switch_run_in_background != NULL &&
      pumpkin_config_get_run_in_background(self->config) != gtk_switch_get_active(self->switch_run_in_background)) {
    return FALSE;
  }
  return TRUE;
}

static gboolean
settings_match_server(PumpkinWindow *self)
{
  if (self->current == NULL) {
    return TRUE;
  }
  PumpkinServer *server = self->current;
  if (!entry_matches_string(self->entry_server_name, pumpkin_server_get_name(server))) {
    return FALSE;
  }
  if (!entry_matches_string(self->entry_download_url, pumpkin_server_get_download_url(server))) {
    return FALSE;
  }
  if (!entry_matches_int(self->entry_server_port, pumpkin_server_get_port(server))) {
    return FALSE;
  }
  if (!entry_matches_int(self->entry_bedrock_port, pumpkin_server_get_bedrock_port(server))) {
    return FALSE;
  }
  if (!entry_matches_int(self->entry_max_players, pumpkin_server_get_max_players(server))) {
    return FALSE;
  }
  int sys_cores = 0;
  int sys_ram_mb = 0;
  get_system_limits(&sys_cores, &sys_ram_mb);
  if (parse_limit_entry(self->entry_max_cpu_cores, sys_cores) != pumpkin_server_get_max_cpu_cores(server)) {
    return FALSE;
  }
  if (parse_limit_entry(self->entry_max_ram_mb, sys_ram_mb) != pumpkin_server_get_max_ram_mb(server)) {
    return FALSE;
  }
  if (self->switch_auto_restart != NULL &&
      pumpkin_server_get_auto_restart(server) != gtk_switch_get_active(self->switch_auto_restart)) {
    return FALSE;
  }
  if (!entry_matches_int(self->entry_auto_restart_delay, pumpkin_server_get_auto_restart_delay(server))) {
    return FALSE;
  }
  if (!entry_matches_string(self->entry_rcon_host, pumpkin_server_get_rcon_host(server))) {
    return FALSE;
  }
  if (!entry_matches_int(self->entry_rcon_port, pumpkin_server_get_rcon_port(server))) {
    return FALSE;
  }
  GtkEntry *rcon_password_entry = self->entry_rcon_password != NULL ? GTK_ENTRY(self->entry_rcon_password) : NULL;
  if (!entry_matches_string(rcon_password_entry, pumpkin_server_get_rcon_password(server))) {
    return FALSE;
  }
  return global_switches_match(self);
}

static gboolean
settings_match_config(PumpkinWindow *self)
{
  if (self->config == NULL) {
    return TRUE;
  }
  if (self->entry_download_url != NULL &&
      !entry_matches_string(self->entry_download_url, pumpkin_config_get_default_download_url(self->config))) {
    return FALSE;
  }
  return global_switches_match(self);
}

static gboolean
settings_still_match_saved(PumpkinWindow *self)
{
  if (self->current != NULL) {
    return settings_match_server(self);
  }
  return settings_match_config(self);
}

static void
mark_settings_dirty(PumpkinWindow *self)
{
  if (self->settings_loading) {
    return;
  }
  self->settings_dirty = !settings_still_match_saved(self);
  update_save_button(self);
}

static void
update_save_button(PumpkinWindow *self)
{
  if (self->btn_save_settings == NULL) {
    return;
  }
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings),
                           self->settings_dirty && !self->settings_invalid);
}

static void
discard_settings_changes(PumpkinWindow *self)
{
  self->settings_dirty = FALSE;
  self->settings_invalid = FALSE;
  if (self->btn_save_settings != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings), FALSE);
  }
  if (self->stats_row != NULL) {
    gtk_widget_set_visible(GTK_WIDGET(self->stats_row), TRUE);
  }
  if (self->plugin_drop_hint != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->plugin_drop_hint), FALSE);
  }
  if (self->world_drop_hint != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->world_drop_hint), FALSE);
  }
  update_settings_form(self);
}

static void
on_settings_changed(GtkEditable *editable, PumpkinWindow *self)
{
  (void)editable;
  mark_settings_dirty(self);
  validate_settings_limits(self);
}

static void
on_settings_switch_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self)
{
  (void)object;
  (void)pspec;
  if (self->switch_run_in_background != NULL &&
      GTK_WIDGET(object) == GTK_WIDGET(self->switch_run_in_background)) {
    mark_settings_dirty(self);
    validate_settings_limits(self);
    return;
  }
  mark_settings_dirty(self);
  validate_settings_limits(self);
}

static gboolean
parse_optional_positive_int(GtkEntry *entry, int *out_value, gboolean *has_value)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (has_value != NULL) {
    *has_value = FALSE;
  }
  if (text == NULL) {
    return TRUE;
  }
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  if (*text == '\0') {
    return TRUE;
  }
  if (has_value != NULL) {
    *has_value = TRUE;
  }
  char *endptr = NULL;
  long value = strtol(text, &endptr, 10);
  if (endptr == text) {
    return FALSE;
  }
  while (*endptr == ' ' || *endptr == '\t') {
    endptr++;
  }
  if (*endptr != '\0') {
    return FALSE;
  }
  if (value < 0 || value > INT_MAX) {
    return FALSE;
  }
  if (out_value != NULL) {
    *out_value = (int)value;
  }
  return TRUE;
}

static gboolean
query_is_fresh(PumpkinWindow *self)
{
  if (!self->query_valid) {
    return FALSE;
  }
  gint64 age = g_get_monotonic_time() - self->query_updated_at;
  return age >= 0 && age <= (QUERY_STALE_SECONDS * G_USEC_PER_SEC);
}

static void
query_players_context_free(QueryPlayersContext *ctx)
{
  if (ctx == NULL) {
    return;
  }
  if (ctx->self != NULL) {
    g_object_unref(ctx->self);
  }
  if (ctx->server != NULL) {
    g_object_unref(ctx->server);
  }
  g_clear_pointer(&ctx->host, g_free);
  g_free(ctx);
}

static void
query_players_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  (void)source_object;
  (void)cancellable;
  QueryPlayersContext *ctx = task_data;
  QueryResult *result = g_new0(QueryResult, 1);
  int players = 0;
  int max_players = 0;
  if (query_minecraft_players(ctx->host, ctx->port, &players, &max_players)) {
    result->ok = TRUE;
    result->players = players;
    result->max_players = max_players;
  }
  g_task_return_pointer(task, result, g_free);
}

static void
query_players_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  (void)source_object;
  QueryPlayersContext *ctx = user_data;
  PumpkinWindow *self = ctx != NULL ? ctx->self : NULL;
  if (self == NULL) {
    return;
  }

  self->query_in_flight = FALSE;
  g_autofree QueryResult *result = g_task_propagate_pointer(G_TASK(res), NULL);
  if (result == NULL || !result->ok) {
    return;
  }
  if (self->current != ctx->server) {
    return;
  }
  self->query_players = result->players;
  self->query_max_players = result->max_players;
  self->query_updated_at = g_get_monotonic_time();
  self->query_valid = TRUE;
}

static void
start_query_players(PumpkinWindow *self, PumpkinServer *server)
{
  if (self->query_in_flight || server == NULL) {
    return;
  }
  int port = pumpkin_server_get_port(server);
  if (port <= 0) {
    return;
  }
  self->query_in_flight = TRUE;
  QueryPlayersContext *ctx = g_new0(QueryPlayersContext, 1);
  ctx->self = g_object_ref(self);
  ctx->server = g_object_ref(server);
  ctx->host = g_strdup("127.0.0.1");
  ctx->port = port;

  GTask *task = g_task_new(self, NULL, query_players_done, ctx);
  g_task_set_task_data(task, ctx, (GDestroyNotify)query_players_context_free);
  g_task_run_in_thread(task, query_players_task);
  g_object_unref(task);
}

static const char *
skip_ws(const char *text)
{
  while (text != NULL && (*text == ' ' || *text == '\t')) {
    text++;
  }
  return text;
}

static void
validate_settings_limits(PumpkinWindow *self)
{
  if (self->entry_max_cpu_cores == NULL || self->entry_max_ram_mb == NULL ||
      self->entry_server_port == NULL || self->entry_bedrock_port == NULL) {
    return;
  }

  int sys_cores = 0;
  int sys_ram_mb = 0;
  get_system_limits(&sys_cores, &sys_ram_mb);

  gboolean cpu_has = FALSE;
  gboolean ram_has = FALSE;
  gboolean port_has = FALSE;
  gboolean bedrock_port_has = FALSE;
  gboolean players_has = FALSE;
  gboolean rcon_port_has = FALSE;
  int cpu_value = 0;
  int ram_value = 0;
  int port_value = 0;
  int bedrock_port_value = 0;
  int players_value = 0;
  int rcon_port_value = 0;
  gboolean cpu_parse_ok = parse_optional_positive_int(self->entry_max_cpu_cores, &cpu_value, &cpu_has);
  gboolean ram_parse_ok = parse_optional_positive_int(self->entry_max_ram_mb, &ram_value, &ram_has);
  gboolean port_parse_ok = parse_optional_positive_int(self->entry_server_port, &port_value, &port_has);
  gboolean bedrock_port_parse_ok =
    parse_optional_positive_int(self->entry_bedrock_port, &bedrock_port_value, &bedrock_port_has);
  gboolean players_parse_ok = parse_optional_positive_int(self->entry_max_players, &players_value, &players_has);
  gboolean rcon_port_parse_ok = parse_optional_positive_int(self->entry_rcon_port, &rcon_port_value, &rcon_port_has);

  gboolean cpu_invalid = FALSE;
  gboolean ram_invalid = FALSE;
  gboolean port_invalid = FALSE;
  gboolean bedrock_port_invalid = FALSE;
  gboolean players_invalid = FALSE;
  gboolean rcon_port_invalid = FALSE;
  gboolean rcon_host_invalid = FALSE;
  const char *cpu_hint = NULL;
  const char *ram_hint = NULL;
  const char *port_hint = NULL;
  const char *bedrock_port_hint = NULL;
  const char *players_hint = NULL;
  const char *rcon_port_hint = NULL;
  const char *rcon_host_hint = NULL;
  const char *cpu_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_cpu_cores)));
  const char *ram_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_ram_mb)));
  const char *port_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_server_port)));
  const char *bedrock_port_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_bedrock_port)));
  const char *players_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_players)));
  const char *rcon_port_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_port)));
  const char *rcon_host_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_host)));
  if (!cpu_parse_ok) {
    cpu_invalid = TRUE;
    if (cpu_text != NULL && *cpu_text == '-') {
      cpu_hint = "No negative values allowed.";
    }
  } else if (cpu_has && cpu_value > 0 && sys_cores > 0 && cpu_value > sys_cores) {
    cpu_invalid = TRUE;
    cpu_hint = "Above available cores.";
  }

  if (!ram_parse_ok) {
    ram_invalid = TRUE;
    if (ram_text != NULL && *ram_text == '-') {
      ram_hint = "No negative values allowed.";
    }
  } else if (ram_has && ram_value > 0 && sys_ram_mb > 0 && ram_value > sys_ram_mb) {
    ram_invalid = TRUE;
    ram_hint = "Above available RAM.";
  }

  if (!port_parse_ok) {
    port_invalid = TRUE;
    if (port_text != NULL && *port_text == '-') {
      port_hint = "No negative values allowed.";
    } else {
      port_hint = "Port must be a number.";
    }
  } else if (port_has && (port_value <= 0 || port_value > 65535)) {
    port_invalid = TRUE;
    port_hint = "Port must be 1–65535.";
  }

  if (!bedrock_port_parse_ok) {
    bedrock_port_invalid = TRUE;
    if (bedrock_port_text != NULL && *bedrock_port_text == '-') {
      bedrock_port_hint = "No negative values allowed.";
    } else {
      bedrock_port_hint = "Port must be a number.";
    }
  } else if (bedrock_port_has && (bedrock_port_value <= 0 || bedrock_port_value > 65535)) {
    bedrock_port_invalid = TRUE;
    bedrock_port_hint = "Port must be 1–65535.";
  }

  if (!players_parse_ok) {
    players_invalid = TRUE;
    if (players_text != NULL && *players_text == '-') {
      players_hint = "No negative values allowed.";
    } else {
      players_hint = "Max players must be a number.";
    }
  } else if (players_has && players_value <= 0) {
    players_invalid = TRUE;
    players_hint = "Max players must be at least 1.";
  }

  if (!rcon_port_parse_ok) {
    rcon_port_invalid = TRUE;
    if (rcon_port_text != NULL && *rcon_port_text == '-') {
      rcon_port_hint = "No negative values allowed.";
    } else {
      rcon_port_hint = "Port must be a number.";
    }
  } else if (rcon_port_has && (rcon_port_value <= 0 || rcon_port_value > 65535)) {
    rcon_port_invalid = TRUE;
    rcon_port_hint = "Port must be 1–65535.";
  }

  if (rcon_host_text != NULL && *rcon_host_text != '\0') {
    g_autoptr(GInetAddress) addr = g_inet_address_new_from_string(rcon_host_text);
    if (addr == NULL) {
      rcon_host_invalid = TRUE;
      rcon_host_hint = "Enter a valid IP address.";
    }
  }

  if (cpu_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_max_cpu_cores), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_max_cpu_cores), "error");
  }

  if (ram_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_max_ram_mb), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_max_ram_mb), "error");
  }

  if (port_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_server_port), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_server_port), "error");
  }

  if (bedrock_port_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_bedrock_port), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_bedrock_port), "error");
  }

  if (players_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_max_players), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_max_players), "error");
  }

  if (rcon_port_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_rcon_port), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_rcon_port), "error");
  }

  if (rcon_host_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_rcon_host), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_rcon_host), "error");
  }

  if (self->label_java_port_hint != NULL) {
    if (port_invalid) {
      gtk_label_set_text(self->label_java_port_hint, port_hint != NULL ? port_hint : "Port is invalid.");
      gtk_widget_set_visible(GTK_WIDGET(self->label_java_port_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_java_port_hint), FALSE);
    }
  }
  if (self->label_bedrock_port_hint != NULL) {
    if (bedrock_port_invalid) {
      gtk_label_set_text(self->label_bedrock_port_hint,
                         bedrock_port_hint != NULL ? bedrock_port_hint : "Port is invalid.");
      gtk_widget_set_visible(GTK_WIDGET(self->label_bedrock_port_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_bedrock_port_hint), FALSE);
    }
  }
  if (self->label_max_players_hint != NULL) {
    if (players_invalid) {
      gtk_label_set_text(self->label_max_players_hint,
                         players_hint != NULL ? players_hint : "Max players is invalid.");
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_players_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_players_hint), FALSE);
    }
  }
  if (self->label_rcon_port_hint != NULL) {
    if (rcon_port_invalid) {
      gtk_label_set_text(self->label_rcon_port_hint,
                         rcon_port_hint != NULL ? rcon_port_hint : "Port is invalid.");
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_port_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_port_hint), FALSE);
    }
  }
  if (self->label_rcon_host_hint != NULL) {
    if (rcon_host_invalid) {
      gtk_label_set_text(self->label_rcon_host_hint,
                         rcon_host_hint != NULL ? rcon_host_hint : "Host is invalid.");
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_host_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_host_hint), FALSE);
    }
  }

  if (self->label_max_cpu_hint != NULL) {
    if (cpu_invalid) {
      if (cpu_hint != NULL) {
        gtk_label_set_text(self->label_max_cpu_hint, cpu_hint);
      } else {
        g_autofree char *hint = g_strdup_printf("Max CPU cores available: %d.", sys_cores > 0 ? sys_cores : 0);
        gtk_label_set_text(self->label_max_cpu_hint, hint);
      }
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_cpu_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_cpu_hint), FALSE);
    }
  }
  if (self->label_max_ram_hint != NULL) {
    if (ram_invalid) {
      if (ram_hint != NULL) {
        gtk_label_set_text(self->label_max_ram_hint, ram_hint);
      } else {
        g_autofree char *hint = g_strdup_printf("Max RAM available: %d MB.", sys_ram_mb > 0 ? sys_ram_mb : 0);
        gtk_label_set_text(self->label_max_ram_hint, hint);
      }
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_ram_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_ram_hint), FALSE);
    }
  }

  self->settings_invalid = cpu_invalid || ram_invalid || port_invalid || bedrock_port_invalid ||
                           players_invalid || rcon_port_invalid || rcon_host_invalid;
  update_save_button(self);
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

static gboolean
on_window_close_request(GtkWindow *window, gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  gboolean run_in_background = FALSE;
  if (self->switch_run_in_background != NULL) {
    run_in_background = gtk_switch_get_active(self->switch_run_in_background);
  } else if (self->config != NULL) {
    run_in_background = pumpkin_config_get_run_in_background(self->config);
  }
  if (run_in_background) {
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    if (!self->background_hold) {
      GApplication *app = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
      if (app != NULL) {
        g_application_hold(app);
        self->background_hold = TRUE;
      }
    }
    return TRUE;
  }
  return FALSE;
}

static void
on_window_visible_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  if (gtk_widget_get_visible(GTK_WIDGET(object)) && self->background_hold) {
    GApplication *app = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
    if (app != NULL) {
      g_application_release(app);
    }
    self->background_hold = FALSE;
  }
}

static void
disable_players_tab(PumpkinWindow *self)
{
  if (self->details_switcher == NULL) {
    return;
  }
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->details_switcher));
  while (child != NULL) {
    if (widget_has_label(child, "Players")) {
      gtk_widget_set_sensitive(child, FALSE);
      gtk_widget_add_css_class(child, "stats-disabled");
      gtk_widget_set_tooltip_text(child, "Players disabled");
      break;
    }
    child = gtk_widget_get_next_sibling(child);
  }
}

static gboolean
widget_has_label(GtkWidget *widget, const char *label)
{
  if (widget == NULL || label == NULL) {
    return FALSE;
  }
  if (GTK_IS_LABEL(widget)) {
    const char *text = gtk_label_get_text(GTK_LABEL(widget));
    return text != NULL && g_strcmp0(text, label) == 0;
  }
  GtkWidget *child = gtk_widget_get_first_child(widget);
  while (child != NULL) {
    if (widget_has_label(child, label)) {
      return TRUE;
    }
    child = gtk_widget_get_next_sibling(child);
  }
  return FALSE;
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
  if (g_strcmp0(page, "players") == 0) {
    const char *fallback = self->last_details_page != NULL ? self->last_details_page : "console";
    self->settings_guard = TRUE;
    adw_view_stack_set_visible_child_name(self->details_stack, fallback);
    self->settings_guard = FALSE;
    return;
  }
  g_free(self->last_details_page);
  self->last_details_page = g_strdup(page);
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
  g_autofree char *clean = line != NULL ? strip_ansi(line) : NULL;
  const char *check = clean != NULL ? clean : line;
  if (check != NULL) {
    if (self->tps_enabled) {
      double tps = 0.0;
      if (parse_tps_from_line(check, &tps)) {
        self->last_tps = tps;
        self->last_tps_valid = TRUE;
      }
    }
  }
  if (check != NULL && self->current == server && self->ui_state == UI_STATE_STARTING) {
    if (strstr(check, "Server is now running") != NULL ||
        strstr(check, "Started server") != NULL ||
        strstr(check, "Server query running on port") != NULL ||
        strstr(check, "Server is running") != NULL ||
        strstr(check, "Done") != NULL ||
        strstr(check, "Listening") != NULL) {
      self->ui_state = UI_STATE_RUNNING;
      update_details(self);
      refresh_plugin_list(self);
      refresh_world_list(self);
      refresh_player_list(self);
      refresh_log_files(self);
    }
  }
  if (line != NULL && g_strcmp0(line, "Server process exited") == 0) {
    if (self->current == server && !self->restart_requested) {
      if (!self->user_stop_requested) {
        set_console_warning(self, "Server stopped unexpectedly.", TRUE);
      } else {
        set_console_warning(self, NULL, FALSE);
      }
      self->user_stop_requested = FALSE;
      self->ui_state = UI_STATE_IDLE;
      update_details(self);
      refresh_overview_list(self);
      refresh_world_list(self);
    }
    if (self->current == server && self->restart_requested && self->restart_pending) {
      self->restart_pending = FALSE;
      RestartContext *ctx = g_new0(RestartContext, 1);
      ctx->self = g_object_ref(self);
      ctx->server = g_object_ref(server);
      if (self->restart_delay_id != 0) {
        g_source_remove(self->restart_delay_id);
      }
      self->restart_delay_id = g_timeout_add(0, restart_after_delay, ctx);
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

static int
get_overview_player_count(PumpkinWindow *self, PumpkinServer *server)
{
  if (self != NULL && server != NULL && server == self->current && query_is_fresh(self)) {
    return self->query_players;
  }
  return get_player_count(server);
}

static gboolean
query_minecraft_players(const char *host, int port, int *out_players, int *out_max_players)
{
  if (host == NULL || port <= 0 || out_players == NULL || out_max_players == NULL) {
    return FALSE;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
                                           G_SOCKET_PROTOCOL_UDP, &error);
  if (socket == NULL) {
    return FALSE;
  }
  g_socket_set_timeout(socket, 1);

  g_autoptr(GInetAddress) addr = g_inet_address_new_from_string(host);
  if (addr == NULL) {
    addr = g_inet_address_new_from_string("127.0.0.1");
  }
  g_autoptr(GSocketAddress) saddr = g_inet_socket_address_new(addr, port);

  guint32 session_id = g_random_int();
  guint8 handshake[7] = {
    0xFE, 0xFD, 0x09,
    (guint8)((session_id >> 24) & 0xFF),
    (guint8)((session_id >> 16) & 0xFF),
    (guint8)((session_id >> 8) & 0xFF),
    (guint8)(session_id & 0xFF)
  };

  if (g_socket_send_to(socket, saddr, (const gchar *)handshake, sizeof(handshake), NULL, &error) < 0) {
    return FALSE;
  }

  guint8 buffer[2048];
  gssize received = g_socket_receive_from(socket, NULL, (gchar *)buffer, sizeof(buffer) - 1, NULL, &error);
  if (received < 6) {
    return FALSE;
  }
  buffer[received] = '\0';
  if (buffer[0] != 0x09) {
    return FALSE;
  }

  char *token_str = (char *)&buffer[5];
  long token_long = strtol(token_str, NULL, 10);
  gint32 token = (gint32)token_long;

  guint8 stat_req[11] = {
    0xFE, 0xFD, 0x00,
    (guint8)((session_id >> 24) & 0xFF),
    (guint8)((session_id >> 16) & 0xFF),
    (guint8)((session_id >> 8) & 0xFF),
    (guint8)(session_id & 0xFF),
    (guint8)((token >> 24) & 0xFF),
    (guint8)((token >> 16) & 0xFF),
    (guint8)((token >> 8) & 0xFF),
    (guint8)(token & 0xFF)
  };

  if (g_socket_send_to(socket, saddr, (const gchar *)stat_req, sizeof(stat_req), NULL, &error) < 0) {
    return FALSE;
  }

  received = g_socket_receive_from(socket, NULL, (gchar *)buffer, sizeof(buffer) - 1, NULL, &error);
  if (received < 6) {
    return FALSE;
  }
  buffer[received] = '\0';
  if (buffer[0] != 0x00) {
    return FALSE;
  }

  int players = -1;
  int max_players = -1;
  char *ptr = (char *)&buffer[5];
  char *end = (char *)&buffer[received];
  while (ptr < end && *ptr != '\0') {
    char *key = ptr;
    size_t key_len = strlen(key);
    ptr += key_len + 1;
    if (ptr >= end) {
      break;
    }
    char *value = ptr;
    size_t val_len = strlen(value);
    ptr += val_len + 1;

    if (g_strcmp0(key, "numplayers") == 0) {
      players = (int)strtol(value, NULL, 10);
    } else if (g_strcmp0(key, "maxplayers") == 0) {
      max_players = (int)strtol(value, NULL, 10);
    }
  }

  if (players < 0) {
    return FALSE;
  }
  *out_players = players;
  *out_max_players = max_players > 0 ? max_players : 0;
  return TRUE;
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

typedef struct {
  char *name;
  char *uuid;
} PlayerEntry;

static void
player_entry_free(PlayerEntry *entry)
{
  if (entry == NULL) {
    return;
  }
  g_clear_pointer(&entry->name, g_free);
  g_clear_pointer(&entry->uuid, g_free);
  g_free(entry);
}

static char *
resolve_data_file(PumpkinServer *server, const char *filename)
{
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  g_autofree char *path = g_build_filename(data_dir, "data", filename, NULL);
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    return g_strdup(path);
  }
  g_autofree char *fallback = g_build_filename(data_dir, filename, NULL);
  if (g_file_test(fallback, G_FILE_TEST_EXISTS)) {
    return g_strdup(fallback);
  }
  return g_strdup(path);
}

static GPtrArray *
load_player_entries_from_file(const char *path)
{
  if (path == NULL) {
    return NULL;
  }

  g_autofree char *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL)) {
    return NULL;
  }

  g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func((GDestroyNotify)player_entry_free);
  g_autoptr(GRegex) regex = g_regex_new(
    "\"uuid\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"name\"\\s*:\\s*\"([^\"]+)\"|\"name\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"uuid\"\\s*:\\s*\"([^\"]+)\"",
    G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, contents, 0, &match)) {
    return g_steal_pointer(&entries);
  }

  while (g_match_info_matches(match)) {
    g_autofree char *uuid1 = g_match_info_fetch(match, 1);
    g_autofree char *name1 = g_match_info_fetch(match, 2);
    g_autofree char *name2 = g_match_info_fetch(match, 3);
    g_autofree char *uuid2 = g_match_info_fetch(match, 4);

    const char *uuid = NULL;
    const char *name = NULL;
    if (uuid1 != NULL && name1 != NULL) {
      uuid = uuid1;
      name = name1;
    } else if (uuid2 != NULL && name2 != NULL) {
      uuid = uuid2;
      name = name2;
    }

    if (name != NULL && *name != '\0') {
      PlayerEntry *entry = g_new0(PlayerEntry, 1);
      entry->name = g_strdup(name);
      entry->uuid = (uuid != NULL && *uuid != '\0') ? g_strdup(uuid) : NULL;
      g_ptr_array_add(entries, entry);
    }

    g_match_info_next(match, NULL);
  }

  return g_steal_pointer(&entries);
}

static void
load_player_name_map(GHashTable *map, PumpkinServer *server)
{
  const char *files[] = {
    "usercache.json",
    "whitelist.json",
    "ops.json",
    "banned-players.json",
    NULL
  };

  for (int i = 0; files[i] != NULL; i++) {
    g_autofree char *path = resolve_data_file(server, files[i]);
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
    int players = get_overview_player_count(self, server);
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
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  const char *file = g_object_get_data(G_OBJECT(sw), "plugin-file");
  const char *dir_path = g_object_get_data(G_OBJECT(sw), "plugin-dir");

  if (file == NULL || dir_path == NULL) {
    return FALSE;
  }
  if (self != NULL &&
      (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_RESTARTING)) {
    gtk_switch_set_active(sw, !state);
    return TRUE;
  }

  g_autofree char *from = g_build_filename(dir_path, file, NULL);
  g_autofree char *to = NULL;

  if (state) {
    if (g_str_has_suffix(file, ".disabled") || g_str_has_suffix(file, ".deactivated")) {
      g_autofree char *base = g_strdup(file);
      if (g_str_has_suffix(base, ".disabled")) {
        base[strlen(base) - strlen(".disabled")] = '\0';
      } else if (g_str_has_suffix(base, ".deactivated")) {
        base[strlen(base) - strlen(".deactivated")] = '\0';
      }
      to = g_build_filename(dir_path, base, NULL);
    }
  } else {
    if (!g_str_has_suffix(file, ".disabled") && !g_str_has_suffix(file, ".deactivated")) {
      g_autofree char *disabled = g_strconcat(file, ".deactivated", NULL);
      to = g_build_filename(dir_path, disabled, NULL);
    }
  }

  if (to != NULL) {
    if (g_rename(from, to) != 0) {
      if (self != NULL) {
        g_autofree char *msg = g_strdup_printf("Failed to toggle plugin: %s", g_strerror(errno));
        append_log(self, msg);
      }
      return TRUE;
    }
    if (self != NULL) {
      refresh_plugin_list(self);
    }
  }

  return FALSE;
}

static void
on_plugin_delete_clicked(GtkButton *button, PumpkinWindow *self)
{
  if (self->current == NULL) {
    return;
  }
  if (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_RESTARTING) {
    return;
  }

  const char *file = g_object_get_data(G_OBJECT(button), "plugin-file");
  const char *dir_path = g_object_get_data(G_OBJECT(button), "plugin-dir");
  if (file == NULL || dir_path == NULL) {
    return;
  }

  g_autofree char *path = g_build_filename(dir_path, file, NULL);
  if (g_remove(path) != 0) {
    append_log(self, "Failed to delete plugin.");
    return;
  }

  refresh_plugin_list(self);
}

static void
plugin_overwrite_context_free(PluginOverwriteContext *ctx)
{
  if (ctx == NULL) {
    return;
  }
  g_clear_object(&ctx->self);
  g_clear_pointer(&ctx->src_path, g_free);
  g_clear_pointer(&ctx->dest_dir, g_free);
  g_clear_pointer(&ctx->dest_path, g_free);
  g_free(ctx);
}

static void
on_plugin_overwrite_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
  if (response == NULL || g_strcmp0(response, "cancel") == 0) {
    return;
  }

  PluginOverwriteContext *ctx = g_object_get_data(G_OBJECT(dialog), "plugin-overwrite-ctx");
  if (ctx == NULL || ctx->self == NULL || ctx->src_path == NULL || ctx->dest_path == NULL) {
    return;
  }

  g_autoptr(GFile) src_file = g_file_new_for_path(ctx->src_path);
  g_autoptr(GFile) dest_file = g_file_new_for_path(ctx->dest_path);
  g_autoptr(GError) error = NULL;
  if (!g_file_copy(src_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error)) {
    if (error != NULL) {
      append_log(ctx->self, error->message);
    }
    return;
  }

  refresh_plugin_list(ctx->self);
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
  } else if (g_str_has_suffix(clean, ".deactivated")) {
    clean[strlen(clean) - strlen(".deactivated")] = '\0';
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
  gboolean can_start = installed && !running && !busy &&
                       (self->ui_state == UI_STATE_IDLE || self->ui_state == UI_STATE_ERROR);
  gboolean can_stop = running && self->ui_state == UI_STATE_RUNNING;
  gboolean can_restart = running && self->ui_state == UI_STATE_RUNNING &&
                         self->restart_delay_id == 0;

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_start), can_start);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_stop), can_stop);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_restart), can_restart);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_install), !running && !busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_update), update_available && !running && !busy);
  gtk_widget_set_visible(GTK_WIDGET(self->btn_details_update), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_check_updates), installed && !running && !busy);
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
    self->user_stop_requested = FALSE;
    set_console_warning(self, NULL, FALSE);
  }

  if (self->current == server) {
    update_details(self);
  }
  refresh_overview_list(self);
  if (self->current == server) {
    refresh_plugin_list(self);
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
  refresh_plugin_list(self);
  refresh_world_list(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static gboolean
read_system_cpu(unsigned long long *total, unsigned long long *idle)
{
#if defined(G_OS_WIN32)
  FILETIME idle_time, kernel_time, user_time;
  if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
    return FALSE;
  }
  ULARGE_INTEGER idle_ul = { .LowPart = idle_time.dwLowDateTime, .HighPart = idle_time.dwHighDateTime };
  ULARGE_INTEGER kernel_ul = { .LowPart = kernel_time.dwLowDateTime, .HighPart = kernel_time.dwHighDateTime };
  ULARGE_INTEGER user_ul = { .LowPart = user_time.dwLowDateTime, .HighPart = user_time.dwHighDateTime };
  *idle = idle_ul.QuadPart;
  *total = kernel_ul.QuadPart + user_ul.QuadPart;
  return TRUE;
#elif defined(__APPLE__)
  natural_t cpu_count = 0;
  processor_info_array_t info_array = NULL;
  mach_msg_type_number_t info_count = 0;
  kern_return_t kr = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                         &cpu_count, &info_array, &info_count);
  if (kr != KERN_SUCCESS || info_array == NULL) {
    return FALSE;
  }
  unsigned long long user = 0, system = 0, nice = 0, idle_time = 0;
  for (natural_t i = 0; i < cpu_count; i++) {
    unsigned long long *cpu = (unsigned long long *)(info_array + (CPU_STATE_MAX * i));
    user += cpu[CPU_STATE_USER];
    system += cpu[CPU_STATE_SYSTEM];
    nice += cpu[CPU_STATE_NICE];
    idle_time += cpu[CPU_STATE_IDLE];
  }
  vm_deallocate(mach_task_self(), (vm_address_t)info_array,
                (vm_size_t)(info_count * sizeof(integer_t)));
  *idle = idle_time;
  *total = user + system + nice + idle_time;
  return TRUE;
#else
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
#endif
}

static gboolean
read_system_mem(unsigned long long *total_bytes, unsigned long long *avail_bytes)
{
#if defined(G_OS_WIN32)
  MEMORYSTATUSEX mem = { 0 };
  mem.dwLength = sizeof(mem);
  if (!GlobalMemoryStatusEx(&mem)) {
    return FALSE;
  }
  *total_bytes = mem.ullTotalPhys;
  *avail_bytes = mem.ullAvailPhys;
  return TRUE;
#elif defined(__APPLE__)
  uint64_t total = 0;
  size_t total_len = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &total_len, NULL, 0) != 0) {
    return FALSE;
  }
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  vm_statistics64_data_t vmstat;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        (host_info64_t)&vmstat, &count) != KERN_SUCCESS) {
    return FALSE;
  }
  vm_size_t page_size = 0;
  host_page_size(mach_host_self(), &page_size);
  uint64_t free_bytes = (uint64_t)vmstat.free_count * page_size;
  uint64_t inactive_bytes = (uint64_t)vmstat.inactive_count * page_size;
  *total_bytes = total;
  *avail_bytes = free_bytes + inactive_bytes;
  return TRUE;
#else
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
#endif
}

static int
stats_sample_index(PumpkinWindow *self, int offset)
{
  int start = self->stats_index - self->stats_count;
  if (start < 0) {
    start += STATS_SAMPLES;
  }
  int idx = start + offset;
  if (idx >= STATS_SAMPLES) {
    idx -= STATS_SAMPLES;
  }
  return idx;
}

static double
stats_get_sample(PumpkinWindow *self, double *series, int offset)
{
  if (self->stats_count == 0) {
    return 0.0;
  }
  if (offset < 0 || offset >= self->stats_count) {
    return 0.0;
  }
  int idx = stats_sample_index(self, offset);
  return series[idx];
}

static double
stats_get_smoothed(PumpkinWindow *self, double *series, int offset, int window)
{
  if (self->stats_count == 0) {
    return 0.0;
  }
  int start = offset - (window - 1);
  if (start < 0) {
    start = 0;
  }
  double sum = 0.0;
  int count = 0;
  for (int i = start; i <= offset; i++) {
    sum += stats_get_sample(self, series, i);
    count++;
  }
  return count > 0 ? sum / (double)count : 0.0;
}

static void
draw_stats_series(PumpkinWindow *self, cairo_t *cr, double *series, int total_samples, int valid_count,
                  double max_value, double r, double g, double b,
                  double left, double top, double right, double bottom,
                  double width, double height)
{
  if (valid_count < 2 || max_value <= 0.0 || total_samples < 2) {
    return;
  }
  cairo_set_source_rgb(cr, r, g, b);
  cairo_set_line_width(cr, 2.0);

  double graph_w = width - left - right;
  double graph_h = height - top - bottom;
  int window = 5;
  int start = total_samples - valid_count;
  if (start < 0) {
    start = 0;
  }
  gboolean started = FALSE;
  for (int i = start; i < total_samples; i++) {
    int offset = i - start;
    double raw = stats_get_smoothed(self, series, offset, window);
    double val = raw / max_value;
    if (val < 0.0) {
      val = 0.0;
    } else if (val > 1.0) {
      val = 1.0;
    }
    double x = left + ((double)i / (double)(total_samples - 1)) * graph_w;
    double y = top + (1.0 - val) * graph_h;
    if (!started) {
      cairo_move_to(cr, x, y);
      started = TRUE;
    } else {
      cairo_line_to(cr, x, y);
    }
  }
  cairo_stroke(cr);
}

static void
draw_stats_grid(cairo_t *cr, double left, double top, double right, double bottom,
                int width, int height, int rows)
{
  double graph_w = width - left - right;
  double graph_h = height - top - bottom;

  cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
  cairo_set_line_width(cr, 1.0);
  for (int i = 1; i < rows; i++) {
    double y = top + (graph_h / (double)rows) * i;
    cairo_move_to(cr, left, y);
    cairo_line_to(cr, left + graph_w, y);
  }
  cairo_stroke(cr);
}

static int
usage_scale_percent(PumpkinWindow *self)
{
  double max_val = 0.0;
  for (int i = 0; i < self->stats_count; i++) {
    double cpu = stats_get_sample(self, self->stats_cpu, i);
    double ram = stats_get_sample(self, self->stats_ram_mb, i);
    if (cpu > max_val) {
      max_val = cpu;
    }
    if (ram > max_val) {
      max_val = ram;
    }
  }
  int scale = (int)(ceil(max_val / 10.0) * 10.0);
  if (scale < 5) {
    scale = 5;
  } else if (scale < 10) {
    scale = 10;
  }
  if (scale > 100) {
    scale = 100;
  }
  return scale;
}

static gboolean
parse_tps_from_line(const char *line, double *out)
{
  if (line == NULL || out == NULL) {
    return FALSE;
  }
  static GRegex *regex = NULL;
  if (regex == NULL) {
    regex = g_regex_new("tps[^0-9]*([0-9]+(\\.[0-9]+)?)", G_REGEX_CASELESS, 0, NULL);
  }
  if (regex == NULL) {
    return FALSE;
  }
  GMatchInfo *match_info = NULL;
  gboolean matched = g_regex_match(regex, line, 0, &match_info);
  if (!matched || match_info == NULL) {
    if (match_info != NULL) {
      g_match_info_free(match_info);
    }
    return FALSE;
  }
  g_autofree char *num = g_match_info_fetch(match_info, 1);
  g_match_info_free(match_info);
  if (num == NULL) {
    return FALSE;
  }
  char *endptr = NULL;
  double val = g_ascii_strtod(num, &endptr);
  if (endptr == num) {
    return FALSE;
  }
  *out = val;
  return TRUE;
}

static void
draw_stats_axis_labels(cairo_t *cr, double left, double top, double right, double bottom,
                       int width, int height, int rows, const char *top_label,
                       const char *mid_label, const char *bottom_label)
{
  (void)right;
  (void)width;
  double graph_h = height - top - bottom;
  cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11.0);

  double y_top = top + 2.0;
  double y_mid = top + (graph_h / (double)rows) * (rows / 2.0) + 2.0;
  double y_bottom = top + graph_h + 2.0;

  cairo_move_to(cr, left - 36.0, y_top);
  cairo_show_text(cr, top_label);
  cairo_move_to(cr, left - 36.0, y_mid);
  cairo_show_text(cr, mid_label);
  cairo_move_to(cr, left - 36.0, y_bottom);
  cairo_show_text(cr, bottom_label);
}

static void
draw_time_axis_labels(cairo_t *cr, double left, double top, double right, double bottom,
                      int width, int height)
{
  double graph_h = height - top - bottom;
  double y = top + graph_h + 14.0;
  double graph_w = width - left - right;
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11.0);

  const int labels[] = {180, 150, 120, 90, 60, 30, 0};
  for (int i = 0; i < 7; i++) {
    double x = left + graph_w * (1.0 - (double)labels[i] / 180.0);
    g_autofree char *text = g_strdup_printf("%ds", labels[i]);
    cairo_move_to(cr, x - 10.0, y);
    cairo_show_text(cr, text);
  }
}

static void
stats_graph_draw_usage(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  (void)area;

  cairo_set_source_rgb(cr, 0.11, 0.11, 0.12);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (self->stats_count < 2) {
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 12.0, 20.0);
    cairo_show_text(cr, "Waiting for data…");
    return;
  }

  double left = 44.0;
  double top = 12.0;
  double right = 12.0;
  double bottom = 16.0;

  int scale = usage_scale_percent(self);
  int rows = scale == 5 ? 1 : scale / 10;
  g_autofree char *top_label = g_strdup_printf("%d%%", scale);
  g_autofree char *mid_label = g_strdup_printf("%d%%", scale == 5 ? 0 : scale / 2);
  draw_stats_grid(cr, left, top, right, bottom, width, height, rows);
  draw_stats_axis_labels(cr, left, top, right, bottom, width, height, rows,
                         top_label, mid_label, "0%");
  draw_time_axis_labels(cr, left, top, right, bottom, width, height);

  draw_stats_series(self, cr, self->stats_cpu, STATS_SAMPLES, self->stats_count, (double)scale,
                    0.93, 0.33, 0.33, left, top, right, bottom, width, height);
  draw_stats_series(self, cr, self->stats_ram_mb, STATS_SAMPLES, self->stats_count, (double)scale,
                    0.33, 0.55, 0.93, left, top, right, bottom, width, height);
}

static void
stats_graph_draw_players(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  (void)area;

  cairo_set_source_rgb(cr, 0.11, 0.11, 0.12);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (self->stats_count < 2) {
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 12.0, 20.0);
    cairo_show_text(cr, "Waiting for data…");
    return;
  }

  double left = 44.0;
  double top = 12.0;
  double right = 12.0;
  double bottom = 16.0;

  draw_stats_grid(cr, left, top, right, bottom, width, height, 4);

  double players_max = 1.0;
  if (self->current != NULL) {
    int max_players = pumpkin_server_get_max_players(self->current);
    if (query_is_fresh(self) && self->query_max_players > 0) {
      max_players = self->query_max_players;
    }
    if (max_players > 0) {
      players_max = (double)max_players;
    }
  }
  if (players_max <= 1.0) {
    double max_seen = 0.0;
    for (int i = 0; i < self->stats_count; i++) {
      max_seen = fmax(max_seen, stats_get_sample(self, self->stats_players, i));
    }
    players_max = max_seen > 0.0 ? max_seen : 1.0;
  }

  g_autofree char *top_label = g_strdup_printf("%d", (int)players_max);
  g_autofree char *mid_label = g_strdup_printf("%d", (int)(players_max / 2.0));
  draw_stats_axis_labels(cr, left, top, right, bottom, width, height, 4,
                         top_label, mid_label, "0");
  draw_time_axis_labels(cr, left, top, right, bottom, width, height);

  draw_stats_series(self, cr, self->stats_players, STATS_SAMPLES, self->stats_count, players_max,
                    0.95, 0.66, 0.26, left, top, right, bottom, width, height);
}

static void
stats_graph_draw_disk(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  (void)area;

  if (!self->tps_enabled) {
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.12);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 12.0, 20.0);
    cairo_show_text(cr, "TPS disabled");
    return;
  }

  cairo_set_source_rgb(cr, 0.11, 0.11, 0.12);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if (self->stats_count < 2) {
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 12.0, 20.0);
    cairo_show_text(cr, "Waiting for data…");
    return;
  }

  double left = 44.0;
  double top = 12.0;
  double right = 12.0;
  double bottom = 16.0;

  draw_stats_grid(cr, left, top, right, bottom, width, height, 4);
  draw_stats_axis_labels(cr, left, top, right, bottom, width, height, 4, "20", "10", "0");
  draw_time_axis_labels(cr, left, top, right, bottom, width, height);

  draw_stats_series(self, cr, self->stats_disk_mb, STATS_SAMPLES, self->stats_count, 20.0,
                    0.35, 0.77, 0.45, left, top, right, bottom, width, height);
}

static void
get_system_limits(int *max_cores, int *max_ram_mb)
{
  if (max_cores != NULL) {
    int cores = g_get_num_processors();
    *max_cores = cores > 0 ? cores : 0;
  }
  if (max_ram_mb != NULL) {
    unsigned long long total = 0;
    unsigned long long avail = 0;
    if (read_system_mem(&total, &avail)) {
      *max_ram_mb = (int)(total / (1024ULL * 1024ULL));
    } else {
      *max_ram_mb = 0;
    }
  }
}

static void
reset_stats_history(PumpkinWindow *self)
{
  self->stats_index = 0;
  self->stats_count = 0;
  memset(self->stats_cpu, 0, sizeof(self->stats_cpu));
  memset(self->stats_ram_mb, 0, sizeof(self->stats_ram_mb));
  memset(self->stats_disk_mb, 0, sizeof(self->stats_disk_mb));
  memset(self->stats_players, 0, sizeof(self->stats_players));
  self->last_tps = 0.0;
  self->last_tps_valid = FALSE;
  self->tps_enabled = FALSE;
}

static gboolean
read_process_stats(int pid, unsigned long long *proc_ticks, unsigned long long *rss_bytes)
{
#if defined(G_OS_WIN32)
  if (pid <= 0) {
    return FALSE;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
  if (process == NULL) {
    return FALSE;
  }

  FILETIME create_time, exit_time, kernel_time, user_time;
  if (!GetProcessTimes(process, &create_time, &exit_time, &kernel_time, &user_time)) {
    CloseHandle(process);
    return FALSE;
  }
  ULARGE_INTEGER kernel_ul = { .LowPart = kernel_time.dwLowDateTime, .HighPart = kernel_time.dwHighDateTime };
  ULARGE_INTEGER user_ul = { .LowPart = user_time.dwLowDateTime, .HighPart = user_time.dwHighDateTime };
  *proc_ticks = kernel_ul.QuadPart + user_ul.QuadPart;

  PROCESS_MEMORY_COUNTERS_EX mem = { 0 };
  mem.cb = sizeof(mem);
  if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&mem, sizeof(mem))) {
    *rss_bytes = (unsigned long long)mem.WorkingSetSize;
  }

  CloseHandle(process);
  return TRUE;
#elif defined(__APPLE__)
  if (pid <= 0) {
    return FALSE;
  }
  struct proc_taskinfo info;
  int ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info));
  if (ret <= 0) {
    return FALSE;
  }
  long ticks = sysconf(_SC_CLK_TCK);
  if (ticks <= 0) {
    ticks = 100;
  }
  unsigned long long total_ns = info.pti_total_user + info.pti_total_system;
  *proc_ticks = (total_ns * (unsigned long long)ticks) / 1000000000ULL;
  *rss_bytes = (unsigned long long)info.pti_resident_size;
  return TRUE;
#else
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
#endif
}

static gboolean
update_stats_tick(gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  if (self->label_sys_cpu == NULL || self->label_sys_ram == NULL ||
      self->label_srv_cpu == NULL || self->label_srv_ram == NULL) {
    return G_SOURCE_CONTINUE;
  }
  gboolean server_running = self->current != NULL && pumpkin_server_get_running(self->current);
  gtk_widget_set_visible(GTK_WIDGET(self->label_srv_cpu), server_running);
  gtk_widget_set_visible(GTK_WIDGET(self->label_srv_ram), server_running);

  unsigned long long total = 0, idle = 0;
  unsigned long long mem_total = 0, mem_avail = 0;
  double sys_cpu = 0.0;
  unsigned long long prev_total = self->last_total_jiffies;
  unsigned long long prev_idle = self->last_idle_jiffies;

  gboolean has_system_cpu = read_system_cpu(&total, &idle);
  if (has_system_cpu) {
    if (prev_total != 0) {
      unsigned long long delta_total = total - prev_total;
      unsigned long long delta_idle = idle - prev_idle;
      if (delta_total > 0) {
        sys_cpu = (double)(delta_total - delta_idle) / (double)delta_total * 100.0;
      }
    }
    self->last_total_jiffies = total;
    self->last_idle_jiffies = idle;
  } else {
    self->last_total_jiffies = 0;
    self->last_idle_jiffies = 0;
  }

  if (read_system_mem(&mem_total, &mem_avail)) {
    unsigned long long used = mem_total - mem_avail;
    g_autofree char *used_str = g_format_size_full(used, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *total_str = g_format_size_full(mem_total, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *ram = g_strdup_printf("System RAM: %s / %s", used_str, total_str);
    gtk_label_set_text(self->label_sys_ram, ram);
  } else {
    gtk_label_set_text(self->label_sys_ram, "System RAM: --");
  }

  if (has_system_cpu) {
    g_autofree char *cpu = g_strdup_printf("System CPU: %.1f%%", sys_cpu);
    gtk_label_set_text(self->label_sys_cpu, cpu);
  } else {
    gtk_label_set_text(self->label_sys_cpu, "System CPU: --");
  }

  double proc_cpu = 0.0;
  unsigned long long proc_ticks = 0;
  unsigned long long rss = 0;
  int pid = 0;
  int players_count = 0;
  int max_ram_mb = 0;
  int max_players = 0;
  if (self->current != NULL) {
    pid = pumpkin_server_get_pid(self->current);
    max_ram_mb = pumpkin_server_get_max_ram_mb(self->current);
    max_players = pumpkin_server_get_max_players(self->current);
    if (pumpkin_server_get_running(self->current)) {
      if (!self->query_in_flight && !query_is_fresh(self)) {
        start_query_players(self, self->current);
      }
    } else {
      self->query_valid = FALSE;
    }
    if (query_is_fresh(self)) {
      players_count = self->query_players;
      if (self->query_max_players > 0) {
        max_players = self->query_max_players;
      }
    } else if (self->live_player_names != NULL) {
      players_count = (int)g_hash_table_size(self->live_player_names);
    }
  }

  if (pid != self->last_proc_pid) {
    self->last_proc_jiffies = 0;
    self->last_proc_pid = pid;
  }

  if (has_system_cpu && pid > 0 && read_process_stats(pid, &proc_ticks, &rss) && prev_total != 0) {
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
    if (pid == 0) {
      self->last_proc_pid = 0;
    }
  }

  if (server_running && pid > 0 && rss > 0) {
    g_autofree char *rss_str = g_format_size_full(rss, G_FORMAT_SIZE_IEC_UNITS);
    g_autofree char *ram = g_strdup_printf("Pumpkin RAM: %s", rss_str);
    gtk_label_set_text(self->label_srv_ram, ram);
  } else if (!server_running) {
    gtk_label_set_text(self->label_srv_ram, "");
  } else {
    gtk_label_set_text(self->label_srv_ram, "Pumpkin RAM: --");
  }

  if (server_running && pid > 0) {
    g_autofree char *pcpu = g_strdup_printf("Pumpkin CPU: %.1f%%", proc_cpu);
    gtk_label_set_text(self->label_srv_cpu, pcpu);
  } else if (!server_running) {
    gtk_label_set_text(self->label_srv_cpu, "");
  } else {
    gtk_label_set_text(self->label_srv_cpu, "Pumpkin CPU: --");
  }

  if (self->tps_enabled) {
    double tps_value = self->last_tps_valid ? self->last_tps : 0.0;
    if (tps_value < 0.0) {
      tps_value = 0.0;
    } else if (tps_value > 20.0) {
      tps_value = 20.0;
    }
    self->stats_disk_mb[self->stats_index] = tps_value;
  }

  double rss_mb = (rss > 0) ? (double)rss / (1024.0 * 1024.0) : 0.0;
  double ram_limit_mb = 0.0;
  if (max_ram_mb > 0) {
    ram_limit_mb = (double)max_ram_mb;
  } else if (mem_total > 0) {
    ram_limit_mb = (double)mem_total / (1024.0 * 1024.0);
  }
  double ram_pct = 0.0;
  if (ram_limit_mb > 0.0) {
    ram_pct = (rss_mb / ram_limit_mb) * 100.0;
  }
  if (ram_pct > 100.0) {
    ram_pct = 100.0;
  } else if (ram_pct < 0.0) {
    ram_pct = 0.0;
  }
  self->stats_cpu[self->stats_index] = proc_cpu;
  self->stats_ram_mb[self->stats_index] = ram_pct;
  self->stats_players[self->stats_index] = (double)players_count;
  self->stats_index = (self->stats_index + 1) % STATS_SAMPLES;
  if (self->stats_count < STATS_SAMPLES) {
    self->stats_count++;
  }

  if (self->stats_graph_usage != NULL) {
    gtk_widget_queue_draw(GTK_WIDGET(self->stats_graph_usage));
  }
  if (self->stats_graph_players != NULL) {
    gtk_widget_queue_draw(GTK_WIDGET(self->stats_graph_players));
  }
  if (self->stats_graph_disk != NULL) {
    gtk_widget_queue_draw(GTK_WIDGET(self->stats_graph_disk));
  }

  if (self->label_stats_cpu != NULL) {
    g_autofree char *val = g_strdup_printf("CPU %.1f%%", proc_cpu);
    gtk_label_set_text(self->label_stats_cpu, val);
  }
  if (self->label_stats_ram != NULL) {
    if (rss > 0 && ram_limit_mb > 0.0) {
      g_autofree char *used_str = g_format_size_full((guint64)rss, G_FORMAT_SIZE_IEC_UNITS);
      g_autofree char *limit_str = g_format_size_full((guint64)(ram_limit_mb * 1024.0 * 1024.0),
                                                      G_FORMAT_SIZE_IEC_UNITS);
      g_autofree char *val = g_strdup_printf("RAM %s / %s (%.0f%%)", used_str, limit_str, ram_pct);
      gtk_label_set_text(self->label_stats_ram, val);
    } else if (rss > 0) {
      g_autofree char *used_str = g_format_size_full((guint64)rss, G_FORMAT_SIZE_IEC_UNITS);
      g_autofree char *val = g_strdup_printf("RAM %s", used_str);
      gtk_label_set_text(self->label_stats_ram, val);
    } else {
      gtk_label_set_text(self->label_stats_ram, "RAM --");
    }
  }
  if (self->label_stats_disk != NULL) {
    if (!self->tps_enabled) {
      gtk_label_set_text(self->label_stats_disk, "TPS disabled");
    } else if (self->last_tps_valid) {
      g_autofree char *val = g_strdup_printf("TPS %.1f", self->last_tps);
      gtk_label_set_text(self->label_stats_disk, val);
    } else {
      gtk_label_set_text(self->label_stats_disk, "TPS --");
    }
  }
  if (self->label_stats_players != NULL) {
    g_autofree char *val = NULL;
    if (max_players > 0) {
      val = g_strdup_printf("Players %d / %d", players_count, max_players);
    } else {
      val = g_strdup_printf("Players %d", players_count);
    }
    gtk_label_set_text(self->label_stats_players, val);
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
    if (self->plugin_drop_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->plugin_drop_hint), TRUE);
    }
    return;
  }

  g_autofree char *plugins_dir = pumpkin_server_get_plugins_dir(self->current);
  GDir *dir = g_dir_open(plugins_dir, 0, NULL);
  if (dir == NULL) {
    return;
  }

  gboolean busy = (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_RESTARTING);
  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    gboolean disabled = g_str_has_suffix(entry, ".disabled") || g_str_has_suffix(entry, ".deactivated");
    gboolean is_so = g_str_has_suffix(entry, ".so") ||
                     g_str_has_suffix(entry, ".so.disabled") ||
                     g_str_has_suffix(entry, ".so.deactivated");
    if (!is_so) {
      continue;
    }
    g_autofree char *display = g_strdup(entry);
    if (g_str_has_suffix(display, ".disabled")) {
      display[strlen(display) - strlen(".disabled")] = '\0';
    } else if (g_str_has_suffix(display, ".deactivated")) {
      display[strlen(display) - strlen(".deactivated")] = '\0';
    }

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(display);
    GtkWidget *toggle = gtk_switch_new();
    GtkWidget *btn_delete = gtk_button_new_with_label("Delete");

    gtk_switch_set_active(GTK_SWITCH(toggle), !disabled);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(toggle, FALSE);
    gtk_widget_set_halign(toggle, GTK_ALIGN_END);
    gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(toggle, FALSE);
    gtk_widget_set_size_request(toggle, 40, 22);
    gtk_widget_add_css_class(toggle, "compact-switch");
    gtk_widget_set_sensitive(toggle, !busy);

    gtk_box_append(GTK_BOX(box), label);
    g_autofree char *version = plugin_version_label(plugins_dir, display);
    if (version != NULL) {
      GtkWidget *ver_label = gtk_label_new(version);
      gtk_widget_add_css_class(ver_label, "dim-label");
      gtk_box_append(GTK_BOX(box), ver_label);
    }
    gtk_box_append(GTK_BOX(box), toggle);
    gtk_widget_add_css_class(btn_delete, "destructive-action");
    gtk_widget_add_css_class(btn_delete, "small");
    gtk_widget_add_css_class(btn_delete, "compact-button");
    gtk_widget_add_css_class(btn_delete, "plugin-delete-button");
    gtk_widget_set_valign(btn_delete, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(btn_delete, FALSE);
    gtk_widget_set_sensitive(btn_delete, !busy);
    g_object_set_data_full(G_OBJECT(btn_delete), "plugin-file", g_strdup(entry), g_free);
    g_object_set_data_full(G_OBJECT(btn_delete), "plugin-dir", g_strdup(plugins_dir), g_free);
    g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_plugin_delete_clicked), self);
    gtk_box_append(GTK_BOX(box), btn_delete);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    g_object_set_data_full(G_OBJECT(row), "plugin-file", g_strdup(entry), g_free);
    g_object_set_data_full(G_OBJECT(toggle), "plugin-file", g_strdup(entry), g_free);
    g_object_set_data_full(G_OBJECT(toggle), "plugin-dir", g_strdup(plugins_dir), g_free);
    g_signal_connect(toggle, "state-set", G_CALLBACK(on_plugin_toggle_state), self);
    gtk_list_box_append(self->plugin_list, row);
  }
  g_dir_close(dir);

  if (self->plugin_drop_hint != NULL) {
    gboolean empty = (gtk_list_box_get_row_at_index(self->plugin_list, 0) == NULL);
    gtk_widget_set_visible(GTK_WIDGET(self->plugin_drop_hint), empty);
  }
}

static void
refresh_world_list(PumpkinWindow *self)
{
  clear_list_box(self->world_list);
  if (self->current == NULL) {
    if (self->world_drop_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->world_drop_hint), TRUE);
    }
    return;
  }

  gboolean running = pumpkin_server_get_running(self->current);
  gboolean busy = (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_RESTARTING);
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

      GtkWidget *btn_delete = gtk_button_new_with_label("Delete");
      gtk_widget_add_css_class(btn_delete, "destructive-action");
      gtk_widget_set_sensitive(btn_delete, !running && !busy);
      g_object_set_data_full(G_OBJECT(btn_delete), "world-path", g_strdup(child), g_free);
      g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_world_delete_clicked), self);
      gtk_box_append(GTK_BOX(box), btn_delete);

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

    GtkWidget *btn_delete = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(btn_delete, "destructive-action");
    gtk_widget_set_sensitive(btn_delete, !running && !busy);
    g_object_set_data_full(G_OBJECT(btn_delete), "world-path", g_strdup(default_world), g_free);
    g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_world_delete_clicked), self);
    gtk_box_append(GTK_BOX(box), btn_delete);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(self->world_list, row);
  }

  if (self->world_drop_hint != NULL) {
    gboolean empty = (gtk_list_box_get_row_at_index(self->world_list, 0) == NULL);
    gtk_widget_set_visible(GTK_WIDGET(self->world_drop_hint), empty);
  }
}

static void
append_simple_player_row(GtkListBox *list, const char *name, const char *uuid)
{
  if (list == NULL || name == NULL) {
    return;
  }

  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *label = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);

  if (uuid != NULL && *uuid != '\0') {
    GtkWidget *uuid_label = gtk_label_new(uuid);
    gtk_label_set_xalign(GTK_LABEL(uuid_label), 1.0);
    gtk_widget_add_css_class(uuid_label, "dim-label");
    gtk_box_append(GTK_BOX(box), uuid_label);
  }

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_list_box_append(list, row);
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

  refresh_whitelist_list(self);
  refresh_banned_list(self);
}

static void
refresh_whitelist_list(PumpkinWindow *self)
{
  clear_list_box(self->whitelist_list);
  if (self->current == NULL) {
    return;
  }

  g_autofree char *path = resolve_data_file(self->current, "whitelist.json");
  g_autoptr(GPtrArray) entries = load_player_entries_from_file(path);
  if (entries == NULL) {
    return;
  }

  for (guint i = 0; i < entries->len; i++) {
    PlayerEntry *entry = g_ptr_array_index(entries, i);
    append_simple_player_row(self->whitelist_list, entry->name, entry->uuid);
  }
}

static void
refresh_banned_list(PumpkinWindow *self)
{
  clear_list_box(self->banned_list);
  if (self->current == NULL) {
    return;
  }

  g_autofree char *path = resolve_data_file(self->current, "banned-players.json");
  g_autoptr(GPtrArray) entries = load_player_entries_from_file(path);
  if (entries == NULL) {
    return;
  }

  for (guint i = 0; i < entries->len; i++) {
    PlayerEntry *entry = g_ptr_array_index(entries, i);
    append_simple_player_row(self->banned_list, entry->name, entry->uuid);
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
      self->entry_server_port == NULL || self->entry_bedrock_port == NULL ||
      self->entry_max_players == NULL ||
      self->entry_max_cpu_cores == NULL || self->entry_max_ram_mb == NULL ||
      self->entry_rcon_host == NULL || self->entry_rcon_port == NULL ||
      self->entry_rcon_password == NULL) {
    return;
  }
  self->settings_loading = TRUE;
  self->settings_invalid = FALSE;
  if (self->current == NULL) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_server_name), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_download_url), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_server_port), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_bedrock_port), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_max_cpu_cores), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_max_ram_mb), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_host), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_port), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_password), "");
    if (self->label_resource_limits != NULL) {
      gtk_label_set_text(self->label_resource_limits, "Max available: -- cores, -- MB RAM.");
    }
    if (self->switch_auto_restart != NULL) {
      gtk_switch_set_active(self->switch_auto_restart, FALSE);
    }
    if (self->entry_auto_restart_delay != NULL) {
      gtk_editable_set_text(GTK_EDITABLE(self->entry_auto_restart_delay), "");
    }
    self->settings_loading = FALSE;
    self->settings_dirty = FALSE;
    if (self->btn_save_settings != NULL) {
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings), FALSE);
    }
    if (self->label_java_port_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_java_port_hint), FALSE);
    }
    if (self->label_bedrock_port_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_bedrock_port_hint), FALSE);
    }
    if (self->label_max_players_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_players_hint), FALSE);
    }
    if (self->label_rcon_host_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_host_hint), FALSE);
    }
    if (self->label_rcon_port_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_port_hint), FALSE);
    }
    if (self->label_max_cpu_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_cpu_hint), FALSE);
    }
    if (self->label_max_ram_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_ram_hint), FALSE);
    }
    return;
  }

  gtk_editable_set_text(GTK_EDITABLE(self->entry_server_name), pumpkin_server_get_name(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_download_url), pumpkin_server_get_download_url(self->current));

  g_autofree char *port = g_strdup_printf("%d", pumpkin_server_get_port(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_server_port), port);
  g_autofree char *bedrock_port = g_strdup_printf("%d", pumpkin_server_get_bedrock_port(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_bedrock_port), bedrock_port);

  g_autofree char *max_players = g_strdup_printf("%d", pumpkin_server_get_max_players(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_max_players), max_players);

  int max_cpu = pumpkin_server_get_max_cpu_cores(self->current);
  if (max_cpu > 0) {
    g_autofree char *max_cpu_str = g_strdup_printf("%d", max_cpu);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_max_cpu_cores), max_cpu_str);
  } else {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_max_cpu_cores), "");
  }

  int max_ram = pumpkin_server_get_max_ram_mb(self->current);
  if (max_ram > 0) {
    g_autofree char *max_ram_str = g_strdup_printf("%d", max_ram);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_max_ram_mb), max_ram_str);
  } else {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_max_ram_mb), "");
  }

  if (self->label_resource_limits != NULL) {
    int sys_cores = 0;
    int sys_ram_mb = 0;
    get_system_limits(&sys_cores, &sys_ram_mb);
    g_autofree char *cores_str = sys_cores > 0 ? g_strdup_printf("%d", sys_cores) : g_strdup("--");
    g_autofree char *ram_str = sys_ram_mb > 0 ? g_strdup_printf("%d", sys_ram_mb) : g_strdup("--");
    g_autofree char *limits = g_strdup_printf("Max available: %s cores, %s MB RAM.", cores_str, ram_str);
    gtk_label_set_text(self->label_resource_limits, limits);
  }
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
  if (self->btn_save_settings != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings), FALSE);
  }
  validate_settings_limits(self);
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
  self->last_proc_pid = 0;
  reset_stats_history(self);
  self->query_valid = FALSE;
  self->query_in_flight = FALSE;
  self->query_players = 0;
  self->query_max_players = 0;
  self->query_updated_at = 0;

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
  set_console_warning(self, NULL, FALSE);
  if (self->stats_row != NULL) {
    gtk_widget_set_visible(GTK_WIDGET(self->stats_row), TRUE);
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

static char *
unique_import_file(const char *base_dir, const char *name)
{
  g_autofree char *base_name = g_path_get_basename(name);
  g_autofree char *candidate = g_build_filename(base_dir, base_name, NULL);
  if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) {
    return g_strdup(candidate);
  }

  const char *dot = g_strrstr(base_name, ".");
  g_autofree char *stem = NULL;
  g_autofree char *ext = NULL;
  if (dot != NULL && dot != base_name) {
    stem = g_strndup(base_name, (gsize)(dot - base_name));
    ext = g_strdup(dot);
  } else {
    stem = g_strdup(base_name);
    ext = g_strdup("");
  }

  for (guint i = 2; i < 1000; i++) {
    g_autofree char *with_suffix = g_strdup_printf("%s-%u%s", stem, i, ext);
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

static GPtrArray *
files_from_drop_value(const GValue *value)
{
  if (value == NULL) {
    return NULL;
  }

  g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_object_unref);
  if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
    GdkFileList *file_list = g_value_get_boxed(value);
    if (file_list == NULL) {
      return NULL;
    }
    GSList *list = gdk_file_list_get_files(file_list);
    for (GSList *iter = list; iter != NULL; iter = iter->next) {
      GFile *file = iter->data;
      if (file != NULL) {
        g_ptr_array_add(files, g_object_ref(file));
      }
    }
  } else if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
    GFile *file = g_value_get_object(value);
    if (file != NULL) {
      g_ptr_array_add(files, g_object_ref(file));
    }
  } else {
    return NULL;
  }

  return g_steal_pointer(&files);
}

static gboolean
import_drop_files_to_dir(PumpkinWindow *self,
                         GPtrArray *files,
                         const char *dest_dir,
                         gboolean directories_only,
                         const char *kind_label)
{
  if (self->current == NULL || files == NULL || dest_dir == NULL) {
    return FALSE;
  }

  gboolean any = FALSE;
  for (guint i = 0; i < files->len; i++) {
    GFile *file = g_ptr_array_index(files, i);
    if (file == NULL) {
      continue;
    }

    g_autofree char *path = g_file_get_path(file);
    if (path == NULL) {
      append_log(self, "Only local files can be imported.");
      continue;
    }

    gboolean is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
    if (directories_only && !is_dir) {
      if (kind_label != NULL) {
        g_autofree char *msg = g_strdup_printf("Only folders can be imported to %s.", kind_label);
        append_log(self, msg);
      }
      continue;
    }

    g_autoptr(GError) copy_error = NULL;
    if (is_dir) {
      g_autofree char *target_dir = unique_import_dir(dest_dir, path);
      if (!copy_tree(path, target_dir, &copy_error)) {
        if (copy_error != NULL) {
          append_log(self, copy_error->message);
        }
        continue;
      }
    } else {
      g_autofree char *target_path = unique_import_file(dest_dir, path);
      g_autoptr(GFile) src_file = g_file_new_for_path(path);
      g_autoptr(GFile) dest_file = g_file_new_for_path(target_path);
      if (!g_file_copy(src_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &copy_error)) {
        if (copy_error != NULL) {
          append_log(self, copy_error->message);
        }
        continue;
      }
    }

    any = TRUE;
  }

  return any;
}

static gboolean
import_plugin_files_to_dir(PumpkinWindow *self, GPtrArray *files, const char *dest_dir)
{
  if (self == NULL || files == NULL || dest_dir == NULL) {
    return FALSE;
  }

  gboolean any = FALSE;
  for (guint i = 0; i < files->len; i++) {
    GFile *file = g_ptr_array_index(files, i);
    if (file == NULL) {
      continue;
    }

    g_autofree char *path = g_file_get_path(file);
    if (path == NULL) {
      append_log(self, "Only local files can be imported.");
      continue;
    }
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      append_log(self, "Plugins must be .so files.");
      continue;
    }
    if (!g_str_has_suffix(path, ".so")) {
      append_log(self, "Only .so files can be imported as plugins.");
      continue;
    }

    g_autofree char *base_name = g_path_get_basename(path);
    g_autofree char *target_path = g_build_filename(dest_dir, base_name, NULL);
    if (g_file_test(target_path, G_FILE_TEST_EXISTS)) {
      PluginOverwriteContext *ctx = g_new0(PluginOverwriteContext, 1);
      ctx->self = g_object_ref(self);
      ctx->src_path = g_strdup(path);
      ctx->dest_dir = g_strdup(dest_dir);
      ctx->dest_path = g_strdup(target_path);

      g_autofree char *title = g_strdup_printf("Overwrite plugin \"%s\"?", base_name);
      AdwDialog *dialog = adw_alert_dialog_new(title, "A plugin with the same name already exists.");
      AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
      adw_alert_dialog_add_response(alert, "cancel", "Cancel");
      adw_alert_dialog_add_response(alert, "overwrite", "Overwrite");
      adw_alert_dialog_set_default_response(alert, "cancel");
      adw_alert_dialog_set_close_response(alert, "cancel");
      adw_alert_dialog_set_response_appearance(alert, "overwrite", ADW_RESPONSE_DESTRUCTIVE);
      g_object_set_data_full(G_OBJECT(dialog), "plugin-overwrite-ctx", ctx, (GDestroyNotify)plugin_overwrite_context_free);
      adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_plugin_overwrite_confirmed, self);
      continue;
    }

    g_autoptr(GFile) src_file = g_file_new_for_path(path);
    g_autoptr(GFile) dest_file = g_file_new_for_path(target_path);
    g_autoptr(GError) copy_error = NULL;
    if (!g_file_copy(src_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &copy_error)) {
      if (copy_error != NULL) {
        append_log(self, copy_error->message);
      }
      continue;
    }
    any = TRUE;
  }

  return any;
}

static void
set_drop_highlight(GtkWidget *widget, gboolean active)
{
  if (widget == NULL) {
    return;
  }
  if (active) {
    gtk_widget_add_css_class(widget, "drop-active");
  } else {
    gtk_widget_remove_css_class(widget, "drop-active");
  }
}

static void
set_console_warning(PumpkinWindow *self, const char *message, gboolean visible)
{
  if (self == NULL || self->console_warning_revealer == NULL ||
      self->console_warning_label == NULL) {
    return;
  }
  gtk_label_set_text(self->console_warning_label, message != NULL ? message : "");
  gtk_revealer_set_reveal_child(self->console_warning_revealer, visible);
  if (visible) {
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
      gdk_display_beep(display);
    }
  }
}

static char *
strip_ansi(const char *line)
{
  if (line == NULL) {
    return NULL;
  }
  GString *out = g_string_new(NULL);
  for (const char *p = line; *p != '\0'; p++) {
    if (*p == '\x1b' && p[1] == '[') {
      p += 2;
      while (*p != '\0' && ((*p >= '0' && *p <= '9') || *p == ';')) {
        p++;
      }
      if (*p == 'm') {
        continue;
      }
    }
    g_string_append_c(out, *p);
  }
  return g_string_free(out, FALSE);
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

static gboolean
on_plugins_drop(GtkDropTarget *target, const GValue *value, double x, double y, PumpkinWindow *self)
{
  (void)target;
  (void)x;
  (void)y;
  if (self->current == NULL) {
    return FALSE;
  }

  g_autofree char *plugins_dir = pumpkin_server_get_plugins_dir(self->current);
  if (plugins_dir == NULL) {
    return FALSE;
  }

  g_autoptr(GPtrArray) files = files_from_drop_value(value);
  if (files == NULL || files->len == 0) {
    return FALSE;
  }

  gboolean imported = import_plugin_files_to_dir(self, files, plugins_dir);
  if (imported) {
    refresh_plugin_list(self);
  }
  set_drop_highlight(GTK_WIDGET(self->plugin_list), FALSE);
  return imported;
}

static GdkDragAction
on_plugins_drop_enter(GtkDropTarget *target, double x, double y, PumpkinWindow *self)
{
  (void)target;
  (void)x;
  (void)y;
  set_drop_highlight(GTK_WIDGET(self->plugin_list), TRUE);
  return GDK_ACTION_COPY;
}

static void
on_plugins_drop_leave(GtkDropTarget *target, PumpkinWindow *self)
{
  (void)target;
  set_drop_highlight(GTK_WIDGET(self->plugin_list), FALSE);
}

static gboolean
on_worlds_drop(GtkDropTarget *target, const GValue *value, double x, double y, PumpkinWindow *self)
{
  (void)target;
  (void)x;
  (void)y;
  if (self->current == NULL) {
    return FALSE;
  }
  if (pumpkin_server_get_running(self->current) ||
      self->ui_state == UI_STATE_STARTING ||
      self->ui_state == UI_STATE_RESTARTING) {
    set_details_error(self, "Stop the server before modifying worlds.");
    return FALSE;
  }

  g_autofree char *worlds_dir = pumpkin_server_get_worlds_dir(self->current);
  if (worlds_dir == NULL) {
    return FALSE;
  }

  g_autoptr(GPtrArray) files = files_from_drop_value(value);
  if (files == NULL || files->len == 0) {
    return FALSE;
  }

  gboolean imported = import_drop_files_to_dir(self, files, worlds_dir, TRUE, "Worlds");
  if (imported) {
    refresh_world_list(self);
  }
  set_drop_highlight(GTK_WIDGET(self->world_list), FALSE);
  return imported;
}

static GdkDragAction
on_worlds_drop_enter(GtkDropTarget *target, double x, double y, PumpkinWindow *self)
{
  (void)target;
  (void)x;
  (void)y;
  set_drop_highlight(GTK_WIDGET(self->world_list), TRUE);
  return GDK_ACTION_COPY;
}

static void
on_worlds_drop_leave(GtkDropTarget *target, PumpkinWindow *self)
{
  (void)target;
  set_drop_highlight(GTK_WIDGET(self->world_list), FALSE);
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

  PumpkinDownloadResult result_code = PUMPKIN_DOWNLOAD_OK;
  g_autofree char *url = pumpkin_resolve_latest_finish(res, &result_code, &error);
  if (url == NULL) {
    append_log(self, error->message);
    set_details_error(self, error->message);
    return;
  }
  if (result_code == PUMPKIN_DOWNLOAD_FALLBACK_USED) {
    append_log(self, "Using fallback download URL; verify if install fails.");
    set_details_status(self, "Using fallback download URL", 4);
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
  update_details(self);
  refresh_plugin_list(self);
  refresh_world_list(self);

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
    self->user_stop_requested = FALSE;
    set_console_warning(self, NULL, FALSE);
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
  self->user_stop_requested = TRUE;
  pumpkin_server_stop(self->current);
  update_details(self);
  refresh_plugin_list(self);
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
  self->restart_pending = TRUE;
  self->ui_state = UI_STATE_RESTARTING;
  self->user_stop_requested = TRUE;
  refresh_plugin_list(self);
  refresh_world_list(self);

  if (pumpkin_server_get_running(self->current)) {
    pumpkin_server_stop(self->current);
  } else {
    self->restart_pending = FALSE;
    RestartContext *ctx = g_new0(RestartContext, 1);
    ctx->self = g_object_ref(self);
    ctx->server = g_object_ref(self->current);
    self->restart_delay_id = g_timeout_add(0, restart_after_delay, ctx);
  }
  update_details(self);
}

static void
on_world_delete_clicked(GtkButton *button, PumpkinWindow *self)
{
  if (self->current == NULL || pumpkin_server_get_running(self->current)) {
    return;
  }
  if (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_RESTARTING) {
    set_details_error(self, "Wait for the server to finish starting/restarting.");
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
  pumpkin_resolve_latest_async(NULL, on_latest_only_resolve_done, self);
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

static int
parse_limit_entry(GtkEntry *entry, int max_value)
{
  if (entry == NULL) {
    return 0;
  }
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (text == NULL || *text == '\0') {
    return 0;
  }
  int value = atoi(text);
  if (value <= 0) {
    return 0;
  }
  if (max_value > 0 && value > max_value) {
    return max_value;
  }
  return value;
}

static void
on_save_settings(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->settings_invalid) {
    set_details_status(self, "Fix invalid settings first", 3);
    return;
  }
  if (self->current == NULL) {
    if (self->config != NULL) {
      if (self->switch_use_cache != NULL) {
        pumpkin_config_set_use_cache(self->config, gtk_switch_get_active(self->switch_use_cache));
      }
      if (self->switch_run_in_background != NULL) {
        pumpkin_config_set_run_in_background(self->config, gtk_switch_get_active(self->switch_run_in_background));
      }
      pumpkin_config_save(self->config, NULL);
      append_log(self, "Settings saved");
      set_details_status(self, "Settings saved", 3);
    }
    return;
  }

  pumpkin_server_set_name(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_server_name)));
  pumpkin_server_set_download_url(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_download_url)));
  pumpkin_server_set_port(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_server_port))));
  pumpkin_server_set_bedrock_port(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_bedrock_port))));
  pumpkin_server_set_max_players(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_players))));
  int sys_cores = 0;
  int sys_ram_mb = 0;
  get_system_limits(&sys_cores, &sys_ram_mb);
  int max_cpu = parse_limit_entry(self->entry_max_cpu_cores, sys_cores);
  int max_ram = parse_limit_entry(self->entry_max_ram_mb, sys_ram_mb);
  pumpkin_server_set_max_cpu_cores(self->current, max_cpu);
  pumpkin_server_set_max_ram_mb(self->current, max_ram);
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
    set_details_status(self, "Settings saved", 3);
  }

  if (self->config != NULL) {
    const char *download_url = gtk_editable_get_text(GTK_EDITABLE(self->entry_download_url));
    pumpkin_config_set_default_download_url(self->config, download_url);
    if (self->switch_use_cache != NULL) {
      pumpkin_config_set_use_cache(self->config, gtk_switch_get_active(self->switch_use_cache));
    }
    if (self->switch_run_in_background != NULL) {
      pumpkin_config_set_run_in_background(self->config, gtk_switch_get_active(self->switch_run_in_background));
    }
    pumpkin_config_save(self->config, NULL);
  }

  self->settings_dirty = FALSE;
  if (self->btn_save_settings != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings), FALSE);
  }
  clear_list_box(self->server_list);
  load_server_list(self);
  update_overview(self);
}

static void
pumpkin_window_init(PumpkinWindow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self, "close-request", G_CALLBACK(on_window_close_request), self);
  g_signal_connect(self, "notify::visible", G_CALLBACK(on_window_visible_changed), self);

  adw_view_stack_set_visible_child_name(self->view_stack, "overview");
  if (self->details_stack != NULL) {
    self->last_details_page = g_strdup(adw_view_stack_get_visible_child_name(self->details_stack));
  }
  disable_players_tab(self);
  if (self->btn_save_settings != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings), FALSE);
  }

  if (self->plugin_list != NULL) {
    gtk_widget_set_vexpand(GTK_WIDGET(self->plugin_list), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self->plugin_list), TRUE);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "enter", G_CALLBACK(on_plugins_drop_enter), self);
    g_signal_connect(drop, "leave", G_CALLBACK(on_plugins_drop_leave), self);
    g_signal_connect(drop, "drop", G_CALLBACK(on_plugins_drop), self);
    gtk_widget_add_controller(GTK_WIDGET(self->plugin_list), GTK_EVENT_CONTROLLER(drop));
  }
  if (self->world_list != NULL) {
    gtk_widget_set_vexpand(GTK_WIDGET(self->world_list), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self->world_list), TRUE);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "enter", G_CALLBACK(on_worlds_drop_enter), self);
    g_signal_connect(drop, "leave", G_CALLBACK(on_worlds_drop_leave), self);
    g_signal_connect(drop, "drop", G_CALLBACK(on_worlds_drop), self);
    gtk_widget_add_controller(GTK_WIDGET(self->world_list), GTK_EVENT_CONTROLLER(drop));
  }

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
  if (self->entry_bedrock_port != NULL) {
    g_signal_connect(self->entry_bedrock_port, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_max_players != NULL) {
    g_signal_connect(self->entry_max_players, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_max_cpu_cores != NULL) {
    g_signal_connect(self->entry_max_cpu_cores, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->entry_max_ram_mb != NULL) {
    g_signal_connect(self->entry_max_ram_mb, "changed", G_CALLBACK(on_settings_changed), self);
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
  if (self->switch_run_in_background != NULL) {
    g_signal_connect(self->switch_run_in_background, "notify::active",
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
    if (self->switch_run_in_background != NULL) {
      gtk_switch_set_active(self->switch_run_in_background,
                            pumpkin_config_get_run_in_background(self->config));
    }
  }

  pumpkin_resolve_latest_async(NULL, on_latest_only_resolve_done, self);

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

#if defined(G_OS_WIN32)
  self->clk_tck = 0;
#else
  self->clk_tck = sysconf(_SC_CLK_TCK);
#endif
  reset_stats_history(self);
  if (self->stats_graph_usage != NULL) {
    gtk_drawing_area_set_draw_func(self->stats_graph_usage, stats_graph_draw_usage, self, NULL);
  }
  if (self->stats_graph_players != NULL) {
    gtk_drawing_area_set_draw_func(self->stats_graph_players, stats_graph_draw_players, self, NULL);
  }
  if (self->stats_graph_disk != NULL) {
    gtk_drawing_area_set_draw_func(self->stats_graph_disk, stats_graph_draw_disk, self, NULL);
  }
  self->stats_refresh_id = g_timeout_add_seconds(STATS_SAMPLE_SECONDS, update_stats_tick, self);
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
  g_clear_pointer(&self->last_details_page, g_free);
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
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, details_switcher);
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
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, plugin_drop_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, world_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, world_drop_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, player_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, whitelist_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, banned_list);
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
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, stats_graph_usage);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, stats_graph_players);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, stats_graph_disk);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_stats_cpu);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_stats_ram);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_stats_disk);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_stats_players);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, console_warning_revealer);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, console_warning_label);

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
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_bedrock_port);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_max_players);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_max_cpu_cores);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_max_ram_mb);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_java_port_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_bedrock_port_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_max_players_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_max_cpu_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_max_ram_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_auto_restart);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_auto_restart_delay);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_host);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_port);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_password);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_rcon_host_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_rcon_port_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_resource_limits);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_use_cache);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_run_in_background);
}

GtkWindow *
pumpkin_window_new(AdwApplication *app)
{
  return GTK_WINDOW(g_object_new(PUMPKIN_TYPE_WINDOW, "application", app, NULL));
}
