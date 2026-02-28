#include "window.h"
#include "app.h"

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
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#if !defined(G_OS_WIN32)
#include <unistd.h>
#endif
#include <time.h>

#define DEFAULT_STATS_SAMPLE_MSEC 200
#define STATS_SAMPLE_MSEC_MIN 2
#define STATS_SAMPLE_MSEC_MAX 2000
#define STATS_HISTORY_SECONDS 180
#define STATS_SAMPLES ((STATS_HISTORY_SECONDS * 1000) / DEFAULT_STATS_SAMPLE_MSEC)
#define PLAYER_STATE_FLUSH_INTERVAL_USEC (15 * G_USEC_PER_SEC)
#define COMMAND_HISTORY_MAX 200

struct _PumpkinWindow {
  AdwApplicationWindow parent_instance;

  AdwViewStack *view_stack;
  AdwViewStack *details_stack;
  AdwViewSwitcher *details_switcher;

  GtkListBox *server_list;
  GtkTextView *log_view;
  GtkListBox *overview_list;
  char *latest_url;
  char *latest_build_id;
  char *latest_build_label;
  guint latest_poll_id;
  gboolean latest_resolve_in_flight;

  GtkButton *btn_add_server;
  GtkButton *btn_remove_server;
  GtkButton *btn_add_server_overview;
  GtkButton *btn_import_server;
  GtkButton *btn_import_server_overview;
  GtkButton *btn_open_plugins;
  GtkButton *btn_open_players;
  GtkButton *btn_open_worlds;
  GtkButton *btn_save_settings;

  GtkListBox *plugin_list;
  GtkListBox *world_list;
  GtkListBox *player_list;
  GtkSearchEntry *player_search;
  GtkButton *btn_player_sort_last_online;
  GtkButton *btn_player_sort_playtime;
  GtkButton *btn_player_sort_first_joined;
  GtkButton *btn_player_sort_name;
  GtkBox *plugin_drop_hint;
  GtkBox *world_drop_hint;
  GtkSearchEntry *whitelist_search;
  GtkSearchEntry *banned_search;
  GtkListBox *whitelist_list;
  GtkListBox *banned_list;
  GtkListBox *log_files_list;
  GtkTextView *log_file_view;
  GtkDropDown *log_filter;
  GtkDropDown *log_level_filter;
  GtkEntry *log_search;
  GtkButton *btn_open_logs;
  GtkButton *btn_clear_all_logs;
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
  GtkButton *btn_console_send;
  GtkButton *btn_console_copy;
  GtkButton *btn_console_clear;
  GtkMenuButton *btn_console_filter;
  GtkButton *btn_console_filter_all;
  GtkCheckButton *check_console_trace;
  GtkCheckButton *check_console_debug;
  GtkCheckButton *check_console_info;
  GtkCheckButton *check_console_warn;
  GtkCheckButton *check_console_error;
  GtkCheckButton *check_console_smpk;
  GtkCheckButton *check_console_other;
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
  guint auto_update_countdown_id;
  GHashTable *download_progress_state;
  gboolean restart_requested;
  gboolean user_stop_requested;
  gboolean restart_pending;
  PumpkinServer *auto_update_server;
  int auto_update_countdown_remaining;
  gint64 auto_update_cooldown_until;
  gint64 last_auto_update_eval_at;
  int auto_update_last_schedule_day;
  char *auto_update_last_schedule_server_id;
  char *auto_update_last_attempt_server_id;
  char *auto_update_last_attempt_build_id;
  guint stats_refresh_id;
  int stats_sample_msec;
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
  gint64 last_tps_request_at;
  gint64 last_player_list_request_at;
  gint64 last_player_state_flush_at;
  gboolean player_state_dirty;
  guint pending_auto_tps_lines;
  guint pending_auto_list_lines;
  guint pending_java_platform_hints;
  guint pending_bedrock_platform_hints;

  int ui_state;

  GtkEntry *entry_server_name;
  GtkEntry *entry_download_url;
  GtkButton *btn_choose_icon;
  GtkButton *btn_reset_icon;
  GtkEntry *entry_server_port;
  GtkEntry *entry_bedrock_port;
  GtkEntry *entry_max_players;
  GtkEntry *entry_stats_sample_msec;
  GtkEntry *entry_max_cpu_cores;
  GtkEntry *entry_max_ram_mb;
  GtkLabel *label_java_port_hint;
  GtkLabel *label_bedrock_port_hint;
  GtkLabel *label_max_players_hint;
  GtkLabel *label_stats_sample_hint;
  GtkLabel *label_max_cpu_hint;
  GtkLabel *label_max_ram_hint;
  GtkSwitch *switch_auto_restart;
  GtkEntry *entry_auto_restart_delay;
  GtkSwitch *switch_auto_update;
  GtkSwitch *switch_auto_update_schedule;
  GtkEntry *entry_auto_update_time;
  GtkLabel *label_auto_update_time_hint;
  GtkEntry *entry_rcon_host;
  GtkEntry *entry_rcon_port;
  GtkPasswordEntry *entry_rcon_password;
  GtkSwitch *switch_use_cache;
  GtkSwitch *switch_run_in_background;
  GtkSwitch *switch_autostart_on_boot;
  GtkSwitch *switch_start_minimized;
  GtkSwitch *switch_auto_start_servers;
  GtkDropDown *drop_date_format;
  GtkDropDown *drop_time_format;
  GtkListBox *autostart_server_list;
  GtkButton *btn_add_autostart_server;
  GtkButton *btn_save_general_settings;
  GtkButton *btn_reset_general_settings;
  GtkButton *btn_reset_server_settings;
  GtkButton *btn_clear_cache;
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
  gboolean player_list_signature_valid;
  guint64 player_list_signature;
  guint player_list_signature_count;
  int player_sort_field;
  gboolean player_sort_ascending;
  GHashTable *live_player_names;
  GHashTable *platform_hint_by_ip;
  GHashTable *player_states;
  GHashTable *player_states_by_uuid;
  GHashTable *player_states_by_name;
  GHashTable *deleted_player_keys;
  GHashTable *player_head_downloads;
  GHashTable *console_buffers;
  GPtrArray *command_history;
  int command_history_index;
  char *command_history_draft;
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
  char *used_build_id;
  char *used_build_label;
  char *dest_path;
  char *tmp_path;
  char *server_bin;
  gboolean use_cache;
  gboolean restart_after_download;
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

typedef enum {
  PLAYER_PLATFORM_UNKNOWN = 0,
  PLAYER_PLATFORM_JAVA,
  PLAYER_PLATFORM_BEDROCK
} PlayerPlatform;

typedef enum {
  CONSOLE_LEVEL_OTHER = 0,
  CONSOLE_LEVEL_TRACE,
  CONSOLE_LEVEL_DEBUG,
  CONSOLE_LEVEL_INFO,
  CONSOLE_LEVEL_WARN,
  CONSOLE_LEVEL_ERROR,
  CONSOLE_LEVEL_SMPK
} ConsoleLevel;

typedef struct {
  char *key;
  char *name;
  char *uuid;
  char *last_ip;
  PlayerPlatform platform;
  gboolean online;
  gint64 first_joined_unix;
  gint64 last_online_unix;
  guint64 playtime_seconds;
  gint64 session_started_mono;
} PlayerState;

typedef struct {
  int field;
  gboolean ascending;
  GHashTable *op_level_map;
} PlayerSortSettings;

typedef struct {
  PumpkinWindow *self;
  PumpkinServer *server;
  char *uuid_key;
  char *cache_path;
} PlayerHeadDownloadContext;

static void start_download_for_server(PumpkinWindow *self,
                                      PumpkinServer *server,
                                      const char *url,
                                      gboolean force_no_cache,
                                      gboolean restart_after_download);
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
static void trigger_latest_resolve(PumpkinWindow *self);
static gboolean poll_latest_release_tick(gpointer data);
static void update_check_updates_badge(PumpkinWindow *self);
static void update_settings_form(PumpkinWindow *self);
static void on_save_settings(GtkButton *button, PumpkinWindow *self);
static gboolean server_settings_require_restart(PumpkinWindow *self);
static void save_settings_impl(PumpkinWindow *self, gboolean restart_server);
static void on_restart_required_save_confirmed(AdwAlertDialog *dialog, const char *response, PumpkinWindow *self);
static void complete_pending_settings_navigation(PumpkinWindow *self);
static void on_console_copy(GtkButton *button, PumpkinWindow *self);
static void on_console_clear(GtkButton *button, PumpkinWindow *self);
static void on_console_filter_toggled(GtkCheckButton *button, PumpkinWindow *self);
static void on_console_filter_all_clicked(GtkButton *button, PumpkinWindow *self);
static void on_settings_changed(GtkEditable *editable, PumpkinWindow *self);
static void on_settings_switch_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void populate_autostart_server_list(PumpkinWindow *self);
static void on_add_autostart_server(GtkButton *button, PumpkinWindow *self);
static void on_popover_server_picked(GtkButton *btn, PumpkinWindow *self);
static void update_autostart_sensitivity(PumpkinWindow *self);
static void on_save_general_settings(GtkButton *button, PumpkinWindow *self);
static void on_reset_general_settings(GtkButton *button, PumpkinWindow *self);
static void on_reset_server_settings(GtkButton *button, PumpkinWindow *self);
static void mark_settings_dirty(PumpkinWindow *self);
static void append_log(PumpkinWindow *self, const char *line);
static void set_details_status(PumpkinWindow *self, const char *message, guint timeout_seconds);
static void on_clear_cache(GtkButton *button, PumpkinWindow *self);
static char *cache_dir_for_config(PumpkinWindow *self);
static void on_details_stack_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void update_save_button(PumpkinWindow *self);
static void get_system_limits(int *max_cores, int *max_ram_mb);
static void validate_settings_limits(PumpkinWindow *self);
static int parse_limit_entry(GtkEntry *entry, int max_value);
static gboolean parse_tps_from_line(const char *line, double *out);
static void on_settings_leave_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static gboolean on_window_close_request(GtkWindow *window, gpointer user_data);
static void on_window_visible_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
static gboolean query_minecraft_players(const char *host, int port, int *out_players, int *out_max_players);
static gboolean is_player_list_snapshot_line(const char *line);
static gboolean parse_clock_time_text(const char *text, int *out_hour, int *out_minute);
static gboolean parse_clock_time_entry(GtkEntry *entry, int *out_hour, int *out_minute);
static int sanitize_stats_sample_msec(int value);
static int current_stats_sample_msec(PumpkinWindow *self);
static gint64 query_stale_usec(PumpkinWindow *self);
static gint64 tps_query_interval_usec(PumpkinWindow *self);
static gint64 player_list_query_interval_usec(PumpkinWindow *self);
static void restart_stats_refresh_timer(PumpkinWindow *self);
static void restart_players_refresh_timer(PumpkinWindow *self);
static void apply_stats_sample_msec(PumpkinWindow *self, int msec, gboolean reset_history);
static void update_auto_update_controls_sensitivity(PumpkinWindow *self);
static void clear_auto_update_countdown(PumpkinWindow *self);
static gboolean auto_update_countdown_tick(gpointer data);
static void maybe_trigger_auto_update(PumpkinWindow *self);
static void on_send_command(GtkWidget *widget, PumpkinWindow *self);
static gboolean on_command_entry_key_pressed(GtkEventControllerKey *controller,
                                             guint keyval,
                                             guint keycode,
                                             GdkModifierType state,
                                             PumpkinWindow *self);
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
static void invalidate_player_list_signature(PumpkinWindow *self);
static gboolean delete_player_tracking(PumpkinWindow *self,
                                       const char *state_key,
                                       const char *name,
                                       const char *uuid);
static void on_player_row_activated(GtkListBox *box, GtkListBoxRow *row, PumpkinWindow *self);
static void on_player_action_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static void on_player_ban_reason_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data);
static void update_live_player_names(PumpkinWindow *self, const char *line);
static void on_player_search_changed(GtkEditable *editable, PumpkinWindow *self);
static void on_player_sort_button_clicked(GtkButton *button, PumpkinWindow *self);
static void update_player_sort_buttons(PumpkinWindow *self);
static void on_whitelist_search_changed(GtkEditable *editable, PumpkinWindow *self);
static void on_banned_search_changed(GtkEditable *editable, PumpkinWindow *self);
static void refresh_log_files(PumpkinWindow *self);
static void on_log_filter_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void on_log_level_filter_changed(GObject *object, GParamSpec *pspec, PumpkinWindow *self);
static void on_log_search_changed(GtkEditable *editable, PumpkinWindow *self);
static void on_log_file_activated(GtkListBox *box, GtkListBoxRow *row, PumpkinWindow *self);
static void on_open_logs(GtkButton *button, PumpkinWindow *self);
static void on_clear_all_logs(GtkButton *button, PumpkinWindow *self);
static char *build_label_from_binary_path(PumpkinWindow *self, const char *bin_path);
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
static char *sanitize_console_text(const char *line);
static char *format_console_line(PumpkinWindow *self, const char *line, ConsoleLevel *out_level);
static ConsoleLevel console_level_from_text(const char *level_text);
static const char *console_level_tag_name(ConsoleLevel level);
static gboolean console_level_enabled(PumpkinWindow *self, ConsoleLevel level);
static void apply_console_filters_to_buffer(PumpkinWindow *self, GtkTextBuffer *buffer);
static void ensure_console_buffer_tags(PumpkinWindow *self, GtkTextBuffer *buffer);
static void apply_console_filters(PumpkinWindow *self);
static gboolean hide_status_cb(gpointer data);
static gboolean update_stats_tick(gpointer data);
static void reset_stats_history(PumpkinWindow *self);
static void set_stats_graphs_disabled(PumpkinWindow *self, gboolean disabled);
static void on_open_server_root(GtkButton *button, PumpkinWindow *self);
static gboolean restart_after_delay(gpointer data);
static void select_server_row(PumpkinWindow *self, PumpkinServer *server);
static void player_state_free(PlayerState *state);
static void player_states_clear(PumpkinWindow *self);
static void player_states_load(PumpkinWindow *self, PumpkinServer *server);
static void player_states_save(PumpkinWindow *self, PumpkinServer *server);
static void player_states_mark_all_offline(PumpkinWindow *self);
static PlayerState *ensure_player_state(PumpkinWindow *self, const char *uuid, const char *name, gboolean create);
static void player_state_mark_online(PumpkinWindow *self, PlayerState *state, PlayerPlatform platform_hint);
static void player_state_mark_offline(PumpkinWindow *self, PlayerState *state);
static guint64 player_state_effective_playtime(const PlayerState *state);
static int player_online_count(PumpkinWindow *self);
static void ingest_players_from_disk(PumpkinWindow *self);
static char *format_unix_time(PumpkinWindow *self, gint64 unix_ts);
static char *format_duration(guint64 seconds);
static char *relative_time_label(gint64 unix_ts);
static const char *date_time_pattern_for_config(PumpkinWindow *self);
static char *normalize_build_label(PumpkinWindow *self, const char *label);
static const char *platform_label(PlayerPlatform platform);
static PlayerPlatform platform_from_line(const char *line);
static PlayerPlatform platform_guess_from_uuid(const char *uuid);
static char *extract_ip_from_socket_text(const char *text);
static void remember_platform_hint_for_ip(PumpkinWindow *self, const char *ip, PlayerPlatform platform);
static PlayerPlatform platform_hint_for_ip(PumpkinWindow *self, const char *ip);
static void set_player_head_image(PumpkinWindow *self, GtkImage *image, const char *uuid);
static void on_player_head_download_done(GObject *source, GAsyncResult *res, gpointer user_data);
static char *player_tracking_file(PumpkinServer *server);

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

static gboolean
console_level_matches_log_filter(ConsoleLevel level, int level_index)
{
  if (level_index <= 0) {
    return TRUE;
  }
  if (level_index == 1) {
    return level == CONSOLE_LEVEL_INFO;
  }
  if (level_index == 2) {
    return level == CONSOLE_LEVEL_WARN;
  }
  if (level_index == 3) {
    return level == CONSOLE_LEVEL_ERROR;
  }
  return TRUE;
}

static gboolean
is_auto_poll_noise_line(const char *line)
{
  if (line == NULL) {
    return FALSE;
  }
  g_autofree char *clean = strip_ansi(line);
  const char *check = clean != NULL ? clean : line;
  if (is_player_list_snapshot_line(check)) {
    return TRUE;
  }
  double tps = 0.0;
  if (parse_tps_from_line(check, &tps)) {
    return TRUE;
  }
  return FALSE;
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

/* ---- Autostart server list helpers ---- */

typedef struct {
  PumpkinWindow *win;
  PumpkinServer *server;
} AutostartRowCtx;

static void
autostart_row_ctx_free(gpointer data, GClosure *closure)
{
  (void)closure;
  AutostartRowCtx *ctx = data;
  if (ctx != NULL) {
    g_object_unref(ctx->server);
    g_free(ctx);
  }
}

static void
on_autostart_delay_changed(GtkEditable *editable, PumpkinWindow *self)
{
  const char *text = gtk_editable_get_text(editable);
  PumpkinServer *server = g_object_get_data(G_OBJECT(editable), "server");
  if (server != NULL && text != NULL) {
    int val = atoi(text);
    if (val > 0) {
      pumpkin_server_set_auto_start_delay(server, val);
    }
  }
  if (!self->settings_loading) {
    self->settings_dirty = TRUE;
    if (self->btn_save_settings != NULL) {
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings), TRUE);
    }
  }
}

static void
on_remove_autostart_server(GtkButton *button, gpointer user_data)
{
  AutostartRowCtx *ctx = user_data;
  (void)button;
  if (ctx == NULL || ctx->win == NULL || ctx->server == NULL) {
    return;
  }
  pumpkin_server_set_auto_start_on_launch(ctx->server, FALSE);
  populate_autostart_server_list(ctx->win);
  ctx->win->settings_dirty = TRUE;
  if (ctx->win->btn_save_settings != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->win->btn_save_settings), TRUE);
  }
}

static void
populate_autostart_server_list(PumpkinWindow *self)
{
  if (self->autostart_server_list == NULL || self->store == NULL) {
    return;
  }

  /* Remove all existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->autostart_server_list))) != NULL) {
    gtk_list_box_remove(self->autostart_server_list, child);
  }

  GListModel *model = pumpkin_server_store_get_model(self->store);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(model, i);
    if (server == NULL) {
      continue;
    }
    if (!pumpkin_server_get_auto_start_on_launch(server)) {
      g_object_unref(server);
      continue;
    }

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);

    const char *name = pumpkin_server_get_name(server);
    GtkWidget *label = gtk_label_new(name != NULL ? name : "???" );
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(row), label);

    /* Delay entry */
    GtkWidget *delay_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(delay_entry), "s");
    gtk_widget_set_size_request(delay_entry, 80, -1);
    g_autofree char *delay_str = g_strdup_printf("%d", pumpkin_server_get_auto_start_delay(server));
    gtk_editable_set_text(GTK_EDITABLE(delay_entry), delay_str);
    g_object_set_data(G_OBJECT(delay_entry), "server", server);
    g_signal_connect(delay_entry, "changed", G_CALLBACK(on_autostart_delay_changed), self);
    gtk_box_append(GTK_BOX(row), delay_entry);

    GtkWidget *sec_label = gtk_label_new("s");
    gtk_widget_add_css_class(sec_label, "dim-label");
    gtk_box_append(GTK_BOX(row), sec_label);

    /* Remove button */
    GtkWidget *remove_btn = gtk_button_new_from_icon_name("edit-delete-symbolic");
    gtk_widget_add_css_class(remove_btn, "flat");
    AutostartRowCtx *ctx = g_new0(AutostartRowCtx, 1);
    ctx->win = self;
    ctx->server = g_object_ref(server);
    g_signal_connect_data(remove_btn, "clicked", G_CALLBACK(on_remove_autostart_server),
                          ctx, (GClosureNotify)autostart_row_ctx_free, 0);
    gtk_box_append(GTK_BOX(row), remove_btn);

    gtk_list_box_append(self->autostart_server_list, row);
    g_object_unref(server);
  }
}

static void
on_add_autostart_server(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->store == NULL) {
    return;
  }

  /* Build a popover with all servers not yet in the autostart list */
  GtkWidget *popover = gtk_popover_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_popover_set_child(GTK_POPOVER(popover), box);

  GListModel *model = pumpkin_server_store_get_model(self->store);
  guint n = g_list_model_get_n_items(model);
  gboolean has_items = FALSE;
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(model, i);
    if (server == NULL) {
      continue;
    }
    if (pumpkin_server_get_auto_start_on_launch(server)) {
      g_object_unref(server);
      continue;
    }

    const char *name = pumpkin_server_get_name(server);
    GtkWidget *btn = gtk_button_new_with_label(name != NULL ? name : "???");
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    g_object_set_data_full(G_OBJECT(btn), "server", server, g_object_unref);
    g_object_set_data(G_OBJECT(btn), "popover", popover);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_popover_server_picked), self);
    gtk_box_append(GTK_BOX(box), btn);
    has_items = TRUE;
  }

  if (!has_items) {
    GtkWidget *label = gtk_label_new("No more servers available");
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
  }

  gtk_widget_set_parent(popover, GTK_WIDGET(self->btn_add_autostart_server));
  gtk_popover_popup(GTK_POPOVER(popover));
}

static void
on_popover_server_picked(GtkButton *btn, PumpkinWindow *self)
{
  PumpkinServer *server = g_object_get_data(G_OBJECT(btn), "server");
  GtkWidget *popover = g_object_get_data(G_OBJECT(btn), "popover");
  if (server != NULL) {
    pumpkin_server_set_auto_start_on_launch(server, TRUE);
    populate_autostart_server_list(self);
    self->settings_dirty = TRUE;
    if (self->btn_save_settings != NULL) {
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save_settings), TRUE);
    }
  }
  if (popover != NULL) {
    gtk_popover_popdown(GTK_POPOVER(popover));
    gtk_widget_unparent(popover);
  }
}

static void
update_autostart_sensitivity(PumpkinWindow *self)
{
  gboolean boot_enabled = self->switch_autostart_on_boot != NULL &&
                           gtk_switch_get_active(self->switch_autostart_on_boot);
  if (self->switch_start_minimized != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->switch_start_minimized), boot_enabled);
  }
  if (self->switch_auto_start_servers != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->switch_auto_start_servers), boot_enabled);
  }
  gboolean list_enabled = boot_enabled &&
                          self->switch_auto_start_servers != NULL &&
                          gtk_switch_get_active(self->switch_auto_start_servers);
  if (self->autostart_server_list != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->autostart_server_list), list_enabled);
  }
  if (self->btn_add_autostart_server != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_add_autostart_server), list_enabled);
  }
}

/* ---- Save General Settings ---- */
static void
on_save_general_settings(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->config == NULL) {
    return;
  }
  if (self->switch_use_cache != NULL) {
    pumpkin_config_set_use_cache(self->config, gtk_switch_get_active(self->switch_use_cache));
  }
  if (self->switch_run_in_background != NULL) {
    pumpkin_config_set_run_in_background(self->config, gtk_switch_get_active(self->switch_run_in_background));
  }
  if (self->switch_autostart_on_boot != NULL) {
    gboolean autostart = gtk_switch_get_active(self->switch_autostart_on_boot);
    pumpkin_config_set_autostart_on_boot(self->config, autostart);
    pumpkin_config_manage_autostart_desktop(autostart);
  }
  if (self->switch_start_minimized != NULL) {
    pumpkin_config_set_start_minimized(self->config, gtk_switch_get_active(self->switch_start_minimized));
  }
  if (self->switch_auto_start_servers != NULL) {
    pumpkin_config_set_auto_start_servers_enabled(self->config, gtk_switch_get_active(self->switch_auto_start_servers));
  }
  if (self->drop_date_format != NULL) {
    pumpkin_config_set_date_format(self->config, (PumpkinDateFormat)gtk_drop_down_get_selected(self->drop_date_format));
  }
  if (self->drop_time_format != NULL) {
    pumpkin_config_set_time_format(self->config, (PumpkinTimeFormat)gtk_drop_down_get_selected(self->drop_time_format));
  }
  if (self->entry_download_url != NULL) {
    pumpkin_config_set_default_download_url(self->config, gtk_editable_get_text(GTK_EDITABLE(self->entry_download_url)));
  }
  /* Save per-server auto-start settings */
  if (self->store != NULL) {
    GListModel *model = pumpkin_server_store_get_model(self->store);
    guint n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; i++) {
      PumpkinServer *server = g_list_model_get_item(model, i);
      pumpkin_server_save(server, NULL);
      g_object_unref(server);
    }
  }
  pumpkin_config_save(self->config, NULL);
  if (self->switch_run_in_background != NULL) {
    GApplication *app = g_application_get_default();
    if (PUMPKIN_IS_APP(app)) {
      pumpkin_app_set_tray_enabled(PUMPKIN_APP(app),
                                   gtk_switch_get_active(self->switch_run_in_background));
    }
  }
  append_log(self, "General settings saved");
  set_details_status(self, "General settings saved", 3);
  self->settings_dirty = FALSE;
  update_save_button(self);
  refresh_overview_list(self);
  refresh_player_list(self);
  refresh_log_files(self);
  update_details(self);
}

/* ---- Reset General Settings ---- */
static void
on_reset_general_response(AdwAlertDialog *dialog, const char *response, PumpkinWindow *self)
{
  (void)dialog;
  if (g_strcmp0(response, "reset") != 0) {
    return;
  }

  self->settings_loading = TRUE;
  if (self->switch_autostart_on_boot != NULL) {
    gtk_switch_set_active(self->switch_autostart_on_boot, FALSE);
  }
  if (self->switch_start_minimized != NULL) {
    gtk_switch_set_active(self->switch_start_minimized, FALSE);
  }
  if (self->switch_auto_start_servers != NULL) {
    gtk_switch_set_active(self->switch_auto_start_servers, FALSE);
  }
  if (self->switch_run_in_background != NULL) {
    gtk_switch_set_active(self->switch_run_in_background, TRUE);
  }
  if (self->switch_use_cache != NULL) {
    gtk_switch_set_active(self->switch_use_cache, TRUE);
  }
  if (self->drop_date_format != NULL) {
    gtk_drop_down_set_selected(self->drop_date_format, (guint)PUMPKIN_DATE_FORMAT_DMY);
  }
  if (self->drop_time_format != NULL) {
    gtk_drop_down_set_selected(self->drop_time_format, (guint)PUMPKIN_TIME_FORMAT_24H);
  }
  if (self->entry_download_url != NULL) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_download_url), "");
  }
  /* Clear all server auto-start flags */
  if (self->store != NULL) {
    GListModel *model = pumpkin_server_store_get_model(self->store);
    guint n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; i++) {
      PumpkinServer *server = g_list_model_get_item(model, i);
      pumpkin_server_set_auto_start_on_launch(server, FALSE);
      g_object_unref(server);
    }
  }
  self->settings_loading = FALSE;
  populate_autostart_server_list(self);
  update_autostart_sensitivity(self);
  mark_settings_dirty(self);
}

static void
on_reset_general_settings(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  AdwDialog *dialog = adw_alert_dialog_new(
    "Reset General Settings?",
    "All general settings will be reset to their default values. This cannot be undone.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
    "cancel", "Cancel",
    "reset", "Reset",
    NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
    "reset", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_reset_general_response), self);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ---- Reset Server Settings ---- */
static void
on_reset_server_response(AdwAlertDialog *dialog, const char *response, PumpkinWindow *self)
{
  (void)dialog;
  if (g_strcmp0(response, "reset") != 0) {
    return;
  }

  if (self->current == NULL) {
    return;
  }

  pumpkin_server_set_name(self->current, "");
  pumpkin_server_set_port(self->current, 25565);
  pumpkin_server_set_bedrock_port(self->current, 19132);
  pumpkin_server_set_max_players(self->current, 20);
  pumpkin_server_set_stats_sample_msec(self->current, DEFAULT_STATS_SAMPLE_MSEC);
  pumpkin_server_set_max_cpu_cores(self->current, 0);
  pumpkin_server_set_max_ram_mb(self->current, 0);
  pumpkin_server_set_auto_restart(self->current, FALSE);
  pumpkin_server_set_auto_restart_delay(self->current, 10000);
  pumpkin_server_set_auto_update_enabled(self->current, FALSE);
  pumpkin_server_set_auto_update_use_schedule(self->current, FALSE);
  pumpkin_server_set_auto_update_hour(self->current, 1);
  pumpkin_server_set_auto_update_minute(self->current, 0);
  pumpkin_server_set_rcon_host(self->current, "127.0.0.1");
  pumpkin_server_set_rcon_port(self->current, 25575);
  pumpkin_server_set_rcon_password(self->current, "");

  update_settings_form(self);
  mark_settings_dirty(self);
}

static void
on_reset_server_settings(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }
  AdwDialog *dialog = adw_alert_dialog_new(
    "Reset Server Settings?",
    "All settings for this server will be reset to their default values. This cannot be undone.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
    "cancel", "Cancel",
    "reset", "Reset",
    NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
    "reset", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_reset_server_response), self);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ---- Click-to-unfocus ---- */
static void
on_window_click(GtkGestureClick *gesture, int n_press, double x, double y, PumpkinWindow *self)
{
  (void)n_press;
  (void)gesture;

  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(self));
  if (focus == NULL) {
    return;
  }
  /* Only unfocus text entries */
  if (!GTK_IS_ENTRY(focus) && !GTK_IS_TEXT(focus) && !GTK_IS_EDITABLE(focus)) {
    return;
  }
  /* Check what widget is under the click — don't unfocus if clicking into another entry or a button */
  GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(self), x, y, GTK_PICK_DEFAULT);
  while (picked != NULL && picked != GTK_WIDGET(self)) {
    if (GTK_IS_ENTRY(picked) || GTK_IS_TEXT(picked) || GTK_IS_EDITABLE(picked) ||
        GTK_IS_BUTTON(picked) || GTK_IS_SWITCH(picked)) {
      return;
    }
    picked = gtk_widget_get_parent(picked);
  }
  gtk_widget_grab_focus(GTK_WIDGET(self));
}

/* ---- Clear Cache ---- */
static gint64
recursive_dir_size(const char *path)
{
  gint64 total = 0;
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir == NULL) {
    return 0;
  }
  const char *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child = g_build_filename(path, name, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
      total += recursive_dir_size(child);
    } else {
      GStatBuf st;
      if (g_stat(child, &st) == 0) {
        total += st.st_size;
      }
    }
  }
  g_dir_close(dir);
  return total;
}

static void
recursive_dir_delete(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir == NULL) {
    return;
  }
  const char *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child = g_build_filename(path, name, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
      recursive_dir_delete(child);
    } else {
      g_remove(child);
    }
  }
  g_dir_close(dir);
  g_rmdir(path);
}

static void
on_clear_cache_response(AdwAlertDialog *dialog, const char *response, PumpkinWindow *self)
{
  (void)dialog;
  if (g_strcmp0(response, "clear") != 0) {
    return;
  }

  g_autofree char *cache_dir = cache_dir_for_config(self);
  if (cache_dir == NULL || !g_file_test(cache_dir, G_FILE_TEST_IS_DIR)) {
    append_log(self, "Cache directory does not exist.");
    set_details_status(self, "Cache is already empty", 3);
    return;
  }

  gint64 bytes = recursive_dir_size(cache_dir);
  recursive_dir_delete(cache_dir);

  g_autofree char *msg = NULL;
  if (bytes > 1024 * 1024) {
    msg = g_strdup_printf("Cache cleared (%.1f MB freed)", bytes / (1024.0 * 1024.0));
  } else if (bytes > 1024) {
    msg = g_strdup_printf("Cache cleared (%.1f KB freed)", bytes / 1024.0);
  } else {
    msg = g_strdup_printf("Cache cleared (%d bytes freed)", (int)bytes);
  }
  append_log(self, msg);
  set_details_status(self, msg, 5);
}

static void
on_clear_cache(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  AdwDialog *dialog = adw_alert_dialog_new(
    "Clear Download Cache?",
    "All cached Pumpkin binaries will be deleted. Future installs will need to download again.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
    "cancel", "Cancel",
    "clear", "Clear Cache",
    NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
    "clear", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_clear_cache_response), self);
  adw_dialog_present(dialog, GTK_WIDGET(self));
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
  if (self->switch_autostart_on_boot != NULL &&
      pumpkin_config_get_autostart_on_boot(self->config) != gtk_switch_get_active(self->switch_autostart_on_boot)) {
    return FALSE;
  }
  if (self->switch_start_minimized != NULL &&
      pumpkin_config_get_start_minimized(self->config) != gtk_switch_get_active(self->switch_start_minimized)) {
    return FALSE;
  }
  if (self->switch_auto_start_servers != NULL &&
      pumpkin_config_get_auto_start_servers_enabled(self->config) != gtk_switch_get_active(self->switch_auto_start_servers)) {
    return FALSE;
  }
  if (self->drop_date_format != NULL &&
      (int)pumpkin_config_get_date_format(self->config) != (int)gtk_drop_down_get_selected(self->drop_date_format)) {
    return FALSE;
  }
  if (self->drop_time_format != NULL &&
      (int)pumpkin_config_get_time_format(self->config) != (int)gtk_drop_down_get_selected(self->drop_time_format)) {
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
  if (!entry_matches_int(self->entry_stats_sample_msec, pumpkin_server_get_stats_sample_msec(server))) {
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
  if (self->switch_auto_update != NULL &&
      pumpkin_server_get_auto_update_enabled(server) != gtk_switch_get_active(self->switch_auto_update)) {
    return FALSE;
  }
  if (self->switch_auto_update_schedule != NULL &&
      pumpkin_server_get_auto_update_use_schedule(server) != gtk_switch_get_active(self->switch_auto_update_schedule)) {
    return FALSE;
  }
  if (self->entry_auto_update_time != NULL) {
    int hour = -1;
    int minute = -1;
    if (!parse_clock_time_entry(self->entry_auto_update_time, &hour, &minute)) {
      return FALSE;
    }
    if (hour != pumpkin_server_get_auto_update_hour(server) ||
        minute != pumpkin_server_get_auto_update_minute(server)) {
      return FALSE;
    }
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
  mark_settings_dirty(self);
  validate_settings_limits(self);
  update_autostart_sensitivity(self);
  update_auto_update_controls_sensitivity(self);
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
parse_clock_time_text(const char *text, int *out_hour, int *out_minute)
{
  if (text == NULL) {
    return FALSE;
  }
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  if (*text == '\0') {
    return FALSE;
  }

  char *end = NULL;
  long hour = strtol(text, &end, 10);
  if (end == text || *end != ':') {
    return FALSE;
  }
  const char *minutes_text = end + 1;
  if (!g_ascii_isdigit(minutes_text[0]) || !g_ascii_isdigit(minutes_text[1])) {
    return FALSE;
  }
  char *minutes_end = NULL;
  long minute = strtol(minutes_text, &minutes_end, 10);
  while (*minutes_end == ' ' || *minutes_end == '\t') {
    minutes_end++;
  }
  if (*minutes_end != '\0') {
    return FALSE;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return FALSE;
  }
  if (out_hour != NULL) {
    *out_hour = (int)hour;
  }
  if (out_minute != NULL) {
    *out_minute = (int)minute;
  }
  return TRUE;
}

static gboolean
parse_clock_time_entry(GtkEntry *entry, int *out_hour, int *out_minute)
{
  if (entry == NULL) {
    return FALSE;
  }
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  return parse_clock_time_text(text, out_hour, out_minute);
}

static int
sanitize_stats_sample_msec(int value)
{
  if (value < STATS_SAMPLE_MSEC_MIN || value > STATS_SAMPLE_MSEC_MAX) {
    return DEFAULT_STATS_SAMPLE_MSEC;
  }
  return value;
}

static int
current_stats_sample_msec(PumpkinWindow *self)
{
  if (self == NULL) {
    return DEFAULT_STATS_SAMPLE_MSEC;
  }
  return sanitize_stats_sample_msec(self->stats_sample_msec);
}

static gint64
query_stale_usec(PumpkinWindow *self)
{
  gint64 by_sample = (gint64)current_stats_sample_msec(self) * 10 * 1000;
  if (by_sample < 3000000) {
    by_sample = 3000000;
  }
  return by_sample;
}

static gint64
tps_query_interval_usec(PumpkinWindow *self)
{
  return (gint64)current_stats_sample_msec(self) * 1000;
}

static gint64
player_list_query_interval_usec(PumpkinWindow *self)
{
  return (gint64)current_stats_sample_msec(self) * 1000;
}

static void
restart_stats_refresh_timer(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  if (self->stats_refresh_id != 0) {
    g_source_remove(self->stats_refresh_id);
    self->stats_refresh_id = 0;
  }
  self->stats_refresh_id = g_timeout_add((guint)current_stats_sample_msec(self), update_stats_tick, self);
}

static void
restart_players_refresh_timer(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  if (self->players_refresh_id != 0) {
    g_source_remove(self->players_refresh_id);
    self->players_refresh_id = 0;
  }
  if (self->current != NULL && pumpkin_server_get_running(self->current)) {
    self->players_refresh_id = g_timeout_add((guint)current_stats_sample_msec(self), refresh_players_tick, self);
  }
}

static void
apply_stats_sample_msec(PumpkinWindow *self, int msec, gboolean reset_history)
{
  if (self == NULL) {
    return;
  }
  int sanitized = sanitize_stats_sample_msec(msec);
  if (self->stats_sample_msec == sanitized) {
    return;
  }
  self->stats_sample_msec = sanitized;
  if (reset_history) {
    reset_stats_history(self);
  }
  self->query_valid = FALSE;
  self->query_in_flight = FALSE;
  self->last_tps_request_at = 0;
  self->last_player_list_request_at = 0;
  restart_stats_refresh_timer(self);
  restart_players_refresh_timer(self);
}

static void
update_auto_update_controls_sensitivity(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  gboolean enabled = self->switch_auto_update != NULL &&
                     gtk_switch_get_active(self->switch_auto_update);
  gboolean use_schedule = self->switch_auto_update_schedule != NULL &&
                          gtk_switch_get_active(self->switch_auto_update_schedule);
  if (self->switch_auto_update_schedule != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->switch_auto_update_schedule), enabled);
  }
  if (self->entry_auto_update_time != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->entry_auto_update_time), enabled && use_schedule);
  }
}

static gboolean
query_is_fresh(PumpkinWindow *self)
{
  if (!self->query_valid) {
    return FALSE;
  }
  gint64 age = g_get_monotonic_time() - self->query_updated_at;
  return age >= 0 && age <= query_stale_usec(self);
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
      self->entry_server_port == NULL || self->entry_bedrock_port == NULL ||
      self->entry_max_players == NULL || self->entry_stats_sample_msec == NULL ||
      self->entry_rcon_host == NULL || self->entry_rcon_port == NULL) {
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
  gboolean stats_sample_has = FALSE;
  gboolean rcon_port_has = FALSE;
  int cpu_value = 0;
  int ram_value = 0;
  int port_value = 0;
  int bedrock_port_value = 0;
  int players_value = 0;
  int stats_sample_value = 0;
  int rcon_port_value = 0;
  gboolean cpu_parse_ok = parse_optional_positive_int(self->entry_max_cpu_cores, &cpu_value, &cpu_has);
  gboolean ram_parse_ok = parse_optional_positive_int(self->entry_max_ram_mb, &ram_value, &ram_has);
  gboolean port_parse_ok = parse_optional_positive_int(self->entry_server_port, &port_value, &port_has);
  gboolean bedrock_port_parse_ok =
    parse_optional_positive_int(self->entry_bedrock_port, &bedrock_port_value, &bedrock_port_has);
  gboolean players_parse_ok = parse_optional_positive_int(self->entry_max_players, &players_value, &players_has);
  gboolean stats_sample_parse_ok =
    parse_optional_positive_int(self->entry_stats_sample_msec, &stats_sample_value, &stats_sample_has);
  gboolean rcon_port_parse_ok = parse_optional_positive_int(self->entry_rcon_port, &rcon_port_value, &rcon_port_has);

  gboolean cpu_invalid = FALSE;
  gboolean ram_invalid = FALSE;
  gboolean port_invalid = FALSE;
  gboolean bedrock_port_invalid = FALSE;
  gboolean players_invalid = FALSE;
  gboolean stats_sample_invalid = FALSE;
  gboolean rcon_port_invalid = FALSE;
  gboolean rcon_host_invalid = FALSE;
  gboolean auto_update_time_invalid = FALSE;
  const char *cpu_hint = NULL;
  const char *ram_hint = NULL;
  const char *port_hint = NULL;
  const char *bedrock_port_hint = NULL;
  const char *players_hint = NULL;
  const char *stats_sample_hint = NULL;
  const char *rcon_port_hint = NULL;
  const char *rcon_host_hint = NULL;
  const char *auto_update_time_hint = NULL;
  const char *cpu_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_cpu_cores)));
  const char *ram_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_ram_mb)));
  const char *port_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_server_port)));
  const char *bedrock_port_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_bedrock_port)));
  const char *players_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_players)));
  const char *stats_sample_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_stats_sample_msec)));
  const char *rcon_port_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_port)));
  const char *rcon_host_text = skip_ws(gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_host)));
  gboolean auto_update_enabled = self->switch_auto_update != NULL &&
                                 gtk_switch_get_active(self->switch_auto_update);
  gboolean auto_update_use_schedule = self->switch_auto_update_schedule != NULL &&
                                      gtk_switch_get_active(self->switch_auto_update_schedule);
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

  if (!stats_sample_parse_ok) {
    stats_sample_invalid = TRUE;
    if (stats_sample_text != NULL && *stats_sample_text == '-') {
      stats_sample_hint = "No negative values allowed.";
    } else {
      stats_sample_hint = "Update rate must be a number.";
    }
  } else if (!stats_sample_has) {
    stats_sample_invalid = TRUE;
    stats_sample_hint = "Update rate is required.";
  } else if (stats_sample_value < STATS_SAMPLE_MSEC_MIN || stats_sample_value > STATS_SAMPLE_MSEC_MAX) {
    stats_sample_invalid = TRUE;
    stats_sample_hint = "Update rate must be between 2ms and 2000ms.";
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

  if (auto_update_enabled && auto_update_use_schedule) {
    int update_hour = 0;
    int update_minute = 0;
    if (!parse_clock_time_entry(self->entry_auto_update_time, &update_hour, &update_minute)) {
      auto_update_time_invalid = TRUE;
      auto_update_time_hint = "Use HH:MM (24h), e.g. 01:00.";
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
  if (stats_sample_invalid) {
    gtk_widget_add_css_class(GTK_WIDGET(self->entry_stats_sample_msec), "error");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_stats_sample_msec), "error");
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
  if (self->entry_auto_update_time != NULL) {
    if (auto_update_time_invalid) {
      gtk_widget_add_css_class(GTK_WIDGET(self->entry_auto_update_time), "error");
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(self->entry_auto_update_time), "error");
    }
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
  if (self->label_stats_sample_hint != NULL) {
    if (stats_sample_invalid) {
      gtk_label_set_text(self->label_stats_sample_hint,
                         stats_sample_hint != NULL ? stats_sample_hint : "Update rate is invalid.");
      gtk_widget_set_visible(GTK_WIDGET(self->label_stats_sample_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_stats_sample_hint), FALSE);
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
  if (self->label_auto_update_time_hint != NULL) {
    if (auto_update_time_invalid) {
      gtk_label_set_text(self->label_auto_update_time_hint,
                         auto_update_time_hint != NULL ? auto_update_time_hint : "Time is invalid.");
      gtk_widget_set_visible(GTK_WIDGET(self->label_auto_update_time_hint), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->label_auto_update_time_hint), FALSE);
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
                           players_invalid || stats_sample_invalid || rcon_port_invalid || rcon_host_invalid ||
                           auto_update_time_invalid;
  update_auto_update_controls_sensitivity(self);
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
complete_pending_settings_navigation(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
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

  g_clear_pointer(&self->pending_details_page, g_free);
  g_clear_pointer(&self->pending_view_page, g_free);
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
    if (self->settings_dirty) {
      return;
    }
  } else if (g_strcmp0(response, "discard") == 0) {
    discard_settings_changes(self);
  }

  complete_pending_settings_navigation(self);
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

  GApplication *app = G_APPLICATION(gtk_window_get_application(window));
  if (run_in_background && PUMPKIN_IS_APP(app) &&
      !pumpkin_app_is_tray_available(PUMPKIN_APP(app))) {
    run_in_background = FALSE;
  }

  if (run_in_background) {
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    if (!self->background_hold) {
      if (app != NULL) {
        g_application_hold(app);
        self->background_hold = TRUE;
      }
    }
    return TRUE;
  }
  pumpkin_window_stop_all_servers(self);
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

static const char *
console_level_token_tag_name(ConsoleLevel level)
{
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      return "console-token-trace";
    case CONSOLE_LEVEL_DEBUG:
      return "console-token-debug";
    case CONSOLE_LEVEL_INFO:
      return "console-token-info";
    case CONSOLE_LEVEL_WARN:
      return "console-token-warn";
    case CONSOLE_LEVEL_ERROR:
      return "console-token-error";
    case CONSOLE_LEVEL_SMPK:
      return "console-token-smpk";
    case CONSOLE_LEVEL_OTHER:
    default:
      return NULL;
  }
}

static void
apply_console_tag_offsets(GtkTextBuffer *buffer, int line_start_offset, int start, int end, const char *tag_name)
{
  if (buffer == NULL || tag_name == NULL || *tag_name == '\0' || start < 0 || end <= start) {
    return;
  }
  GtkTextIter iter_start;
  GtkTextIter iter_end;
  gtk_text_buffer_get_iter_at_offset(buffer, &iter_start, line_start_offset + start);
  gtk_text_buffer_get_iter_at_offset(buffer, &iter_end, line_start_offset + end);
  gtk_text_buffer_apply_tag_by_name(buffer, tag_name, &iter_start, &iter_end);
}

static void
apply_console_inline_tags(GtkTextBuffer *buffer, int line_start_offset, const char *display, ConsoleLevel level)
{
  if (buffer == NULL || display == NULL || *display == '\0') {
    return;
  }

  const char *level_word = NULL;
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      level_word = "TRACE";
      break;
    case CONSOLE_LEVEL_DEBUG:
      level_word = "DEBUG";
      break;
    case CONSOLE_LEVEL_INFO:
      level_word = "INFO";
      break;
    case CONSOLE_LEVEL_WARN:
      level_word = "WARN";
      break;
    case CONSOLE_LEVEL_ERROR:
      level_word = "ERROR";
      break;
    case CONSOLE_LEVEL_SMPK:
      level_word = "SMPK";
      break;
    case CONSOLE_LEVEL_OTHER:
    default:
      break;
  }

  const char *level_tag_name = console_level_token_tag_name(level);
  if (level_word != NULL && level_tag_name != NULL) {
    g_autofree char *level_token = g_strdup_printf("[%s]", level_word);
    const char *token_pos = level_token != NULL ? strstr(display, level_token) : NULL;
    if (token_pos != NULL) {
      int token_start = (int)(token_pos - display) + 1;
      int token_end = token_start + (int)strlen(level_word);
      apply_console_tag_offsets(buffer, line_start_offset, token_start, token_end, level_tag_name);
    }
  }

  static GRegex *startup_ms_re = NULL;
  if (startup_ms_re == NULL) {
    startup_ms_re = g_regex_new("\\b[0-9]+ms\\b", G_REGEX_OPTIMIZE, 0, NULL);
  }
  if (startup_ms_re == NULL) {
    return;
  }

  g_autoptr(GMatchInfo) match = NULL;
  g_regex_match(startup_ms_re, display, 0, &match);
  while (match != NULL && g_match_info_matches(match)) {
    int start = -1;
    int end = -1;
    if (g_match_info_fetch_pos(match, 0, &start, &end) && start >= 0 && end > start) {
      apply_console_tag_offsets(buffer, line_start_offset, start, end, "console-token-startup-ms");
    }
    if (!g_match_info_next(match, NULL)) {
      break;
    }
  }
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
  ConsoleLevel level = CONSOLE_LEVEL_OTHER;
  g_autofree char *display = format_console_line(self, line, &level);
  if (display == NULL || *display == '\0') {
    return;
  }

  GtkTextBuffer *buffer = g_hash_table_lookup(self->console_buffers, server);
  if (buffer == NULL) {
    buffer = gtk_text_buffer_new(NULL);
    g_hash_table_insert(self->console_buffers, g_object_ref(server), buffer);
  }
  ensure_console_buffer_tags(self, buffer);

  if (self->current == server && gtk_text_view_get_buffer(self->log_view) != buffer) {
    gtk_text_view_set_buffer(self->log_view, buffer);
  }

  g_autofree char *with_newline = g_strdup_printf("%s\n", display);
  int line_start_offset = gtk_text_buffer_get_char_count(buffer);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  const char *tag_name = console_level_tag_name(level);
  if (tag_name != NULL && *tag_name != '\0') {
    gtk_text_buffer_insert_with_tags_by_name(buffer, &end, with_newline, -1, tag_name, NULL);
  } else {
    gtk_text_buffer_insert(buffer, &end, with_newline, -1);
  }
  apply_console_inline_tags(buffer, line_start_offset, display, level);
  gtk_text_buffer_get_end_iter(buffer, &end);

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
  gboolean internal_line =
    line != NULL &&
    (g_strcmp0(line, "Server process exited") == 0 ||
     g_strcmp0(line, "Auto-restart scheduled") == 0);
  g_autofree char *clean = line != NULL ? strip_ansi(line) : NULL;
  const char *check = clean != NULL ? clean : line;
  gboolean tps_line = FALSE;
  gboolean list_line = is_player_list_snapshot_line(check);
  if (check != NULL) {
    double tps = 0.0;
    if (parse_tps_from_line(check, &tps)) {
      self->last_tps = tps;
      self->last_tps_valid = TRUE;
      self->tps_enabled = TRUE;
      tps_line = TRUE;
    }
  }
  gboolean suppress_auto_line = FALSE;
  if (self->current == server) {
    if (tps_line && self->pending_auto_tps_lines > 0) {
      self->pending_auto_tps_lines--;
      suppress_auto_line = TRUE;
    }
    if (list_line && self->pending_auto_list_lines > 0) {
      self->pending_auto_list_lines--;
      suppress_auto_line = TRUE;
    }
  }
  if (!internal_line && !suppress_auto_line) {
    append_console_line(self, server, line);
  }
  update_live_player_names(self, line);
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
    if (self->auto_update_server == server) {
      clear_auto_update_countdown(self);
    }
    if (self->current == server) {
      player_states_mark_all_offline(self);
      player_states_save(self, server);
      self->tps_enabled = FALSE;
      self->last_tps_valid = FALSE;
      self->pending_auto_tps_lines = 0;
      self->pending_auto_list_lines = 0;
      self->pending_java_platform_hints = 0;
      self->pending_bedrock_platform_hints = 0;
    }
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
  if (self == NULL || line == NULL || *line == '\0') {
    return;
  }
  if (self->current == NULL) {
    return;
  }
  g_autofree char *prefixed = g_strdup_printf("[SMPK] %s", line);
  append_console_line(self, self->current, prefixed);
}

static void
append_log_for_server(PumpkinWindow *self, PumpkinServer *server, const char *line)
{
  if (self == NULL || line == NULL || *line == '\0') {
    return;
  }
  PumpkinServer *target = server != NULL ? server : self->current;
  if (target == NULL) {
    return;
  }
  g_autofree char *prefixed = g_strdup_printf("[SMPK] %s", line);
  append_console_line(self, target, prefixed);
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
normalized_key(const char *text)
{
  if (text == NULL) {
    return NULL;
  }
  g_autofree char *tmp = g_strdup(text);
  g_strstrip(tmp);
  if (tmp[0] == '\0') {
    return NULL;
  }
  return g_ascii_strdown(tmp, -1);
}

static char *
player_tracking_file(PumpkinServer *server)
{
  if (server == NULL) {
    return NULL;
  }
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  if (data_dir == NULL) {
    return NULL;
  }
  return g_build_filename(data_dir, "player-tracking.ini", NULL);
}

static void
player_state_free(PlayerState *state)
{
  if (state == NULL) {
    return;
  }
  g_clear_pointer(&state->key, g_free);
  g_clear_pointer(&state->name, g_free);
  g_clear_pointer(&state->uuid, g_free);
  g_clear_pointer(&state->last_ip, g_free);
  g_free(state);
}

static guint64
player_state_effective_playtime(const PlayerState *state)
{
  if (state == NULL) {
    return 0;
  }
  guint64 total = state->playtime_seconds;
  if (state->online && state->session_started_mono > 0) {
    gint64 now_mono = g_get_monotonic_time();
    if (now_mono > state->session_started_mono) {
      total += (guint64)((now_mono - state->session_started_mono) / G_USEC_PER_SEC);
    }
  }
  return total;
}

static void
player_states_set_dirty(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  self->player_state_dirty = TRUE;
}

static void
repoint_player_indexes(PumpkinWindow *self, PlayerState *from_state, PlayerState *to_state)
{
  if (self == NULL || from_state == NULL || to_state == NULL) {
    return;
  }

  if (self->player_states_by_uuid != NULL) {
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->player_states_by_uuid);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      if (value == from_state) {
        g_hash_table_iter_replace(&iter, to_state);
      }
    }
  }

  if (self->player_states_by_name != NULL) {
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->player_states_by_name);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      if (value == from_state) {
        g_hash_table_iter_replace(&iter, to_state);
      }
    }
  }
}

static void
merge_player_states(PumpkinWindow *self, PlayerState *dst, PlayerState *src)
{
  if (self == NULL || dst == NULL || src == NULL || dst == src) {
    return;
  }

  if ((dst->name == NULL || dst->name[0] == '\0') && src->name != NULL && src->name[0] != '\0') {
    g_free(dst->name);
    dst->name = g_strdup(src->name);
  }
  if ((dst->uuid == NULL || dst->uuid[0] == '\0') && src->uuid != NULL && src->uuid[0] != '\0') {
    g_free(dst->uuid);
    dst->uuid = g_strdup(src->uuid);
  }
  if (dst->platform == PLAYER_PLATFORM_UNKNOWN && src->platform != PLAYER_PLATFORM_UNKNOWN) {
    dst->platform = src->platform;
  }
  if ((dst->last_ip == NULL || dst->last_ip[0] == '\0') &&
      src->last_ip != NULL && src->last_ip[0] != '\0') {
    g_free(dst->last_ip);
    dst->last_ip = g_strdup(src->last_ip);
  } else if (src->last_ip != NULL && src->last_ip[0] != '\0' &&
             src->last_online_unix >= dst->last_online_unix &&
             g_strcmp0(dst->last_ip, src->last_ip) != 0) {
    g_free(dst->last_ip);
    dst->last_ip = g_strdup(src->last_ip);
  }
  if (dst->first_joined_unix <= 0 ||
      (src->first_joined_unix > 0 && src->first_joined_unix < dst->first_joined_unix)) {
    dst->first_joined_unix = src->first_joined_unix;
  }
  if (src->last_online_unix > dst->last_online_unix) {
    dst->last_online_unix = src->last_online_unix;
  }
  dst->playtime_seconds += src->playtime_seconds;

  if (src->online) {
    if (!dst->online) {
      dst->online = TRUE;
      dst->session_started_mono = src->session_started_mono > 0
                                    ? src->session_started_mono
                                    : g_get_monotonic_time();
    } else if (src->session_started_mono > 0 &&
               (dst->session_started_mono <= 0 || src->session_started_mono < dst->session_started_mono)) {
      dst->session_started_mono = src->session_started_mono;
    }
  }

  repoint_player_indexes(self, src, dst);
  if (self->player_states != NULL && src->key != NULL) {
    g_hash_table_remove(self->player_states, src->key);
  }
  player_states_set_dirty(self);
}

static void
allow_deleted_player_tracking(PumpkinWindow *self, const char *uuid, const char *name)
{
  if (self == NULL || self->deleted_player_keys == NULL) {
    return;
  }

  gboolean changed = FALSE;
  g_autofree char *uuid_key = normalized_key(uuid);
  g_autofree char *name_key = normalized_key(name);

  if (uuid_key != NULL) {
    changed = g_hash_table_remove(self->deleted_player_keys, uuid_key) || changed;
  }
  if (name_key != NULL) {
    changed = g_hash_table_remove(self->deleted_player_keys, name_key) || changed;
  }
  if (changed) {
    player_states_set_dirty(self);
  }
}

static PlayerState *
ensure_player_state(PumpkinWindow *self, const char *uuid, const char *name, gboolean create)
{
  if (self == NULL || self->player_states == NULL) {
    return NULL;
  }

  g_autofree char *uuid_key = normalized_key(uuid);
  g_autofree char *name_key = normalized_key(name);
  if (self->deleted_player_keys != NULL) {
    if (uuid_key != NULL && g_hash_table_contains(self->deleted_player_keys, uuid_key)) {
      return NULL;
    }
    if (name_key != NULL && g_hash_table_contains(self->deleted_player_keys, name_key)) {
      return NULL;
    }
  }
  PlayerState *state_by_uuid = NULL;
  PlayerState *state_by_name = NULL;

  if (uuid_key != NULL && self->player_states_by_uuid != NULL) {
    state_by_uuid = g_hash_table_lookup(self->player_states_by_uuid, uuid_key);
  }
  if (name_key != NULL && self->player_states_by_name != NULL) {
    state_by_name = g_hash_table_lookup(self->player_states_by_name, name_key);
  }

  PlayerState *state = NULL;
  if (state_by_uuid != NULL) {
    state = state_by_uuid;
    if (state_by_name != NULL && state_by_name != state_by_uuid) {
      merge_player_states(self, state, state_by_name);
    }
  } else if (state_by_name != NULL) {
    state = state_by_name;
  } else if (create) {
    const char *key = uuid_key != NULL ? uuid_key : name_key;
    if (key == NULL) {
      return NULL;
    }
    state = g_new0(PlayerState, 1);
    state->key = g_strdup(key);
    g_hash_table_insert(self->player_states, g_strdup(key), state);
    player_states_set_dirty(self);
  } else {
    return NULL;
  }

  if (state != NULL && uuid != NULL && *uuid != '\0') {
    if (state->uuid == NULL || *state->uuid == '\0') {
      g_free(state->uuid);
      state->uuid = g_strdup(uuid);
      player_states_set_dirty(self);
    }
    if (uuid_key != NULL && self->player_states_by_uuid != NULL) {
      g_hash_table_replace(self->player_states_by_uuid, g_strdup(uuid_key), state);
    }
  }

  if (state != NULL && name != NULL && *name != '\0') {
    if (state->name == NULL || g_strcmp0(state->name, name) != 0) {
      g_free(state->name);
      state->name = g_strdup(name);
      player_states_set_dirty(self);
    }
    if (name_key != NULL && self->player_states_by_name != NULL) {
      g_hash_table_replace(self->player_states_by_name, g_strdup(name_key), state);
    }
  }

  return state;
}

static void
player_state_mark_online(PumpkinWindow *self, PlayerState *state, PlayerPlatform platform_hint)
{
  if (self == NULL || state == NULL) {
    return;
  }
  gint64 now_unix = (gint64)time(NULL);
  if (platform_hint != PLAYER_PLATFORM_UNKNOWN) {
    state->platform = platform_hint;
  }
  if (!state->online) {
    state->online = TRUE;
    state->session_started_mono = g_get_monotonic_time();
    if (state->first_joined_unix <= 0) {
      state->first_joined_unix = now_unix;
    }
  }
  if (now_unix > state->last_online_unix) {
    state->last_online_unix = now_unix;
  }
  if (self->live_player_names != NULL) {
    const char *presence = state->name != NULL ? state->name : state->uuid;
    if (presence != NULL && *presence != '\0') {
      g_hash_table_replace(self->live_player_names, g_strdup(presence), g_strdup(presence));
    }
  }
  player_states_set_dirty(self);
}

static void
player_state_mark_offline(PumpkinWindow *self, PlayerState *state)
{
  if (self == NULL || state == NULL) {
    return;
  }
  gint64 now_unix = (gint64)time(NULL);
  if (state->online) {
    if (state->session_started_mono > 0) {
      gint64 now_mono = g_get_monotonic_time();
      if (now_mono > state->session_started_mono) {
        state->playtime_seconds += (guint64)((now_mono - state->session_started_mono) / G_USEC_PER_SEC);
      }
    }
    state->session_started_mono = 0;
    state->online = FALSE;
    if (now_unix > state->last_online_unix) {
      state->last_online_unix = now_unix;
    }
    player_states_set_dirty(self);
  }

  if (self->live_player_names != NULL) {
    if (state->name != NULL) {
      g_hash_table_remove(self->live_player_names, state->name);
    }
    if (state->uuid != NULL) {
      g_hash_table_remove(self->live_player_names, state->uuid);
    }
  }
}

static void
player_states_mark_all_offline(PumpkinWindow *self)
{
  if (self == NULL || self->player_states == NULL) {
    return;
  }
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->player_states);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    PlayerState *state = value;
    player_state_mark_offline(self, state);
  }
}

static void
player_state_remove_from_index_table(GHashTable *table, PlayerState *state)
{
  if (table == NULL || state == NULL) {
    return;
  }
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, table);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    if (value == state) {
      g_hash_table_iter_remove(&iter);
    }
  }
}

static gboolean
delete_player_tracking(PumpkinWindow *self, const char *state_key, const char *name, const char *uuid)
{
  if (self == NULL || self->current == NULL || self->player_states == NULL) {
    return FALSE;
  }

  PlayerState *state = NULL;
  if (state_key != NULL && *state_key != '\0') {
    state = g_hash_table_lookup(self->player_states, state_key);
  }
  if (state == NULL && uuid != NULL && *uuid != '\0' && self->player_states_by_uuid != NULL) {
    g_autofree char *uuid_key = normalized_key(uuid);
    if (uuid_key != NULL) {
      state = g_hash_table_lookup(self->player_states_by_uuid, uuid_key);
    }
  }
  if (state == NULL && name != NULL && *name != '\0' && self->player_states_by_name != NULL) {
    g_autofree char *name_key = normalized_key(name);
    if (name_key != NULL) {
      state = g_hash_table_lookup(self->player_states_by_name, name_key);
    }
  }
  if (state == NULL) {
    return FALSE;
  }

  g_autofree char *key_copy = state->key != NULL ? g_strdup(state->key) : NULL;
  g_autofree char *name_copy = state->name != NULL ? g_strdup(state->name) : NULL;
  g_autofree char *uuid_copy = state->uuid != NULL ? g_strdup(state->uuid) : NULL;
  if (self->deleted_player_keys != NULL) {
    g_autofree char *norm_key = normalized_key(key_copy);
    g_autofree char *norm_name = normalized_key(name_copy);
    g_autofree char *norm_uuid = normalized_key(uuid_copy);
    if (norm_key != NULL) {
      g_hash_table_add(self->deleted_player_keys, g_strdup(norm_key));
    }
    if (norm_name != NULL) {
      g_hash_table_add(self->deleted_player_keys, g_strdup(norm_name));
    }
    if (norm_uuid != NULL) {
      g_hash_table_add(self->deleted_player_keys, g_strdup(norm_uuid));
    }
  }

  if (self->live_player_names != NULL) {
    if (name_copy != NULL) {
      g_hash_table_remove(self->live_player_names, name_copy);
    }
    if (uuid_copy != NULL) {
      g_hash_table_remove(self->live_player_names, uuid_copy);
    }
  }

  player_state_remove_from_index_table(self->player_states_by_uuid, state);
  player_state_remove_from_index_table(self->player_states_by_name, state);

  if (key_copy != NULL) {
    g_hash_table_remove(self->player_states, key_copy);
  } else {
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->player_states);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      if (value == state) {
        g_hash_table_iter_remove(&iter);
        break;
      }
    }
  }

  if (uuid_copy != NULL && *uuid_copy != '\0') {
    g_autofree char *uuid_key = normalized_key(uuid_copy);
    if (uuid_key != NULL) {
      if (self->player_head_downloads != NULL) {
        g_hash_table_remove(self->player_head_downloads, uuid_key);
      }
      g_autofree char *data_dir = pumpkin_server_get_data_dir(self->current);
      g_autofree char *cache_path = g_strdup_printf("%s/cache/player-heads/%s.png", data_dir, uuid_key);
      g_remove(cache_path);
    }
  }

  player_states_set_dirty(self);
  player_states_save(self, self->current);
  invalidate_player_list_signature(self);
  refresh_player_list(self);
  return TRUE;
}

static int
player_online_count(PumpkinWindow *self)
{
  if (self == NULL || self->player_states == NULL) {
    return 0;
  }
  int count = 0;
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->player_states);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    PlayerState *state = value;
    if (state->online) {
      count++;
    }
  }
  return count;
}

static void
player_states_clear(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  if (self->live_player_names != NULL) {
    g_hash_table_remove_all(self->live_player_names);
  }
  if (self->platform_hint_by_ip != NULL) {
    g_hash_table_remove_all(self->platform_hint_by_ip);
  }
  if (self->player_states != NULL) {
    g_hash_table_remove_all(self->player_states);
  }
  if (self->player_states_by_uuid != NULL) {
    g_hash_table_remove_all(self->player_states_by_uuid);
  }
  if (self->player_states_by_name != NULL) {
    g_hash_table_remove_all(self->player_states_by_name);
  }
  if (self->deleted_player_keys != NULL) {
    g_hash_table_remove_all(self->deleted_player_keys);
  }
  self->player_state_dirty = FALSE;
  invalidate_player_list_signature(self);
}

static void
player_states_load(PumpkinWindow *self, PumpkinServer *server)
{
  if (self == NULL) {
    return;
  }
  player_states_clear(self);
  if (server == NULL) {
    return;
  }

  g_autofree char *path = player_tracking_file(server);
  if (path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS)) {
    return;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
    return;
  }

  if (self->deleted_player_keys != NULL && g_key_file_has_group(key_file, "__deleted")) {
    gsize deleted_len = 0;
    g_auto(GStrv) deleted_keys = g_key_file_get_keys(key_file, "__deleted", &deleted_len, NULL);
    for (gsize i = 0; i < deleted_len; i++) {
      const char *entry_key = deleted_keys[i];
      if (entry_key == NULL || *entry_key == '\0') {
        continue;
      }
      g_autofree char *stored = g_key_file_get_string(key_file, "__deleted", entry_key, NULL);
      g_autofree char *normalized = normalized_key(stored);
      if (normalized != NULL) {
        g_hash_table_add(self->deleted_player_keys, g_strdup(normalized));
      }
    }
  }

  gsize groups_len = 0;
  g_auto(GStrv) groups = g_key_file_get_groups(key_file, &groups_len);
  for (gsize i = 0; i < groups_len; i++) {
    const char *group = groups[i];
    if (group == NULL || *group == '\0') {
      continue;
    }
    if (g_strcmp0(group, "__deleted") == 0) {
      continue;
    }

    PlayerState *state = g_new0(PlayerState, 1);
    state->key = g_strdup(group);
    state->name = g_key_file_get_string(key_file, group, "name", NULL);
    state->uuid = g_key_file_get_string(key_file, group, "uuid", NULL);
    state->last_ip = g_key_file_get_string(key_file, group, "last_ip", NULL);
    state->platform = (PlayerPlatform)g_key_file_get_integer(key_file, group, "platform", NULL);
    if (state->platform < PLAYER_PLATFORM_UNKNOWN || state->platform > PLAYER_PLATFORM_BEDROCK) {
      state->platform = PLAYER_PLATFORM_UNKNOWN;
    }
    state->first_joined_unix = g_key_file_get_int64(key_file, group, "first_joined_unix", NULL);
    state->last_online_unix = g_key_file_get_int64(key_file, group, "last_online_unix", NULL);
    gint64 saved_playtime = g_key_file_get_int64(key_file, group, "playtime_seconds", NULL);
    state->playtime_seconds = saved_playtime > 0 ? (guint64)saved_playtime : 0;
    state->online = FALSE;
    state->session_started_mono = 0;

    g_hash_table_insert(self->player_states, g_strdup(group), state);

    g_autofree char *uuid_key = normalized_key(state->uuid);
    if (uuid_key != NULL) {
      g_hash_table_replace(self->player_states_by_uuid, g_strdup(uuid_key), state);
    }
    g_autofree char *name_key = normalized_key(state->name);
    if (name_key != NULL) {
      g_hash_table_replace(self->player_states_by_name, g_strdup(name_key), state);
    }
  }
  self->player_state_dirty = FALSE;
  invalidate_player_list_signature(self);
}

static void
player_states_save(PumpkinWindow *self, PumpkinServer *server)
{
  if (self == NULL || server == NULL || self->player_states == NULL || !self->player_state_dirty) {
    return;
  }

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->player_states);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *section = key;
    PlayerState *state = value;
    if (section == NULL || state == NULL) {
      continue;
    }
    if (state->name != NULL) {
      g_key_file_set_string(key_file, section, "name", state->name);
    }
    if (state->uuid != NULL) {
      g_key_file_set_string(key_file, section, "uuid", state->uuid);
    }
    if (state->last_ip != NULL && *state->last_ip != '\0') {
      g_key_file_set_string(key_file, section, "last_ip", state->last_ip);
    }
    g_key_file_set_integer(key_file, section, "platform", (int)state->platform);
    g_key_file_set_int64(key_file, section, "first_joined_unix", state->first_joined_unix);
    g_key_file_set_int64(key_file, section, "last_online_unix", state->last_online_unix);
    g_key_file_set_int64(key_file, section, "playtime_seconds",
                         (gint64)player_state_effective_playtime(state));
  }

  if (self->deleted_player_keys != NULL && g_hash_table_size(self->deleted_player_keys) > 0) {
    GHashTableIter deleted_iter;
    gpointer deleted_key = NULL;
    gpointer deleted_value = NULL;
    guint index = 0;
    g_hash_table_iter_init(&deleted_iter, self->deleted_player_keys);
    while (g_hash_table_iter_next(&deleted_iter, &deleted_key, &deleted_value)) {
      const char *deleted_token = deleted_key;
      if (deleted_token == NULL || *deleted_token == '\0') {
        continue;
      }
      g_autofree char *entry_key = g_strdup_printf("k%u", index++);
      g_key_file_set_string(key_file, "__deleted", entry_key, deleted_token);
    }
  }

  gsize out_len = 0;
  g_autofree char *serialized = g_key_file_to_data(key_file, &out_len, NULL);
  if (serialized == NULL) {
    return;
  }

  g_autofree char *path = player_tracking_file(server);
  if (path == NULL) {
    return;
  }
  if (g_file_set_contents(path, serialized, (gssize)out_len, NULL)) {
    self->player_state_dirty = FALSE;
    self->last_player_state_flush_at = g_get_monotonic_time();
  }
}

static const char *
platform_label(PlayerPlatform platform)
{
  if (platform == PLAYER_PLATFORM_JAVA) {
    return "Java";
  }
  if (platform == PLAYER_PLATFORM_BEDROCK) {
    return "Bedrock";
  }
  return "Unknown";
}

static PlayerPlatform
platform_from_line(const char *line)
{
  if (line == NULL) {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  g_autofree char *lower = g_ascii_strdown(line, -1);
  if (strstr(lower, "bedrock") != NULL ||
      strstr(lower, "floodgate") != NULL ||
      strstr(lower, "geyser") != NULL ||
      strstr(lower, "raknet") != NULL) {
    return PLAYER_PLATFORM_BEDROCK;
  }
  if (strstr(lower, "java") != NULL || strstr(lower, "java edition") != NULL) {
    return PLAYER_PLATFORM_JAVA;
  }
  return PLAYER_PLATFORM_UNKNOWN;
}

static PlayerPlatform
platform_guess_from_uuid(const char *uuid)
{
  if (uuid == NULL || *uuid == '\0') {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  g_autofree char *lower = g_ascii_strdown(uuid, -1);
  if (g_str_has_prefix(lower, "00000000-0000-0000-")) {
    return PLAYER_PLATFORM_BEDROCK;
  }
  return PLAYER_PLATFORM_UNKNOWN;
}

static char *
extract_ip_from_socket_text(const char *text)
{
  if (text == NULL || *text == '\0') {
    return NULL;
  }

  while (*text == ' ' || *text == '\t') {
    text++;
  }
  if (*text == '\0') {
    return NULL;
  }

  g_autofree char *tmp = g_strdup(text);
  g_strstrip(tmp);
  gsize len = strlen(tmp);
  while (len > 0 && (tmp[len - 1] == ',' || tmp[len - 1] == ')' || tmp[len - 1] == ']')) {
    tmp[--len] = '\0';
  }

  if (tmp[0] == '[') {
    const char *end = strchr(tmp, ']');
    if (end != NULL && end > tmp + 1) {
      return g_strndup(tmp + 1, (gsize)(end - (tmp + 1)));
    }
  }

  const char *last_colon = strrchr(tmp, ':');
  if (last_colon != NULL) {
    gboolean has_other_colon = FALSE;
    for (const char *p = tmp; p < last_colon; p++) {
      if (*p == ':') {
        has_other_colon = TRUE;
        break;
      }
    }
    if (!has_other_colon) {
      return g_strndup(tmp, (gsize)(last_colon - tmp));
    }
  }

  return g_strdup(tmp);
}

static void
remember_platform_hint_for_ip(PumpkinWindow *self, const char *ip, PlayerPlatform platform)
{
  if (self == NULL || self->platform_hint_by_ip == NULL || ip == NULL || *ip == '\0' ||
      platform == PLAYER_PLATFORM_UNKNOWN) {
    return;
  }
  g_autofree char *ip_key = normalized_key(ip);
  if (ip_key == NULL || *ip_key == '\0') {
    return;
  }
  g_hash_table_replace(self->platform_hint_by_ip, g_strdup(ip_key), GINT_TO_POINTER((int)platform));
}

static PlayerPlatform
platform_hint_for_ip(PumpkinWindow *self, const char *ip)
{
  if (self == NULL || self->platform_hint_by_ip == NULL || ip == NULL || *ip == '\0') {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  g_autofree char *ip_key = normalized_key(ip);
  if (ip_key == NULL || *ip_key == '\0') {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  gpointer raw = g_hash_table_lookup(self->platform_hint_by_ip, ip_key);
  int value = GPOINTER_TO_INT(raw);
  if (value < PLAYER_PLATFORM_UNKNOWN || value > PLAYER_PLATFORM_BEDROCK) {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  return (PlayerPlatform)value;
}

static PlayerPlatform
take_pending_platform_hint(PumpkinWindow *self)
{
  if (self == NULL) {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  if (self->pending_bedrock_platform_hints > 0) {
    self->pending_bedrock_platform_hints--;
    return PLAYER_PLATFORM_BEDROCK;
  }
  if (self->pending_java_platform_hints > 0) {
    self->pending_java_platform_hints--;
    return PLAYER_PLATFORM_JAVA;
  }
  return PLAYER_PLATFORM_UNKNOWN;
}

static const char *
date_time_pattern_for_config(PumpkinWindow *self)
{
  PumpkinDateFormat format = PUMPKIN_DATE_FORMAT_DMY;
  PumpkinTimeFormat time_format = PUMPKIN_TIME_FORMAT_24H;
  if (self != NULL && self->config != NULL) {
    format = pumpkin_config_get_date_format(self->config);
    time_format = pumpkin_config_get_time_format(self->config);
  }
  if (format == PUMPKIN_DATE_FORMAT_YMD) {
    return time_format == PUMPKIN_TIME_FORMAT_12H
             ? "%Y-%m-%d %I:%M %p"
             : "%Y-%m-%d %H:%M";
  }
  if (format == PUMPKIN_DATE_FORMAT_MDY) {
    return time_format == PUMPKIN_TIME_FORMAT_12H
             ? "%m/%d/%Y %I:%M %p"
             : "%m/%d/%Y %H:%M";
  }
  return time_format == PUMPKIN_TIME_FORMAT_12H
           ? "%d.%m.%Y %I:%M %p"
           : "%d.%m.%Y %H:%M";
}

static char *
normalize_build_label(PumpkinWindow *self, const char *label)
{
  if (label == NULL || *label == '\0') {
    return NULL;
  }

  const char *text = label;
  if (g_str_has_prefix(text, "Build ")) {
    text += 6;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;

  if (sscanf(text, "%d-%d-%d %d:%d UTC", &year, &month, &day, &hour, &minute) == 5) {
    g_autoptr(GDateTime) utc_dt = g_date_time_new_utc(year, month, day, hour, minute, 0.0);
    if (utc_dt != NULL) {
      g_autoptr(GDateTime) local_dt = g_date_time_to_local(utc_dt);
      if (local_dt != NULL) {
        g_autofree char *formatted = g_date_time_format(local_dt, date_time_pattern_for_config(self));
        return g_strdup_printf("Build %s", formatted);
      }
    }
  }

  if (sscanf(text, "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) == 5) {
    g_autoptr(GDateTime) local_dt = g_date_time_new_local(year, month, day, hour, minute, 0.0);
    if (local_dt != NULL) {
      g_autofree char *formatted = g_date_time_format(local_dt, date_time_pattern_for_config(self));
      return g_strdup_printf("Build %s", formatted);
    }
  }

  if (sscanf(text, "%d-%d-%d", &year, &month, &day) == 3) {
    g_autoptr(GDateTime) local_dt = g_date_time_new_local(year, month, day, 0, 0, 0.0);
    if (local_dt != NULL) {
      g_autofree char *formatted = g_date_time_format(local_dt, date_time_pattern_for_config(self));
      return g_strdup_printf("Build %s", formatted);
    }
  }

  return g_strdup(label);
}

static char *
format_duration(guint64 seconds)
{
  guint64 days = seconds / 86400;
  seconds %= 86400;
  guint64 hours = seconds / 3600;
  seconds %= 3600;
  guint64 minutes = seconds / 60;
  guint64 secs = seconds % 60;

  if (days > 0) {
    return g_strdup_printf("%" G_GUINT64_FORMAT "d %" G_GUINT64_FORMAT "h", days, hours);
  }
  if (hours > 0) {
    return g_strdup_printf("%" G_GUINT64_FORMAT "h %" G_GUINT64_FORMAT "m", hours, minutes);
  }
  if (minutes > 0) {
    return g_strdup_printf("%" G_GUINT64_FORMAT "m %" G_GUINT64_FORMAT "s", minutes, secs);
  }
  return g_strdup_printf("%" G_GUINT64_FORMAT "s", secs);
}

static char *
format_unix_time(PumpkinWindow *self, gint64 unix_ts)
{
  if (unix_ts <= 0) {
    return g_strdup("Never");
  }
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(unix_ts);
  if (dt == NULL) {
    return g_strdup("Never");
  }
  return g_date_time_format(dt, date_time_pattern_for_config(self));
}

static char *
relative_time_label(gint64 unix_ts)
{
  if (unix_ts <= 0) {
    return g_strdup("Never");
  }
  gint64 now = (gint64)time(NULL);
  if (now < unix_ts) {
    now = unix_ts;
  }
  guint64 diff = (guint64)(now - unix_ts);
  if (diff < 60) {
    return g_strdup_printf("%" G_GUINT64_FORMAT "s ago", diff);
  }
  if (diff < 3600) {
    return g_strdup_printf("%" G_GUINT64_FORMAT "m ago", diff / 60);
  }
  if (diff < 86400) {
    return g_strdup_printf("%" G_GUINT64_FORMAT "h ago", diff / 3600);
  }
  if (diff < 31536000) {
    return g_strdup_printf("%" G_GUINT64_FORMAT "d ago", diff / 86400);
  }
  return g_strdup_printf("%" G_GUINT64_FORMAT "y ago", diff / 31536000);
}

static char *
get_server_version(PumpkinWindow *self, PumpkinServer *server)
{
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  if (!g_file_test(bin, G_FILE_TEST_EXISTS)) {
    return g_strdup("Not installed");
  }

  const char *stored_label = pumpkin_server_get_installed_build_label(server);
  if (stored_label != NULL && *stored_label != '\0') {
    return normalize_build_label(self, stored_label);
  }

  g_autofree char *mtime_label = build_label_from_binary_path(self, bin);
  if (mtime_label != NULL) {
    return g_strdup(mtime_label);
  }

  const char *stored_id = pumpkin_server_get_installed_build_id(server);
  if (stored_id != NULL && *stored_id != '\0') {
    g_autofree char *raw = g_strdup_printf("Build %s", stored_id);
    return normalize_build_label(self, raw);
  }

  return g_strdup("Installed");
}

static char *
build_label_from_binary_path(PumpkinWindow *self, const char *bin_path)
{
  if (bin_path == NULL) {
    return NULL;
  }

  GStatBuf st;
  if (g_stat(bin_path, &st) != 0) {
    return NULL;
  }

  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(st.st_mtime);
  if (dt == NULL) {
    return NULL;
  }

  g_autofree char *formatted = g_date_time_format(dt, date_time_pattern_for_config(self));
  return g_strdup_printf("Build %s", formatted != NULL ? formatted : "Unknown");
}

static gboolean
is_update_available_for_server(PumpkinWindow *self, PumpkinServer *server, gboolean installed)
{
  if (!installed || self->latest_url == NULL || server == NULL) {
    return FALSE;
  }

  const char *installed_build_id = pumpkin_server_get_installed_build_id(server);
  if (self->latest_build_id != NULL && *self->latest_build_id != '\0') {
    return (installed_build_id == NULL || g_strcmp0(self->latest_build_id, installed_build_id) != 0);
  }

  const char *installed_url = pumpkin_server_get_installed_url(server);
  return (installed_url == NULL || g_strcmp0(self->latest_url, installed_url) != 0);
}

static void
update_check_updates_badge(PumpkinWindow *self)
{
  if (self == NULL || self->btn_details_check_updates == NULL) {
    return;
  }

  GtkWidget *button = GTK_WIDGET(self->btn_details_check_updates);
  gtk_widget_remove_css_class(button, "update-badge-current");
  gtk_widget_remove_css_class(button, "update-badge-available");

  gboolean update_available = FALSE;
  if (self->current != NULL) {
    g_autofree char *bin = pumpkin_server_get_bin_path(self->current);
    gboolean installed = g_file_test(bin, G_FILE_TEST_EXISTS);
    update_available = is_update_available_for_server(self, self->current, installed);
  }

  if (update_available) {
    gtk_button_set_label(self->btn_details_check_updates, "Update available");
    gtk_widget_add_css_class(button, "update-badge-available");
  } else {
    gtk_button_set_label(self->btn_details_check_updates, "Up to date");
    gtk_widget_add_css_class(button, "update-badge-current");
  }
}

static int
local_day_key(void)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  if (now == NULL) {
    return 0;
  }
  return g_date_time_get_year(now) * 10000 +
         g_date_time_get_month(now) * 100 +
         g_date_time_get_day_of_month(now);
}

static const char *
latest_build_identity(PumpkinWindow *self)
{
  if (self == NULL) {
    return NULL;
  }
  if (self->latest_build_id != NULL && *self->latest_build_id != '\0') {
    return self->latest_build_id;
  }
  return self->latest_url;
}

static void
send_server_chat(PumpkinServer *server, const char *message)
{
  if (server == NULL || message == NULL || *message == '\0') {
    return;
  }
  g_autofree char *cmd = g_strdup_printf("say %s", message);
  pumpkin_server_send_command(server, cmd, NULL);
}

static void
clear_auto_update_countdown(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  if (self->auto_update_countdown_id != 0) {
    g_source_remove(self->auto_update_countdown_id);
    self->auto_update_countdown_id = 0;
  }
  if (self->auto_update_server != NULL) {
    g_object_unref(self->auto_update_server);
    self->auto_update_server = NULL;
  }
  self->auto_update_countdown_remaining = 0;
}

static void
start_auto_update_countdown(PumpkinWindow *self, PumpkinServer *server)
{
  if (self == NULL || server == NULL || self->latest_url == NULL) {
    return;
  }
  if (self->auto_update_countdown_id != 0 || self->auto_update_server != NULL) {
    return;
  }
  if (!pumpkin_server_get_running(server)) {
    return;
  }
  self->auto_update_server = g_object_ref(server);
  self->auto_update_countdown_remaining = 10;
  send_server_chat(server,
                   "Der Server wird in 10 Sekunden neu gestartet, um auf die neueste Version zu aktualisieren.");
  set_details_status_for_server(self, server, "Auto-update countdown started (10s)", 4);
  self->auto_update_countdown_id = g_timeout_add_seconds(1, auto_update_countdown_tick, self);
}

static gboolean
auto_update_countdown_tick(gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  if (self == NULL || self->auto_update_server == NULL) {
    clear_auto_update_countdown(self);
    return G_SOURCE_REMOVE;
  }
  PumpkinServer *server = self->auto_update_server;
  if (!pumpkin_server_get_running(server)) {
    clear_auto_update_countdown(self);
    return G_SOURCE_REMOVE;
  }

  self->auto_update_countdown_remaining--;
  if (self->auto_update_countdown_remaining == 3 ||
      self->auto_update_countdown_remaining == 2 ||
      self->auto_update_countdown_remaining == 1) {
    g_autofree char *msg = g_strdup_printf("%d...", self->auto_update_countdown_remaining);
    send_server_chat(server, msg);
  }

  if (self->auto_update_countdown_remaining <= 0) {
    g_object_ref(server);
    clear_auto_update_countdown(self);

    if (self->latest_url != NULL) {
      g_autofree char *bin = pumpkin_server_get_bin_path(server);
      gboolean installed = g_file_test(bin, G_FILE_TEST_EXISTS);
      if (is_update_available_for_server(self, server, installed)) {
        set_details_status_for_server(self, server, "Updating to latest build...", 4);
        start_download_for_server(self, server, self->latest_url, TRUE, TRUE);
      }
    }
    g_object_unref(server);
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

static void
maybe_trigger_auto_update(PumpkinWindow *self)
{
  if (self == NULL || self->latest_url == NULL || self->store == NULL) {
    return;
  }
  if (self->auto_update_countdown_id != 0 || self->auto_update_server != NULL) {
    return;
  }

  GListModel *model = pumpkin_server_store_get_model(self->store);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(model, i);
    if (server == NULL) {
      continue;
    }
    if (!pumpkin_server_get_running(server)) {
      g_object_unref(server);
      continue;
    }
    if (server == self->current && self->ui_state != UI_STATE_RUNNING) {
      g_object_unref(server);
      continue;
    }
    if (!pumpkin_server_get_auto_update_enabled(server)) {
      g_object_unref(server);
      continue;
    }

    g_autofree char *bin = pumpkin_server_get_bin_path(server);
    gboolean installed = g_file_test(bin, G_FILE_TEST_EXISTS);
    if (!is_update_available_for_server(self, server, installed)) {
      g_object_unref(server);
      continue;
    }

    gint64 now_mono = g_get_monotonic_time();
    if (self->auto_update_cooldown_until > now_mono) {
      g_object_unref(server);
      continue;
    }

    const char *server_id = pumpkin_server_get_id(server);
    const char *build_identity = latest_build_identity(self);
    if (build_identity == NULL || server_id == NULL) {
      g_object_unref(server);
      continue;
    }

    gboolean use_schedule = pumpkin_server_get_auto_update_use_schedule(server);
    if (use_schedule) {
      g_autoptr(GDateTime) now = g_date_time_new_now_local();
      if (now == NULL) {
        g_object_unref(server);
        continue;
      }
      int target_hour = pumpkin_server_get_auto_update_hour(server);
      int target_minute = pumpkin_server_get_auto_update_minute(server);
      if (g_date_time_get_hour(now) != target_hour || g_date_time_get_minute(now) != target_minute) {
        g_object_unref(server);
        continue;
      }

      int day_key = local_day_key();
      if (day_key != 0 &&
          self->auto_update_last_schedule_day == day_key &&
          g_strcmp0(self->auto_update_last_schedule_server_id, server_id) == 0 &&
          g_strcmp0(self->auto_update_last_attempt_build_id, build_identity) == 0) {
        g_object_unref(server);
        continue;
      }
      self->auto_update_last_schedule_day = day_key;
      g_clear_pointer(&self->auto_update_last_schedule_server_id, g_free);
      self->auto_update_last_schedule_server_id = g_strdup(server_id);
    }

    g_clear_pointer(&self->auto_update_last_attempt_server_id, g_free);
    self->auto_update_last_attempt_server_id = g_strdup(server_id);
    g_clear_pointer(&self->auto_update_last_attempt_build_id, g_free);
    self->auto_update_last_attempt_build_id = g_strdup(build_identity);
    self->auto_update_cooldown_until = now_mono + 300 * G_USEC_PER_SEC;
    start_auto_update_countdown(self, server);
    g_object_unref(server);
    break;
  }
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
get_overview_player_count(PumpkinWindow *self, PumpkinServer *server)
{
  if (self == NULL || server == NULL || !pumpkin_server_get_running(server)) {
    return 0;
  }

  if (server == self->current && query_is_fresh(self)) {
    return self->query_players;
  }

  if (server == self->current) {
    return player_online_count(self);
  }

  return 0;
}

static int
preferred_max_players(PumpkinWindow *self, PumpkinServer *server)
{
  (void)self;
  if (server == NULL) {
    return 0;
  }

  int configured = pumpkin_server_get_max_players(server);
  if (configured <= 0) {
    configured = 20;
  }
  return configured;
}

static void
slp_varint_append(GByteArray *array, guint32 value)
{
  do {
    guint8 temp = (guint8)(value & 0x7F);
    value >>= 7;
    if (value != 0) {
      temp |= 0x80;
    }
    g_byte_array_append(array, &temp, 1);
  } while (value != 0);
}

static gboolean
slp_read_exact(GInputStream *stream, guint8 *buffer, gsize length)
{
  if (stream == NULL || buffer == NULL || length == 0) {
    return FALSE;
  }
  gsize bytes_read = 0;
  return g_input_stream_read_all(stream, buffer, length, &bytes_read, NULL, NULL) && bytes_read == length;
}

static gboolean
slp_read_varint_stream(GInputStream *stream, guint32 *out_value)
{
  if (stream == NULL || out_value == NULL) {
    return FALSE;
  }
  guint32 value = 0;
  int position = 0;
  while (position < 35) {
    guint8 byte = 0;
    if (!slp_read_exact(stream, &byte, 1)) {
      return FALSE;
    }
    value |= (guint32)(byte & 0x7F) << position;
    if ((byte & 0x80) == 0) {
      *out_value = value;
      return TRUE;
    }
    position += 7;
  }
  return FALSE;
}

static gboolean
slp_read_varint_buffer(const guint8 *data, gsize length, gsize *offset, guint32 *out_value)
{
  if (data == NULL || offset == NULL || out_value == NULL) {
    return FALSE;
  }
  guint32 value = 0;
  int position = 0;
  while (position < 35 && *offset < length) {
    guint8 byte = data[*offset];
    (*offset)++;
    value |= (guint32)(byte & 0x7F) << position;
    if ((byte & 0x80) == 0) {
      *out_value = value;
      return TRUE;
    }
    position += 7;
  }
  return FALSE;
}

static gboolean
parse_slp_players_json(const char *json, int *out_players, int *out_max_players)
{
  if (json == NULL || out_players == NULL || out_max_players == NULL) {
    return FALSE;
  }

  g_autoptr(GRegex) players_re = g_regex_new("\"players\"\\s*:\\s*\\{([^}]*)\\}",
                                              G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autoptr(GMatchInfo) players_match = NULL;
  if (!g_regex_match(players_re, json, 0, &players_match)) {
    return FALSE;
  }

  g_autofree char *players_blob = g_match_info_fetch(players_match, 1);
  if (players_blob == NULL) {
    return FALSE;
  }

  g_autoptr(GRegex) online_re = g_regex_new("\"online\"\\s*:\\s*([0-9]+)", G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GRegex) max_re = g_regex_new("\"max\"\\s*:\\s*([0-9]+)", G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) online_match = NULL;
  g_autoptr(GMatchInfo) max_match = NULL;
  if (!g_regex_match(online_re, players_blob, 0, &online_match)) {
    return FALSE;
  }
  if (!g_regex_match(max_re, players_blob, 0, &max_match)) {
    return FALSE;
  }

  g_autofree char *online_txt = g_match_info_fetch(online_match, 1);
  g_autofree char *max_txt = g_match_info_fetch(max_match, 1);
  if (online_txt == NULL || max_txt == NULL) {
    return FALSE;
  }

  *out_players = (int)strtol(online_txt, NULL, 10);
  *out_max_players = (int)strtol(max_txt, NULL, 10);
  if (*out_players < 0) {
    *out_players = 0;
  }
  if (*out_max_players < 0) {
    *out_max_players = 0;
  }
  return TRUE;
}

static gboolean
query_minecraft_players(const char *host, int port, int *out_players, int *out_max_players)
{
  if (host == NULL || port <= 0 || out_players == NULL || out_max_players == NULL) {
    return FALSE;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_socket_client_set_timeout(client, 1);
  g_autofree char *target = g_strdup(host);
  if (target == NULL || *target == '\0') {
    target = g_strdup("127.0.0.1");
  }

  g_autoptr(GSocketConnection) connection = g_socket_client_connect_to_host(client, target, port, NULL, &error);
  if (connection == NULL) {
    return FALSE;
  }
  GSocket *socket = g_socket_connection_get_socket(connection);
  if (socket != NULL) {
    g_socket_set_timeout(socket, 1);
  }

  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  if (output == NULL || input == NULL) {
    return FALSE;
  }

  g_autoptr(GByteArray) handshake_payload = g_byte_array_new();
  slp_varint_append(handshake_payload, 0x00);
  slp_varint_append(handshake_payload, 0);
  slp_varint_append(handshake_payload, (guint32)strlen(target));
  g_byte_array_append(handshake_payload, (const guint8 *)target, strlen(target));
  guint8 port_bytes[2] = {
    (guint8)((port >> 8) & 0xFF),
    (guint8)(port & 0xFF)
  };
  g_byte_array_append(handshake_payload, port_bytes, 2);
  slp_varint_append(handshake_payload, 1);

  g_autoptr(GByteArray) handshake_packet = g_byte_array_new();
  slp_varint_append(handshake_packet, handshake_payload->len);
  g_byte_array_append(handshake_packet, handshake_payload->data, handshake_payload->len);

  gsize written = 0;
  if (!g_output_stream_write_all(output, handshake_packet->data, handshake_packet->len, &written, NULL, NULL) ||
      written != handshake_packet->len) {
    return FALSE;
  }

  static const guint8 status_request[] = {0x01, 0x00};
  written = 0;
  if (!g_output_stream_write_all(output, status_request, sizeof(status_request), &written, NULL, NULL) ||
      written != sizeof(status_request)) {
    return FALSE;
  }
  if (!g_output_stream_flush(output, NULL, NULL)) {
    return FALSE;
  }

  guint32 packet_len = 0;
  if (!slp_read_varint_stream(input, &packet_len) || packet_len == 0 || packet_len > 32768) {
    return FALSE;
  }

  g_autofree guint8 *packet_data = g_malloc0(packet_len);
  if (!slp_read_exact(input, packet_data, packet_len)) {
    return FALSE;
  }

  gsize offset = 0;
  guint32 packet_id = 0;
  if (!slp_read_varint_buffer(packet_data, packet_len, &offset, &packet_id) || packet_id != 0x00) {
    return FALSE;
  }

  guint32 json_len = 0;
  if (!slp_read_varint_buffer(packet_data, packet_len, &offset, &json_len) || json_len == 0) {
    return FALSE;
  }

  if (offset + json_len > packet_len) {
    return FALSE;
  }

  g_autofree char *json = g_strndup((const char *)(packet_data + offset), (gsize)json_len);
  return parse_slp_players_json(json, out_players, out_max_players);
}

static gboolean
is_player_list_snapshot_line(const char *line)
{
  if (line == NULL) {
    return FALSE;
  }
  g_autofree char *lower = g_ascii_strdown(line, -1);
  return strstr(lower, "players online") != NULL || strstr(lower, "of a max of") != NULL;
}

static gboolean
parse_player_list_snapshot_line(const char *line, int *out_count, char **out_names_csv)
{
  if (line == NULL) {
    return FALSE;
  }
  if (out_count != NULL) {
    *out_count = -1;
  }
  if (out_names_csv != NULL) {
    *out_names_csv = NULL;
  }

  g_autoptr(GRegex) list_re =
    g_regex_new("there\\s+are\\s+([0-9]+)\\s+of\\s+a\\s+max\\s+of\\s+[0-9]+\\s+players\\s+online\\s*:?\\s*(.*)$",
                G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(list_re, line, 0, &match)) {
    return FALSE;
  }

  g_autofree char *count_txt = g_match_info_fetch(match, 1);
  g_autofree char *names_txt = g_match_info_fetch(match, 2);
  if (count_txt != NULL && out_count != NULL) {
    *out_count = (int)strtol(count_txt, NULL, 10);
    if (*out_count < 0) {
      *out_count = 0;
    }
  }
  if (out_names_csv != NULL && names_txt != NULL) {
    g_strstrip(names_txt);
    if (names_txt[0] != '\0') {
      *out_names_csv = g_strdup(names_txt);
    }
  }
  return TRUE;
}

static char *
extract_name_before_suffix(const char *line, const char *suffix)
{
  if (line == NULL || suffix == NULL || *suffix == '\0') {
    return NULL;
  }
  const char *pos = strstr(line, suffix);
  if (pos == NULL || pos <= line) {
    return NULL;
  }
  g_autofree char *prefix = g_strndup(line, (gsize)(pos - line));
  g_strstrip(prefix);
  if (prefix[0] == '\0') {
    return NULL;
  }

  const char *start = prefix;
  const char *colon = strrchr(prefix, ':');
  if (colon != NULL) {
    start = colon + 1;
  }
  while (*start == ' ' || *start == '\t') {
    start++;
  }
  if (*start == '\0') {
    return NULL;
  }

  g_autofree char *name = g_strdup(start);
  g_strstrip(name);
  if (name[0] == '\0') {
    return NULL;
  }
  return g_strdup(name);
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
  char *ip;
  char *reason;
  char *created;
  char *source;
  char *expires;
  int op_level;
  gboolean bypasses_player_limit;
} PlayerEntry;

static void
player_entry_free(PlayerEntry *entry)
{
  if (entry == NULL) {
    return;
  }
  g_clear_pointer(&entry->name, g_free);
  g_clear_pointer(&entry->uuid, g_free);
  g_clear_pointer(&entry->ip, g_free);
  g_clear_pointer(&entry->reason, g_free);
  g_clear_pointer(&entry->created, g_free);
  g_clear_pointer(&entry->source, g_free);
  g_clear_pointer(&entry->expires, g_free);
  g_free(entry);
}

static char *
extract_json_string_field(const char *object_text, const char *field)
{
  if (object_text == NULL || field == NULL || *field == '\0') {
    return NULL;
  }
  g_autofree char *pattern = g_strdup_printf("\"%s\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"", field);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  if (regex == NULL) {
    return NULL;
  }
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(regex, object_text, 0, &match) || !g_match_info_matches(match)) {
    return NULL;
  }
  return g_match_info_fetch(match, 1);
}

static int
extract_json_int_field(const char *object_text, const char *field, int default_value)
{
  if (object_text == NULL || field == NULL || *field == '\0') {
    return default_value;
  }
  g_autofree char *pattern = g_strdup_printf("\"%s\"\\s*:\\s*(-?[0-9]+)", field);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  if (regex == NULL) {
    return default_value;
  }
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(regex, object_text, 0, &match) || !g_match_info_matches(match)) {
    return default_value;
  }
  g_autofree char *value_txt = g_match_info_fetch(match, 1);
  if (value_txt == NULL || *value_txt == '\0') {
    return default_value;
  }
  return (int)strtol(value_txt, NULL, 10);
}

static gboolean
extract_json_bool_field(const char *object_text, const char *field, gboolean default_value)
{
  if (object_text == NULL || field == NULL || *field == '\0') {
    return default_value;
  }
  g_autofree char *pattern = g_strdup_printf("\"%s\"\\s*:\\s*(true|false)", field);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  if (regex == NULL) {
    return default_value;
  }
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(regex, object_text, 0, &match) || !g_match_info_matches(match)) {
    return default_value;
  }
  g_autofree char *value_txt = g_match_info_fetch(match, 1);
  if (value_txt == NULL) {
    return default_value;
  }
  return g_ascii_strcasecmp(value_txt, "true") == 0;
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
  g_autoptr(GRegex) regex = g_regex_new("\\{[^\\{\\}]*\\}", G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, contents, 0, &match)) {
    return g_steal_pointer(&entries);
  }

  while (g_match_info_matches(match)) {
    g_autofree char *object_text = g_match_info_fetch(match, 0);
    g_autofree char *name = extract_json_string_field(object_text, "name");
    g_autofree char *uuid = extract_json_string_field(object_text, "uuid");
    g_autofree char *ip = extract_json_string_field(object_text, "ip");
    g_autofree char *reason = extract_json_string_field(object_text, "reason");
    g_autofree char *created = extract_json_string_field(object_text, "created");
    g_autofree char *source = extract_json_string_field(object_text, "source");
    g_autofree char *expires = extract_json_string_field(object_text, "expires");
    int op_level = extract_json_int_field(object_text, "level", -1);
    gboolean bypasses_limit = extract_json_bool_field(object_text, "bypasses_player_limit", FALSE);
    if (!bypasses_limit) {
      bypasses_limit = extract_json_bool_field(object_text, "bypassesPlayerLimit", FALSE);
    }

    if ((name != NULL && *name != '\0') || (ip != NULL && *ip != '\0')) {
      PlayerEntry *entry = g_new0(PlayerEntry, 1);
      entry->name = (name != NULL && *name != '\0') ? g_strdup(name) : NULL;
      entry->uuid = (uuid != NULL && *uuid != '\0') ? g_strdup(uuid) : NULL;
      entry->ip = (ip != NULL && *ip != '\0') ? g_strdup(ip) : NULL;
      entry->reason = (reason != NULL && *reason != '\0') ? g_strdup(reason) : NULL;
      entry->created = (created != NULL && *created != '\0') ? g_strdup(created) : NULL;
      entry->source = (source != NULL && *source != '\0') ? g_strdup(source) : NULL;
      entry->expires = (expires != NULL && *expires != '\0') ? g_strdup(expires) : NULL;
      entry->op_level = op_level;
      entry->bypasses_player_limit = bypasses_limit;
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

static void
ingest_players_from_disk(PumpkinWindow *self)
{
  if (self == NULL || self->current == NULL || self->player_states == NULL) {
    return;
  }

  gboolean changed = FALSE;
  g_autoptr(GHashTable) name_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  load_player_name_map(name_map, self->current);

  GHashTableIter map_iter;
  gpointer map_key = NULL;
  gpointer map_value = NULL;
  g_hash_table_iter_init(&map_iter, name_map);
  while (g_hash_table_iter_next(&map_iter, &map_key, &map_value)) {
    const char *uuid = map_key;
    const char *name = map_value;
    ensure_player_state(self, uuid, name, TRUE);
  }

  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autofree char *players_dir = pumpkin_server_get_players_dir(self->current);
  g_autofree char *world_players = g_build_filename(pumpkin_server_get_data_dir(self->current),
                                                     "world", "playerdata", NULL);

  const char *dirs[] = {players_dir, world_players, NULL};
  for (int i = 0; dirs[i] != NULL; i++) {
    GDir *dir = g_dir_open(dirs[i], 0, NULL);
    if (dir == NULL) {
      continue;
    }
    const char *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(dirs[i], entry, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
        continue;
      }
      if (g_hash_table_contains(seen, entry)) {
        continue;
      }
      g_hash_table_add(seen, g_strdup(entry));
    }
    g_dir_close(dir);
  }

  GHashTableIter seen_iter;
  gpointer seen_key = NULL;
  g_hash_table_iter_init(&seen_iter, seen);
  while (g_hash_table_iter_next(&seen_iter, &seen_key, NULL)) {
    const char *entry = seen_key;
    g_autofree char *token = g_strdup(entry);
    char *dot = strrchr(token, '.');
    if (dot != NULL) {
      *dot = '\0';
    }

    const char *uuid = is_uuid_string(token) ? token : NULL;
    const char *name = token;
    if (uuid != NULL) {
      const char *mapped = g_hash_table_lookup(name_map, uuid);
      if (mapped != NULL && *mapped != '\0') {
        name = mapped;
      }
    }

    PlayerState *state = ensure_player_state(self, uuid, name, TRUE);
    if (state == NULL) {
      continue;
    }

    time_t mtime = player_last_seen_mtime(players_dir, world_players, entry, token);
    if (state->first_joined_unix <= 0 && mtime > 0) {
      state->first_joined_unix = (gint64)mtime;
      changed = TRUE;
    }
    if (!state->online && mtime > 0 && (gint64)mtime > state->last_online_unix) {
      state->last_online_unix = (gint64)mtime;
      changed = TRUE;
    }
  }

  if (changed) {
    player_states_set_dirty(self);
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

  start_download_for_server(self, server, self->latest_url, FALSE, FALSE);
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
    start_download_for_server(self, server, url, FALSE, FALSE);
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

    g_autofree char *version = get_server_version(self, server);
    g_autofree char *size = get_server_size(server);
    int players = get_overview_player_count(self, server);
    gboolean installed = g_strcmp0(version, "Not installed") != 0;
    gboolean update_available = is_update_available_for_server(self, server, installed);

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

    GtkWidget *btn_remove = gtk_button_new_with_label("Delete");
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
    update_check_updates_badge(self);
    return;
  }

  gboolean running = pumpkin_server_get_running(self->current);
  g_autofree char *bin = pumpkin_server_get_bin_path(self->current);
  gboolean installed = g_file_test(bin, G_FILE_TEST_EXISTS);
  gboolean update_available = is_update_available_for_server(self, self->current, installed);

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
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_details_check_updates), self->current != NULL);
  update_check_updates_badge(self);
  if (self->btn_console_copy != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_console_copy), self->current != NULL);
  }
  if (self->btn_console_send != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_console_send), self->current != NULL);
  }
  if (self->btn_console_clear != NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_console_clear), self->current != NULL);
  }

  if (running) {
    if (self->players_refresh_id == 0) {
      self->players_refresh_id = g_timeout_add((guint)current_stats_sample_msec(self),
                                               refresh_players_tick,
                                               self);
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
                  double max_value, int smoothing_window, double r, double g, double b,
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
  int window = smoothing_window > 1 ? smoothing_window : 1;
  int start = total_samples - valid_count;
  if (start < 0) {
    start = 0;
  }
  gboolean started = FALSE;
  for (int i = start; i < total_samples; i++) {
    int offset = i - start;
    double raw = window > 1
                   ? stats_get_smoothed(self, series, offset, window)
                   : stats_get_sample(self, series, offset);
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
  g_autofree char *clean = strip_ansi(line);
  const char *check = clean != NULL ? clean : line;

  static GRegex *primary = NULL;
  static GRegex *fallback = NULL;
  if (primary == NULL) {
    primary = g_regex_new("TPS\\s*:\\s*([0-9]+(\\.[0-9]+)?)", G_REGEX_CASELESS, 0, NULL);
  }
  if (fallback == NULL) {
    fallback = g_regex_new("tps[^0-9]*([0-9]+(\\.[0-9]+)?)", G_REGEX_CASELESS, 0, NULL);
  }
  if (primary == NULL || fallback == NULL) {
    return FALSE;
  }

  g_autoptr(GMatchInfo) match_info = NULL;
  if (!g_regex_match(primary, check, 0, &match_info) || !g_match_info_matches(match_info)) {
    g_clear_pointer(&match_info, g_match_info_free);
    if (!g_regex_match(fallback, check, 0, &match_info) || !g_match_info_matches(match_info)) {
      return FALSE;
    }
  }

  g_autofree char *num = g_match_info_fetch(match_info, 1);
  if (num == NULL || *num == '\0') {
    return FALSE;
  }

  char *endptr = NULL;
  double value = g_ascii_strtod(num, &endptr);
  if (endptr == num) {
    return FALSE;
  }
  *out = value;
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
draw_time_axis_labels(PumpkinWindow *self, cairo_t *cr, double left, double top, double right, double bottom,
                      int width, int height)
{
  double graph_h = height - top - bottom;
  double y = top + graph_h + 14.0;
  double graph_w = width - left - right;
  double history_seconds = (double)STATS_SAMPLES * (double)current_stats_sample_msec(self) / 1000.0;
  const int segments = 6;
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11.0);

  for (int i = 0; i <= segments; i++) {
    double ratio = (double)i / (double)segments;
    double x = left + graph_w * ratio;
    int seconds = (int)llround(history_seconds * (1.0 - ratio));
    g_autofree char *text = NULL;
    if (seconds >= 60) {
      int minutes = seconds / 60;
      int rem = seconds % 60;
      if (rem == 0) {
        text = g_strdup_printf("%dm", minutes);
      } else {
        text = g_strdup_printf("%dm%02ds", minutes, rem);
      }
    } else {
      text = g_strdup_printf("%ds", seconds);
    }
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
  draw_time_axis_labels(self, cr, left, top, right, bottom, width, height);

  draw_stats_series(self, cr, self->stats_cpu, STATS_SAMPLES, self->stats_count, (double)scale,
                    5, 0.93, 0.33, 0.33, left, top, right, bottom, width, height);
  draw_stats_series(self, cr, self->stats_ram_mb, STATS_SAMPLES, self->stats_count, (double)scale,
                    5, 0.33, 0.55, 0.93, left, top, right, bottom, width, height);
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
    int max_players = preferred_max_players(self, self->current);
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
  draw_time_axis_labels(self, cr, left, top, right, bottom, width, height);

  draw_stats_series(self, cr, self->stats_players, STATS_SAMPLES, self->stats_count, players_max,
                    3, 0.95, 0.66, 0.26, left, top, right, bottom, width, height);
}

static void
stats_graph_draw_disk(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
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
  draw_stats_axis_labels(cr, left, top, right, bottom, width, height, 4, "20", "10", "0");
  draw_time_axis_labels(self, cr, left, top, right, bottom, width, height);

  draw_stats_series(self, cr, self->stats_disk_mb, STATS_SAMPLES, self->stats_count, 20.0,
                    1, 0.35, 0.77, 0.45, left, top, right, bottom, width, height);
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

static void
set_stats_graphs_disabled(PumpkinWindow *self, gboolean disabled)
{
  if (self == NULL) {
    return;
  }

  GtkWidget *graphs[3] = {
    self->stats_graph_usage != NULL ? GTK_WIDGET(self->stats_graph_usage) : NULL,
    self->stats_graph_players != NULL ? GTK_WIDGET(self->stats_graph_players) : NULL,
    self->stats_graph_disk != NULL ? GTK_WIDGET(self->stats_graph_disk) : NULL
  };

  for (guint i = 0; i < G_N_ELEMENTS(graphs); i++) {
    if (graphs[i] == NULL) {
      continue;
    }
    if (disabled) {
      gtk_widget_add_css_class(graphs[i], "stats-disabled");
    } else {
      gtk_widget_remove_css_class(graphs[i], "stats-disabled");
    }
  }
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
  gint64 now_mono = g_get_monotonic_time();
  gtk_widget_set_visible(GTK_WIDGET(self->label_srv_cpu), server_running);
  gtk_widget_set_visible(GTK_WIDGET(self->label_srv_ram), server_running);
  set_stats_graphs_disabled(self, !server_running);

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
    max_players = preferred_max_players(self, self->current);
    int tracked_online = player_online_count(self);
    if (pumpkin_server_get_running(self->current)) {
      self->tps_enabled = TRUE;
      if (!self->query_in_flight && !query_is_fresh(self)) {
        start_query_players(self, self->current);
      }
      if (now_mono - self->last_tps_request_at >= tps_query_interval_usec(self)) {
        if (pumpkin_server_send_command(self->current, "tps", NULL)) {
          if (self->pending_auto_tps_lines < 8) {
            self->pending_auto_tps_lines++;
          }
        }
        self->last_tps_request_at = now_mono;
      }
      if (now_mono - self->last_player_list_request_at >= player_list_query_interval_usec(self)) {
        if (pumpkin_server_send_command(self->current, "list", NULL)) {
          if (self->pending_auto_list_lines < 8) {
            self->pending_auto_list_lines++;
          }
        }
        self->last_player_list_request_at = now_mono;
      }
    } else {
      self->query_valid = FALSE;
      self->tps_enabled = FALSE;
      self->last_tps_valid = FALSE;
      self->pending_auto_tps_lines = 0;
      self->pending_auto_list_lines = 0;
      self->pending_java_platform_hints = 0;
      self->pending_bedrock_platform_hints = 0;
    }
    if (query_is_fresh(self)) {
      players_count = self->query_players;
    } else {
      players_count = tracked_online;
    }
    if (tracked_online > players_count) {
      players_count = tracked_online;
    }
  }

  if (!server_running) {
    if (self->current != NULL && player_online_count(self) > 0) {
      player_states_mark_all_offline(self);
    }
    players_count = 0;
  }

  if (self->current != NULL &&
      self->player_state_dirty &&
      (now_mono - self->last_player_state_flush_at) >= PLAYER_STATE_FLUSH_INTERVAL_USEC) {
    player_states_save(self, self->current);
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

  if (server_running) {
    if (self->last_tps_valid) {
      double tps_value = self->last_tps;
      if (tps_value < 0.0) {
        tps_value = 0.0;
      } else if (tps_value > 20.0) {
        tps_value = 20.0;
      }
      self->stats_disk_mb[self->stats_index] = tps_value;
    } else if (self->stats_count > 0) {
      int last_idx = self->stats_index - 1;
      if (last_idx < 0) {
        last_idx = STATS_SAMPLES - 1;
      }
      self->stats_disk_mb[self->stats_index] = self->stats_disk_mb[last_idx];
    } else {
      self->stats_disk_mb[self->stats_index] = 0.0;
    }

    self->stats_cpu[self->stats_index] = proc_cpu;
    self->stats_ram_mb[self->stats_index] = ram_pct;
    self->stats_players[self->stats_index] = (double)players_count;
    self->stats_index = (self->stats_index + 1) % STATS_SAMPLES;
    if (self->stats_count < STATS_SAMPLES) {
      self->stats_count++;
    }
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

  if (server_running) {
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
      if (self->last_tps_valid) {
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
  }

  if (now_mono - self->last_auto_update_eval_at >= G_USEC_PER_SEC) {
    self->last_auto_update_eval_at = now_mono;
    maybe_trigger_auto_update(self);
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
player_lookup_add_set(GHashTable *set, const char *name, const char *uuid)
{
  if (set == NULL) {
    return;
  }
  g_autofree char *name_key = normalized_key(name);
  g_autofree char *uuid_key = normalized_key(uuid);
  if (name_key != NULL && *name_key != '\0') {
    g_hash_table_add(set, g_strdup(name_key));
  }
  if (uuid_key != NULL && *uuid_key != '\0') {
    g_hash_table_add(set, g_strdup(uuid_key));
  }
}

static void
player_lookup_add_reason(GHashTable *map, const char *name, const char *uuid, const char *reason)
{
  if (map == NULL) {
    return;
  }
  g_autofree char *name_key = normalized_key(name);
  g_autofree char *uuid_key = normalized_key(uuid);
  const char *stored = (reason != NULL && *reason != '\0') ? reason : "";
  if (name_key != NULL && *name_key != '\0') {
    g_hash_table_replace(map, g_strdup(name_key), g_strdup(stored));
  }
  if (uuid_key != NULL && *uuid_key != '\0') {
    g_hash_table_replace(map, g_strdup(uuid_key), g_strdup(stored));
  }
}

static void
player_lookup_add_ip_reason(GHashTable *map, const char *ip, const char *reason)
{
  if (map == NULL) {
    return;
  }
  g_autofree char *ip_key = normalized_key(ip);
  if (ip_key == NULL || *ip_key == '\0') {
    return;
  }
  const char *stored = (reason != NULL && *reason != '\0') ? reason : "";
  g_hash_table_replace(map, g_strdup(ip_key), g_strdup(stored));
}

static void
player_lookup_add_op_level(GHashTable *map, const char *name, const char *uuid, int op_level)
{
  if (map == NULL) {
    return;
  }
  int stored_level = op_level >= 0 ? op_level : 0;
  gpointer value = GINT_TO_POINTER(stored_level + 1);
  g_autofree char *name_key = normalized_key(name);
  g_autofree char *uuid_key = normalized_key(uuid);
  if (name_key != NULL && *name_key != '\0') {
    g_hash_table_replace(map, g_strdup(name_key), value);
  }
  if (uuid_key != NULL && *uuid_key != '\0') {
    g_hash_table_replace(map, g_strdup(uuid_key), value);
  }
}

static gboolean
player_lookup_contains_state(GHashTable *set, const PlayerState *state)
{
  if (set == NULL || state == NULL) {
    return FALSE;
  }
  g_autofree char *uuid_key = normalized_key(state->uuid);
  if (uuid_key != NULL && g_hash_table_contains(set, uuid_key)) {
    return TRUE;
  }
  g_autofree char *name_key = normalized_key(state->name);
  if (name_key != NULL && g_hash_table_contains(set, name_key)) {
    return TRUE;
  }
  return FALSE;
}

static int
player_lookup_op_level_for_state(GHashTable *map, const PlayerState *state)
{
  if (map == NULL || state == NULL) {
    return -1;
  }
  g_autofree char *uuid_key = normalized_key(state->uuid);
  if (uuid_key != NULL) {
    gpointer raw = g_hash_table_lookup(map, uuid_key);
    if (raw != NULL) {
      return GPOINTER_TO_INT(raw) - 1;
    }
  }
  g_autofree char *name_key = normalized_key(state->name);
  if (name_key != NULL) {
    gpointer raw = g_hash_table_lookup(map, name_key);
    if (raw != NULL) {
      return GPOINTER_TO_INT(raw) - 1;
    }
  }
  return -1;
}

static gboolean
player_is_admin_from_op_level(int op_level)
{
  return op_level >= 4;
}

static const char *
player_lookup_reason_for_state(GHashTable *map, const PlayerState *state)
{
  if (map == NULL || state == NULL) {
    return NULL;
  }
  g_autofree char *uuid_key = normalized_key(state->uuid);
  if (uuid_key != NULL) {
    const char *reason = g_hash_table_lookup(map, uuid_key);
    if (reason != NULL) {
      return reason;
    }
  }
  g_autofree char *name_key = normalized_key(state->name);
  if (name_key != NULL) {
    return g_hash_table_lookup(map, name_key);
  }
  return NULL;
}

static const char *
player_lookup_reason_for_ip(GHashTable *map, const char *ip)
{
  if (map == NULL || ip == NULL || *ip == '\0') {
    return NULL;
  }
  g_autofree char *ip_key = normalized_key(ip);
  if (ip_key == NULL || *ip_key == '\0') {
    return NULL;
  }
  return g_hash_table_lookup(map, ip_key);
}

static void
append_player_state_row(PumpkinWindow *self,
                        GtkListBox *list,
                        PlayerState *state,
                        gboolean whitelisted,
                        gboolean banned,
                        const char *ban_reason,
                        gboolean ip_banned,
                        const char *ip_ban_reason,
                        int op_level,
                        gboolean interactive)
{
  if (self == NULL || list == NULL || state == NULL) {
    return;
  }

  const char *display_name = state->name != NULL && *state->name != '\0'
                               ? state->name
                               : (state->uuid != NULL ? state->uuid : "Unknown");

  g_autofree char *playtime = format_duration(player_state_effective_playtime(state));
  g_autofree char *last_seen = state->online
                                ? g_strdup("Online now")
                                : relative_time_label(state->last_online_unix);
  g_autofree char *first_joined = format_unix_time(self, state->first_joined_unix);

  g_autofree char *meta = g_strdup_printf("Played %s · Last seen %s · First joined %s",
                                          playtime, last_seen, first_joined);
  if (banned && ban_reason != NULL && *ban_reason != '\0') {
    g_autofree char *with_reason = g_strdup_printf("%s · Ban reason: %s", meta, ban_reason);
    g_free(meta);
    meta = g_strdup(with_reason);
  }
  if (ip_banned) {
    if (ip_ban_reason != NULL && *ip_ban_reason != '\0') {
      g_autofree char *with_ip_reason = g_strdup_printf("%s · IP ban reason: %s", meta, ip_ban_reason);
      g_free(meta);
      meta = g_strdup(with_ip_reason);
    } else {
      g_autofree char *with_ip = g_strdup_printf("%s · IP banned", meta);
      g_free(meta);
      meta = g_strdup(with_ip);
    }
  }
  gboolean is_op = op_level >= 0;
  gboolean is_admin = player_is_admin_from_op_level(op_level);

  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  gtk_widget_set_margin_start(box, 4);
  gtk_widget_set_margin_end(box, 4);

  GtkWidget *avatar = gtk_image_new_from_icon_name("avatar-default-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(avatar), 32);
  set_player_head_image(self, GTK_IMAGE(avatar), state->uuid);
  gtk_box_append(GTK_BOX(box), avatar);

  GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(text_box, TRUE);

  GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *name_label = gtk_label_new(display_name);
  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
  gtk_widget_set_hexpand(name_label, TRUE);
  if (!state->online) {
    gtk_widget_add_css_class(name_label, "dim-label");
  }
  gtk_box_append(GTK_BOX(title_row), name_label);

  GtkWidget *platform_chip = gtk_label_new(platform_label(state->platform));
  gtk_widget_add_css_class(platform_chip, "status-badge");
  gtk_widget_add_css_class(platform_chip, "player-platform-badge");
  gtk_box_append(GTK_BOX(title_row), platform_chip);

  GtkWidget *status_chip = gtk_label_new(state->online ? "Online" : "Offline");
  gtk_widget_add_css_class(status_chip, "status-badge");
  gtk_widget_add_css_class(status_chip, state->online ? "status-running" : "status-stopped");
  gtk_box_append(GTK_BOX(title_row), status_chip);

  if (whitelisted) {
    GtkWidget *whitelist_chip = gtk_label_new("Whitelisted");
    gtk_widget_add_css_class(whitelist_chip, "status-badge");
    gtk_widget_add_css_class(whitelist_chip, "status-running");
    gtk_box_append(GTK_BOX(title_row), whitelist_chip);
  }
  if (banned) {
    GtkWidget *ban_chip = gtk_label_new("Banned");
    gtk_widget_add_css_class(ban_chip, "status-badge");
    gtk_widget_add_css_class(ban_chip, "status-stopped");
    gtk_box_append(GTK_BOX(title_row), ban_chip);
  }
  if (ip_banned) {
    GtkWidget *ip_ban_chip = gtk_label_new("IP Banned");
    gtk_widget_add_css_class(ip_ban_chip, "status-badge");
    gtk_widget_add_css_class(ip_ban_chip, "status-stopped");
    gtk_box_append(GTK_BOX(title_row), ip_ban_chip);
  }
  if (is_admin) {
    GtkWidget *admin_chip = gtk_label_new("ADMIN");
    gtk_widget_add_css_class(admin_chip, "status-badge");
    gtk_widget_add_css_class(admin_chip, "warning-badge");
    gtk_box_append(GTK_BOX(title_row), admin_chip);
  } else if (is_op) {
    GtkWidget *op_chip = gtk_label_new("OP");
    gtk_widget_add_css_class(op_chip, "status-badge");
    gtk_widget_add_css_class(op_chip, "status-running");
    gtk_box_append(GTK_BOX(title_row), op_chip);
  }

  gtk_box_append(GTK_BOX(text_box), title_row);

  GtkWidget *meta_label = gtk_label_new(meta);
  gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0);
  gtk_widget_add_css_class(meta_label, "dim-label");
  if (!state->online) {
    gtk_widget_add_css_class(meta_label, "player-offline-meta");
  }
  gtk_box_append(GTK_BOX(text_box), meta_label);

  gtk_box_append(GTK_BOX(box), text_box);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_list_box_append(list, row);

  if (interactive) {
    g_object_set_data_full(G_OBJECT(row), "player-name", g_strdup(display_name), g_free);
    if (state->uuid != NULL) {
      g_object_set_data_full(G_OBJECT(row), "player-uuid", g_strdup(state->uuid), g_free);
    }
    if (state->key != NULL) {
      g_object_set_data_full(G_OBJECT(row), "player-key", g_strdup(state->key), g_free);
    }
    if (state->last_ip != NULL && *state->last_ip != '\0') {
      g_object_set_data_full(G_OBJECT(row), "player-last-ip", g_strdup(state->last_ip), g_free);
    }
    g_object_set_data(G_OBJECT(row), "player-online", GINT_TO_POINTER(state->online ? 1 : 0));
    g_object_set_data(G_OBJECT(row), "player-banned", GINT_TO_POINTER(banned ? 1 : 0));
    g_object_set_data(G_OBJECT(row), "player-ip-banned", GINT_TO_POINTER(ip_banned ? 1 : 0));
    g_object_set_data(G_OBJECT(row), "player-op-level", GINT_TO_POINTER(op_level + 1));
  }
}

static gint
player_state_sort_cmp(gconstpointer a, gconstpointer b, gpointer user_data)
{
  const PlayerState *left = *(const PlayerState * const *)a;
  const PlayerState *right = *(const PlayerState * const *)b;
  const PlayerSortSettings *settings = user_data;

  if (settings != NULL && settings->op_level_map != NULL) {
    gboolean left_admin = player_is_admin_from_op_level(
      player_lookup_op_level_for_state(settings->op_level_map, left));
    gboolean right_admin = player_is_admin_from_op_level(
      player_lookup_op_level_for_state(settings->op_level_map, right));
    if (left_admin != right_admin) {
      return left_admin ? -1 : 1;
    }
  }

  gint cmp = 0;

  if (settings != NULL && settings->field == 1) {
    guint64 lp = player_state_effective_playtime(left);
    guint64 rp = player_state_effective_playtime(right);
    cmp = lp < rp ? -1 : (lp > rp ? 1 : 0);
  } else if (settings != NULL && settings->field == 2) {
    cmp = left->first_joined_unix < right->first_joined_unix ? -1
         : (left->first_joined_unix > right->first_joined_unix ? 1 : 0);
  } else if (settings != NULL && settings->field == 3) {
    cmp = g_ascii_strcasecmp(left->name != NULL ? left->name : "",
                             right->name != NULL ? right->name : "");
  } else {
    gint64 ll = left->online ? (gint64)time(NULL) : left->last_online_unix;
    gint64 rl = right->online ? (gint64)time(NULL) : right->last_online_unix;
    cmp = ll < rl ? -1 : (ll > rl ? 1 : 0);
  }

  if (cmp == 0) {
    cmp = g_ascii_strcasecmp(left->name != NULL ? left->name : "",
                             right->name != NULL ? right->name : "");
  }
  if (settings != NULL && !settings->ascending) {
    cmp = -cmp;
  }
  return cmp;
}

static void
invalidate_player_list_signature(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  self->player_list_signature_valid = FALSE;
  self->player_list_signature = 0;
  self->player_list_signature_count = 0;
}

static guint64
player_list_signature_mix_u64(guint64 signature, guint64 value)
{
  signature ^= value + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
  return signature;
}

static guint64
player_list_signature_mix_str(guint64 signature, const char *text)
{
  if (text == NULL || *text == '\0') {
    return player_list_signature_mix_u64(signature, 0);
  }
  signature = player_list_signature_mix_u64(signature, (guint64)g_str_hash(text));
  signature = player_list_signature_mix_u64(signature, (guint64)strlen(text));
  return signature;
}

static void
refresh_player_list(PumpkinWindow *self)
{
  if (self == NULL || self->player_list == NULL) {
    return;
  }
  if (self->current == NULL) {
    clear_list_box(self->player_list);
    invalidate_player_list_signature(self);
    return;
  }

  ingest_players_from_disk(self);

  g_autofree char *whitelist_path = resolve_data_file(self->current, "whitelist.json");
  g_autoptr(GPtrArray) whitelist_entries = load_player_entries_from_file(whitelist_path);
  g_autoptr(GHashTable) whitelist_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (whitelist_entries != NULL) {
    for (guint i = 0; i < whitelist_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(whitelist_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_set(whitelist_set, entry->name, entry->uuid);
    }
  }

  g_autofree char *banned_path = resolve_data_file(self->current, "banned-players.json");
  g_autoptr(GPtrArray) banned_entries = load_player_entries_from_file(banned_path);
  g_autoptr(GHashTable) banned_reason_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (banned_entries != NULL) {
    for (guint i = 0; i < banned_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(banned_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_reason(banned_reason_map, entry->name, entry->uuid, entry->reason);
    }
  }

  g_autofree char *banned_ips_path = resolve_data_file(self->current, "banned-ips.json");
  g_autoptr(GPtrArray) banned_ip_entries = load_player_entries_from_file(banned_ips_path);
  g_autoptr(GHashTable) banned_ip_reason_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (banned_ip_entries != NULL) {
    for (guint i = 0; i < banned_ip_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(banned_ip_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_ip_reason(banned_ip_reason_map, entry->ip, entry->reason);
    }
  }

  g_autofree char *ops_path = resolve_data_file(self->current, "ops.json");
  g_autoptr(GPtrArray) ops_entries = load_player_entries_from_file(ops_path);
  g_autoptr(GHashTable) op_level_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (ops_entries != NULL) {
    for (guint i = 0; i < ops_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(ops_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_op_level(op_level_map, entry->name, entry->uuid, entry->op_level);
    }
  }

  const char *query = self->player_search != NULL
                        ? gtk_editable_get_text(GTK_EDITABLE(self->player_search))
                        : NULL;
  g_autofree char *query_key = normalized_key(query);

  PlayerSortSettings sort = {0};
  sort.field = self->player_sort_field;
  sort.ascending = self->player_sort_ascending;
  sort.op_level_map = op_level_map;

  g_autoptr(GPtrArray) states = g_ptr_array_new();
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->player_states);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    PlayerState *state = value;
    if (state == NULL) {
      continue;
    }
    if (query_key != NULL && *query_key != '\0') {
      g_autofree char *name_key = normalized_key(state->name);
      g_autofree char *uuid_key = normalized_key(state->uuid);
      g_autofree char *ip_key = normalized_key(state->last_ip);
      const char *ban_reason = player_lookup_reason_for_state(banned_reason_map, state);
      const char *ip_ban_reason = player_lookup_reason_for_ip(banned_ip_reason_map, state->last_ip);
      g_autofree char *ban_reason_key = normalized_key(ban_reason);
      g_autofree char *ip_ban_reason_key = normalized_key(ip_ban_reason);
      gboolean match = FALSE;
      if (name_key != NULL && strstr(name_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && uuid_key != NULL && strstr(uuid_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && ip_key != NULL && strstr(ip_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && ban_reason_key != NULL && strstr(ban_reason_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && ip_ban_reason_key != NULL && strstr(ip_ban_reason_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match) {
        continue;
      }
    }
    g_ptr_array_add(states, state);
  }

  g_ptr_array_sort_with_data(states, player_state_sort_cmp, &sort);

  guint64 signature = 0xcbf29ce484222325ULL;
  signature = player_list_signature_mix_u64(signature, (guint64)sort.field);
  signature = player_list_signature_mix_u64(signature, sort.ascending ? 1 : 0);
  signature = player_list_signature_mix_str(signature, query_key);
  for (guint i = 0; i < states->len; i++) {
    PlayerState *state = g_ptr_array_index(states, i);
    if (state == NULL) {
      continue;
    }
    gboolean whitelisted = player_lookup_contains_state(whitelist_set, state);
    const char *ban_reason = player_lookup_reason_for_state(banned_reason_map, state);
    gboolean banned = (ban_reason != NULL);
    const char *ip_ban_reason = player_lookup_reason_for_ip(banned_ip_reason_map, state->last_ip);
    gboolean ip_banned = (ip_ban_reason != NULL);
    int op_level = player_lookup_op_level_for_state(op_level_map, state);
    signature = player_list_signature_mix_str(signature, state->key);
    signature = player_list_signature_mix_str(signature, state->name);
    signature = player_list_signature_mix_str(signature, state->uuid);
    signature = player_list_signature_mix_str(signature, state->last_ip);
    signature = player_list_signature_mix_u64(signature, state->online ? 1 : 0);
    signature = player_list_signature_mix_u64(signature, (guint64)state->platform);
    signature = player_list_signature_mix_u64(signature, (guint64)state->first_joined_unix);
    signature = player_list_signature_mix_u64(signature, whitelisted ? 1 : 0);
    signature = player_list_signature_mix_u64(signature, banned ? 1 : 0);
    signature = player_list_signature_mix_u64(signature, ip_banned ? 1 : 0);
    signature = player_list_signature_mix_u64(signature, (guint64)(op_level + 1));
    signature = player_list_signature_mix_u64(signature, player_is_admin_from_op_level(op_level) ? 1 : 0);
    signature = player_list_signature_mix_str(signature, ban_reason);
    signature = player_list_signature_mix_str(signature, ip_ban_reason);
  }

  if (self->player_list_signature_valid &&
      self->player_list_signature == signature &&
      self->player_list_signature_count == states->len) {
    return;
  }
  self->player_list_signature_valid = TRUE;
  self->player_list_signature = signature;
  self->player_list_signature_count = states->len;

  clear_list_box(self->player_list);

  for (guint i = 0; i < states->len; i++) {
    PlayerState *state = g_ptr_array_index(states, i);
    if (state == NULL) {
      continue;
    }
    gboolean whitelisted = player_lookup_contains_state(whitelist_set, state);
    const char *ban_reason = player_lookup_reason_for_state(banned_reason_map, state);
    gboolean banned = (ban_reason != NULL);
    const char *ip_ban_reason = player_lookup_reason_for_ip(banned_ip_reason_map, state->last_ip);
    gboolean ip_banned = (ip_ban_reason != NULL);
    int op_level = player_lookup_op_level_for_state(op_level_map, state);
    append_player_state_row(self, self->player_list, state, whitelisted, banned, ban_reason,
                            ip_banned, ip_ban_reason, op_level, TRUE);
  }

  refresh_whitelist_list(self);
  refresh_banned_list(self);
}

static void
player_head_download_context_free(PlayerHeadDownloadContext *ctx)
{
  if (ctx == NULL) {
    return;
  }
  g_clear_object(&ctx->self);
  g_clear_object(&ctx->server);
  g_clear_pointer(&ctx->uuid_key, g_free);
  g_clear_pointer(&ctx->cache_path, g_free);
  g_free(ctx);
}

static void
on_player_head_download_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  PlayerHeadDownloadContext *ctx = user_data;
  if (ctx == NULL) {
    return;
  }

  pumpkin_download_file_finish(res, NULL);
  if (ctx->self->player_head_downloads != NULL && ctx->uuid_key != NULL) {
    g_hash_table_remove(ctx->self->player_head_downloads, ctx->uuid_key);
  }
  if (ctx->self->current != NULL && ctx->self->current == ctx->server) {
    invalidate_player_list_signature(ctx->self);
    refresh_player_list(ctx->self);
  }
  player_head_download_context_free(ctx);
}

static void
set_player_head_image(PumpkinWindow *self, GtkImage *image, const char *uuid)
{
  if (self == NULL || image == NULL || self->current == NULL || uuid == NULL || *uuid == '\0') {
    gtk_image_set_from_icon_name(image, "avatar-default-symbolic");
    return;
  }

  g_autofree char *uuid_key = normalized_key(uuid);
  if (uuid_key == NULL) {
    gtk_image_set_from_icon_name(image, "avatar-default-symbolic");
    return;
  }

  g_autofree char *data_dir = pumpkin_server_get_data_dir(self->current);
  g_autofree char *cache_dir = g_build_filename(data_dir, "cache", "player-heads", NULL);
  g_mkdir_with_parents(cache_dir, 0755);
  g_autofree char *cache_path = g_strdup_printf("%s/%s.png", cache_dir, uuid_key);

  if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = g_file_new_for_path(cache_path);
    g_autoptr(GdkTexture) texture = gdk_texture_new_from_file(file, &error);
    if (texture != NULL) {
      gtk_image_set_from_paintable(image, GDK_PAINTABLE(texture));
      gtk_image_set_pixel_size(image, 32);
      return;
    }
  }

  gtk_image_set_from_icon_name(image, "avatar-default-symbolic");
  gtk_image_set_pixel_size(image, 32);

  if (self->player_head_downloads != NULL &&
      g_hash_table_contains(self->player_head_downloads, uuid_key)) {
    return;
  }
  if (self->player_head_downloads != NULL) {
    g_hash_table_add(self->player_head_downloads, g_strdup(uuid_key));
  }

  g_autofree char *uuid_compact = g_strdup(uuid_key);
  char *dst = uuid_compact;
  for (char *src = uuid_compact; *src != '\0'; src++) {
    if (*src != '-') {
      *dst++ = *src;
    }
  }
  *dst = '\0';

  PlayerHeadDownloadContext *ctx = g_new0(PlayerHeadDownloadContext, 1);
  ctx->self = g_object_ref(self);
  ctx->server = g_object_ref(self->current);
  ctx->uuid_key = g_strdup(uuid_key);
  ctx->cache_path = g_strdup(cache_path);

  g_autofree char *url = g_strdup_printf("https://crafatar.com/avatars/%s?size=64&overlay=true",
                                         uuid_compact);
  pumpkin_download_file_async(url, cache_path, NULL, NULL, NULL, on_player_head_download_done, ctx);
}

static void
refresh_whitelist_list(PumpkinWindow *self)
{
  clear_list_box(self->whitelist_list);
  if (self->current == NULL) {
    return;
  }
  ingest_players_from_disk(self);
  const char *query = self->whitelist_search != NULL
                        ? gtk_editable_get_text(GTK_EDITABLE(self->whitelist_search))
                        : NULL;
  g_autofree char *query_key = normalized_key(query);

  g_autofree char *whitelist_path = resolve_data_file(self->current, "whitelist.json");
  g_autoptr(GPtrArray) whitelist_entries = load_player_entries_from_file(whitelist_path);
  if (whitelist_entries == NULL) {
    return;
  }

  g_autofree char *banned_path = resolve_data_file(self->current, "banned-players.json");
  g_autoptr(GPtrArray) banned_entries = load_player_entries_from_file(banned_path);
  g_autoptr(GHashTable) banned_reason_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (banned_entries != NULL) {
    for (guint i = 0; i < banned_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(banned_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_reason(banned_reason_map, entry->name, entry->uuid, entry->reason);
    }
  }

  g_autofree char *banned_ips_path = resolve_data_file(self->current, "banned-ips.json");
  g_autoptr(GPtrArray) banned_ip_entries = load_player_entries_from_file(banned_ips_path);
  g_autoptr(GHashTable) banned_ip_reason_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (banned_ip_entries != NULL) {
    for (guint i = 0; i < banned_ip_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(banned_ip_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_ip_reason(banned_ip_reason_map, entry->ip, entry->reason);
    }
  }

  g_autofree char *ops_path = resolve_data_file(self->current, "ops.json");
  g_autoptr(GPtrArray) ops_entries = load_player_entries_from_file(ops_path);
  g_autoptr(GHashTable) op_level_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (ops_entries != NULL) {
    for (guint i = 0; i < ops_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(ops_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_op_level(op_level_map, entry->name, entry->uuid, entry->op_level);
    }
  }

  for (guint i = 0; i < whitelist_entries->len; i++) {
    PlayerEntry *entry = g_ptr_array_index(whitelist_entries, i);
    if (entry == NULL) {
      continue;
    }

    PlayerState *state = ensure_player_state(self, entry->uuid, entry->name, TRUE);
    if (state == NULL) {
      continue;
    }

    const char *ban_reason = player_lookup_reason_for_state(banned_reason_map, state);
    gboolean banned = (ban_reason != NULL);
    const char *ip_ban_reason = player_lookup_reason_for_ip(banned_ip_reason_map, state->last_ip);
    gboolean ip_banned = (ip_ban_reason != NULL);
    int op_level = player_lookup_op_level_for_state(op_level_map, state);

    if (query_key != NULL && *query_key != '\0') {
      g_autofree char *name_key = normalized_key(state->name);
      g_autofree char *uuid_key = normalized_key(state->uuid);
      g_autofree char *ip_key = normalized_key(state->last_ip);
      g_autofree char *reason_key = normalized_key(ban_reason);
      g_autofree char *ip_reason_key = normalized_key(ip_ban_reason);
      gboolean match = FALSE;
      if (name_key != NULL && strstr(name_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && uuid_key != NULL && strstr(uuid_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && ip_key != NULL && strstr(ip_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && reason_key != NULL && strstr(reason_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && ip_reason_key != NULL && strstr(ip_reason_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match) {
        continue;
      }
    }
    append_player_state_row(self, self->whitelist_list, state, TRUE, banned, ban_reason,
                            ip_banned, ip_ban_reason, op_level, FALSE);
  }
}

static void
refresh_banned_list(PumpkinWindow *self)
{
  clear_list_box(self->banned_list);
  if (self->current == NULL) {
    return;
  }
  ingest_players_from_disk(self);
  const char *query = self->banned_search != NULL
                        ? gtk_editable_get_text(GTK_EDITABLE(self->banned_search))
                        : NULL;
  g_autofree char *query_key = normalized_key(query);

  g_autofree char *banned_path = resolve_data_file(self->current, "banned-players.json");
  g_autoptr(GPtrArray) banned_entries = load_player_entries_from_file(banned_path);
  if (banned_entries == NULL) {
    return;
  }

  g_autofree char *banned_ips_path = resolve_data_file(self->current, "banned-ips.json");
  g_autoptr(GPtrArray) banned_ip_entries = load_player_entries_from_file(banned_ips_path);
  g_autoptr(GHashTable) banned_ip_reason_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (banned_ip_entries != NULL) {
    for (guint i = 0; i < banned_ip_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(banned_ip_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_ip_reason(banned_ip_reason_map, entry->ip, entry->reason);
    }
  }

  g_autofree char *whitelist_path = resolve_data_file(self->current, "whitelist.json");
  g_autoptr(GPtrArray) whitelist_entries = load_player_entries_from_file(whitelist_path);
  g_autoptr(GHashTable) whitelist_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (whitelist_entries != NULL) {
    for (guint i = 0; i < whitelist_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(whitelist_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_set(whitelist_set, entry->name, entry->uuid);
    }
  }

  g_autofree char *ops_path = resolve_data_file(self->current, "ops.json");
  g_autoptr(GPtrArray) ops_entries = load_player_entries_from_file(ops_path);
  g_autoptr(GHashTable) op_level_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (ops_entries != NULL) {
    for (guint i = 0; i < ops_entries->len; i++) {
      PlayerEntry *entry = g_ptr_array_index(ops_entries, i);
      if (entry == NULL) {
        continue;
      }
      player_lookup_add_op_level(op_level_map, entry->name, entry->uuid, entry->op_level);
    }
  }

  for (guint i = 0; i < banned_entries->len; i++) {
    PlayerEntry *entry = g_ptr_array_index(banned_entries, i);
    if (entry == NULL) {
      continue;
    }

    PlayerState *state = ensure_player_state(self, entry->uuid, entry->name, TRUE);
    if (state == NULL) {
      continue;
    }
    gboolean whitelisted = player_lookup_contains_state(whitelist_set, state);
    int op_level = player_lookup_op_level_for_state(op_level_map, state);
    const char *ip_ban_reason = player_lookup_reason_for_ip(banned_ip_reason_map, state->last_ip);
    gboolean ip_banned = (ip_ban_reason != NULL);

    if (query_key != NULL && *query_key != '\0') {
      g_autofree char *name_key = normalized_key(state->name);
      g_autofree char *uuid_key = normalized_key(state->uuid);
      g_autofree char *reason_key = normalized_key(entry->reason);
      g_autofree char *ip_key = normalized_key(state->last_ip);
      g_autofree char *ip_reason_key = normalized_key(ip_ban_reason);
      gboolean match = FALSE;
      if (name_key != NULL && strstr(name_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && uuid_key != NULL && strstr(uuid_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && ip_key != NULL && strstr(ip_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && reason_key != NULL && strstr(reason_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match && ip_reason_key != NULL && strstr(ip_reason_key, query_key) != NULL) {
        match = TRUE;
      }
      if (!match) {
        continue;
      }
    }
    append_player_state_row(self, self->banned_list, state, whitelisted, TRUE, entry->reason,
                            ip_banned, ip_ban_reason, op_level, FALSE);
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
on_player_search_changed(GtkEditable *editable, PumpkinWindow *self)
{
  (void)editable;
  refresh_player_list(self);
}

static void
update_player_sort_buttons(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }

  struct {
    GtkButton *button;
    int field;
    const char *label;
  } items[] = {
    { self->btn_player_sort_last_online, 0, "Last Online" },
    { self->btn_player_sort_playtime, 1, "Playtime" },
    { self->btn_player_sort_first_joined, 2, "First Joined" },
    { self->btn_player_sort_name, 3, "Name" }
  };

  for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
    if (items[i].button == NULL) {
      continue;
    }
    if (self->player_sort_field == items[i].field) {
      const char *arrow = self->player_sort_ascending ? "^" : "v";
      g_autofree char *label = g_strdup_printf("%s %s", items[i].label, arrow);
      gtk_button_set_label(items[i].button, label);
      gtk_widget_add_css_class(GTK_WIDGET(items[i].button), "suggested-action");
    } else {
      gtk_button_set_label(items[i].button, items[i].label);
      gtk_widget_remove_css_class(GTK_WIDGET(items[i].button), "suggested-action");
    }
  }
}

static void
on_whitelist_search_changed(GtkEditable *editable, PumpkinWindow *self)
{
  (void)editable;
  refresh_whitelist_list(self);
}

static void
on_banned_search_changed(GtkEditable *editable, PumpkinWindow *self)
{
  (void)editable;
  refresh_banned_list(self);
}

static void
on_player_sort_button_clicked(GtkButton *button, PumpkinWindow *self)
{
  int field = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "sort-field"));
  if (self->player_sort_field == field) {
    self->player_sort_ascending = !self->player_sort_ascending;
  } else {
    self->player_sort_field = field;
    self->player_sort_ascending = (field == 3);
  }
  update_player_sort_buttons(self);
  refresh_player_list(self);
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
  const char *state_key = g_object_get_data(G_OBJECT(dialog), "player-key");
  const char *uuid = g_object_get_data(G_OBJECT(dialog), "player-uuid");
  const char *last_ip = g_object_get_data(G_OBJECT(dialog), "player-last-ip");
  gboolean is_online = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "player-online")) != 0;
  gboolean is_banned = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "player-banned")) != 0;
  gboolean is_ip_banned = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "player-ip-banned")) != 0;
  int op_level = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "player-op-level")) - 1;
  gboolean is_op = op_level >= 0;

  if (g_strcmp0(response, "delete_data") == 0) {
    if (self->current == NULL) {
      append_log(self, "No server selected.");
      return;
    }
    if (!delete_player_tracking(self, state_key, name, uuid)) {
      append_log(self, "Failed to delete tracked player data.");
      set_details_status(self, "Failed to delete tracked player data", 4);
      return;
    }
    g_autofree char *status = g_strdup_printf("Deleted tracked player data for %s",
                                              (name != NULL && *name != '\0') ? name : "player");
    append_log(self, status);
    set_details_status(self, status, 3);
    return;
  }

  if (self->current == NULL) {
    append_log(self, "No server selected.");
    return;
  }

  gboolean server_running = pumpkin_server_get_running(self->current);
  if (!server_running) {
    append_log(self, "Server is not running.");
    return;
  }

  if (g_strcmp0(response, "banlist") == 0) {
    g_autoptr(GError) error = NULL;
    if (!pumpkin_server_send_command(self->current, "banlist", &error)) {
      if (error != NULL) {
        append_log(self, error->message);
      }
    }
    return;
  }

  if (g_strcmp0(response, "ban_ip") == 0) {
    if (is_ip_banned) {
      append_log(self, "Player IP is already banned.");
      return;
    }
    const char *target = NULL;
    if (last_ip != NULL && *last_ip != '\0') {
      target = last_ip;
    } else if (is_online && name != NULL && *name != '\0') {
      target = name;
    }
    if (target == NULL || *target == '\0') {
      append_log(self, "No known IP for this player yet; cannot run ban-ip.");
      return;
    }

    AdwDialog *reason_dialog = adw_alert_dialog_new("Ban IP", "Optional reason shown in ban history.");
    AdwAlertDialog *alert = ADW_ALERT_DIALOG(reason_dialog);
    adw_alert_dialog_add_response(alert, "cancel", "Cancel");
    adw_alert_dialog_add_response(alert, "ban", "Ban");
    adw_alert_dialog_set_default_response(alert, "ban");
    adw_alert_dialog_set_close_response(alert, "cancel");
    adw_alert_dialog_set_response_appearance(alert, "ban", ADW_RESPONSE_DESTRUCTIVE);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Reason (optional)");
    adw_alert_dialog_set_extra_child(alert, entry);
    adw_dialog_set_focus(ADW_DIALOG(reason_dialog), entry);
    g_object_set_data_full(G_OBJECT(reason_dialog), "ban-command", g_strdup("ban-ip"), g_free);
    g_object_set_data_full(G_OBJECT(reason_dialog), "ban-target", g_strdup(target), g_free);
    g_object_set_data(G_OBJECT(reason_dialog), "reason-entry", entry);
    adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_player_ban_reason_confirmed, self);
    return;
  }

  if (g_strcmp0(response, "pardon_ip") == 0) {
    if (!is_ip_banned) {
      append_log(self, "Player IP is not banned.");
      return;
    }
    if (last_ip == NULL || *last_ip == '\0') {
      append_log(self, "No known IP for this player; cannot run pardon-ip.");
      return;
    }
    g_autofree char *command = g_strdup_printf("pardon-ip %s", last_ip);
    g_autoptr(GError) error = NULL;
    if (!pumpkin_server_send_command(self->current, command, &error)) {
      if (error != NULL) {
        append_log(self, error->message);
      }
    }
    return;
  }

  if (g_strcmp0(response, "ban") == 0) {
    if (is_banned) {
      append_log(self, "Player is already banned.");
      return;
    }
    if (name == NULL || *name == '\0') {
      append_log(self, "Player name unknown; cannot run command.");
      return;
    }
    AdwDialog *reason_dialog = adw_alert_dialog_new("Ban Player", "Optional reason shown in ban history.");
    AdwAlertDialog *alert = ADW_ALERT_DIALOG(reason_dialog);
    adw_alert_dialog_add_response(alert, "cancel", "Cancel");
    adw_alert_dialog_add_response(alert, "ban", "Ban");
    adw_alert_dialog_set_default_response(alert, "ban");
    adw_alert_dialog_set_close_response(alert, "cancel");
    adw_alert_dialog_set_response_appearance(alert, "ban", ADW_RESPONSE_DESTRUCTIVE);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Reason (optional)");
    adw_alert_dialog_set_extra_child(alert, entry);
    adw_dialog_set_focus(ADW_DIALOG(reason_dialog), entry);
    g_object_set_data_full(G_OBJECT(reason_dialog), "ban-command", g_strdup("ban"), g_free);
    g_object_set_data_full(G_OBJECT(reason_dialog), "ban-target", g_strdup(name), g_free);
    g_object_set_data(G_OBJECT(reason_dialog), "reason-entry", entry);
    adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_player_ban_reason_confirmed, self);
    return;
  }

  if (name == NULL || *name == '\0') {
    append_log(self, "Player name unknown; cannot run command.");
    return;
  }

  const char *cmd = NULL;
  if (g_strcmp0(response, "kick") == 0) {
    cmd = "kick";
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

  if (g_strcmp0(response, "kick") == 0) {
    g_autofree char *command = g_strdup_printf("%s %s", cmd, name);
    g_autoptr(GError) error = NULL;
    if (!pumpkin_server_send_command(self->current, command, &error)) {
      if (error != NULL) {
        append_log(self, error->message);
      }
    }
    return;
  }

  if (g_strcmp0(response, "unban") == 0 && !is_banned) {
    append_log(self, "Player is not banned.");
    return;
  }
  if (g_strcmp0(response, "op") == 0 && is_op) {
    append_log(self, "Player is already op.");
    return;
  }
  if (g_strcmp0(response, "deop") == 0 && !is_op) {
    append_log(self, "Player is not op.");
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
on_player_ban_reason_confirmed(GObject *dialog, GAsyncResult *res, gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
  if (response == NULL || g_strcmp0(response, "ban") != 0) {
    return;
  }

  const char *command_name = g_object_get_data(G_OBJECT(dialog), "ban-command");
  const char *target = g_object_get_data(G_OBJECT(dialog), "ban-target");
  if (command_name == NULL || *command_name == '\0') {
    command_name = "ban";
  }
  if (target == NULL || *target == '\0') {
    target = g_object_get_data(G_OBJECT(dialog), "player-name");
  }
  GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "reason-entry");
  if (self->current == NULL || target == NULL || *target == '\0') {
    append_log(self, "Ban target unknown; cannot run command.");
    return;
  }
  gboolean server_running = pumpkin_server_get_running(self->current);
  if (!server_running) {
    append_log(self, "Server is not running.");
    return;
  }

  const char *reason = entry != NULL ? gtk_editable_get_text(GTK_EDITABLE(entry)) : NULL;
  g_autofree char *reason_clean = NULL;
  if (reason != NULL && *reason != '\0') {
    reason_clean = g_strdup(reason);
    for (char *p = reason_clean; *p != '\0'; p++) {
      if (*p == '\r' || *p == '\n') {
        *p = ' ';
      }
    }
  }

  g_autofree char *command = NULL;
  if (reason_clean != NULL && *reason_clean != '\0') {
    command = g_strdup_printf("%s %s %s", command_name, target, reason_clean);
  } else {
    command = g_strdup_printf("%s %s", command_name, target);
  }

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
  const char *state_key = g_object_get_data(G_OBJECT(row), "player-key");
  const char *last_ip = g_object_get_data(G_OBJECT(row), "player-last-ip");
  gboolean is_online = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "player-online")) != 0;
  gboolean is_banned = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "player-banned")) != 0;
  gboolean is_ip_banned = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "player-ip-banned")) != 0;
  int op_level = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "player-op-level")) - 1;
  gboolean is_op = op_level >= 0;
  gboolean is_admin = player_is_admin_from_op_level(op_level);
  gboolean has_last_ip = (last_ip != NULL && *last_ip != '\0');
  PlayerState *state = NULL;
  if (state_key != NULL && self->player_states != NULL) {
    state = g_hash_table_lookup(self->player_states, state_key);
  }

  const char *title = (name != NULL && *name != '\0') ? name : "Player";
  g_autofree char *body = NULL;
  if (state != NULL) {
    g_autofree char *playtime = format_duration(player_state_effective_playtime(state));
    g_autofree char *first_joined = format_unix_time(self, state->first_joined_unix);
    g_autofree char *last_seen = format_unix_time(self, state->last_online_unix);
    const char *perm_label = is_admin ? "ADMIN" : (is_op ? "OP" : "Player");
    body = g_strdup_printf("UUID: %s\nPlatform: %s\nStatus: %s\nPermissions: %s\nBanned: %s\nIP banned: %s\nLast IP: %s\nFirst joined: %s\nLast online: %s\nPlaytime: %s",
                           state->uuid != NULL ? state->uuid : "Unknown",
                           platform_label(state->platform),
                           state->online ? "Online" : "Offline",
                           perm_label,
                           is_banned ? "Yes" : "No",
                           is_ip_banned ? "Yes" : "No",
                           has_last_ip ? last_ip : "Unknown",
                           first_joined,
                           state->online ? "Now" : last_seen,
                           playtime);
  } else if (uuid != NULL) {
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
  adw_alert_dialog_add_response(alert, "ban_ip", "Ban IP");
  adw_alert_dialog_add_response(alert, "pardon_ip", "Pardon IP");
  adw_alert_dialog_add_response(alert, "banlist", "Banlist");
  adw_alert_dialog_add_response(alert, "delete_data", "Delete Data");
  adw_alert_dialog_set_default_response(alert, "cancel");

  adw_alert_dialog_set_response_appearance(alert, "cancel", ADW_RESPONSE_DEFAULT);
  adw_alert_dialog_set_response_appearance(alert, "ban", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance(alert, "ban_ip", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance(alert, "deop", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance(alert, "delete_data", ADW_RESPONSE_DESTRUCTIVE);
  adw_dialog_set_can_close(ADW_DIALOG(dialog), TRUE);

  if (self->current == NULL) {
    adw_alert_dialog_set_response_enabled(alert, "kick", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "ban", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "unban", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "op", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "deop", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "ban_ip", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "pardon_ip", FALSE);
    adw_alert_dialog_set_response_enabled(alert, "banlist", FALSE);
  } else {
    gboolean running = pumpkin_server_get_running(self->current);
    adw_alert_dialog_set_response_enabled(alert, "ban", running && !is_banned);
    adw_alert_dialog_set_response_enabled(alert, "unban", running && is_banned);
    adw_alert_dialog_set_response_enabled(alert, "op", running && !is_op);
    adw_alert_dialog_set_response_enabled(alert, "deop", running && is_op);
    adw_alert_dialog_set_response_enabled(alert, "kick", running && is_online);
    adw_alert_dialog_set_response_enabled(alert, "ban_ip",
                                          running && !is_ip_banned && ((name != NULL && *name != '\0' && is_online) || has_last_ip));
    adw_alert_dialog_set_response_enabled(alert, "pardon_ip",
                                          running && is_ip_banned && has_last_ip);
    adw_alert_dialog_set_response_enabled(alert, "banlist", running);
  }

  if (name != NULL) {
    g_object_set_data_full(G_OBJECT(dialog), "player-name", g_strdup(name), g_free);
  }
  if (state_key != NULL) {
    g_object_set_data_full(G_OBJECT(dialog), "player-key", g_strdup(state_key), g_free);
  }
  if (uuid != NULL) {
    g_object_set_data_full(G_OBJECT(dialog), "player-uuid", g_strdup(uuid), g_free);
  }
  if (has_last_ip) {
    g_object_set_data_full(G_OBJECT(dialog), "player-last-ip", g_strdup(last_ip), g_free);
  }
  g_object_set_data(G_OBJECT(dialog), "player-online", GINT_TO_POINTER(is_online ? 1 : 0));
  g_object_set_data(G_OBJECT(dialog), "player-banned", GINT_TO_POINTER(is_banned ? 1 : 0));
  g_object_set_data(G_OBJECT(dialog), "player-ip-banned", GINT_TO_POINTER(is_ip_banned ? 1 : 0));
  g_object_set_data(G_OBJECT(dialog), "player-op-level", GINT_TO_POINTER(op_level + 1));

  adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, on_player_action_confirmed, self);
}

static void
update_settings_form(PumpkinWindow *self)
{
  if (self->entry_server_name == NULL || self->entry_download_url == NULL ||
      self->entry_server_port == NULL || self->entry_bedrock_port == NULL ||
      self->entry_max_players == NULL || self->entry_stats_sample_msec == NULL ||
      self->entry_max_cpu_cores == NULL || self->entry_max_ram_mb == NULL ||
      self->entry_rcon_host == NULL || self->entry_rcon_port == NULL ||
      self->entry_rcon_password == NULL) {
    return;
  }
  self->settings_loading = TRUE;
  self->settings_invalid = FALSE;
  if (self->config != NULL && self->drop_date_format != NULL) {
    gtk_drop_down_set_selected(self->drop_date_format, (guint)pumpkin_config_get_date_format(self->config));
  }
  if (self->config != NULL && self->drop_time_format != NULL) {
    gtk_drop_down_set_selected(self->drop_time_format, (guint)pumpkin_config_get_time_format(self->config));
  }
  if (self->current == NULL) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_server_name), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_download_url), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_server_port), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_bedrock_port), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_max_players), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_stats_sample_msec), "");
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
    if (self->switch_auto_update != NULL) {
      gtk_switch_set_active(self->switch_auto_update, FALSE);
    }
    if (self->switch_auto_update_schedule != NULL) {
      gtk_switch_set_active(self->switch_auto_update_schedule, FALSE);
    }
    if (self->entry_auto_update_time != NULL) {
      gtk_editable_set_text(GTK_EDITABLE(self->entry_auto_update_time), "01:00");
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
    if (self->label_stats_sample_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_stats_sample_hint), FALSE);
    }
    if (self->label_rcon_host_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_host_hint), FALSE);
    }
    if (self->label_rcon_port_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_rcon_port_hint), FALSE);
    }
    if (self->label_auto_update_time_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_auto_update_time_hint), FALSE);
    }
    if (self->label_max_cpu_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_cpu_hint), FALSE);
    }
    if (self->label_max_ram_hint != NULL) {
      gtk_widget_set_visible(GTK_WIDGET(self->label_max_ram_hint), FALSE);
    }
    update_auto_update_controls_sensitivity(self);
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
  g_autofree char *stats_sample = g_strdup_printf("%d", pumpkin_server_get_stats_sample_msec(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_stats_sample_msec), stats_sample);

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
  if (self->switch_auto_update != NULL) {
    gtk_switch_set_active(self->switch_auto_update,
                          pumpkin_server_get_auto_update_enabled(self->current));
  }
  if (self->switch_auto_update_schedule != NULL) {
    gtk_switch_set_active(self->switch_auto_update_schedule,
                          pumpkin_server_get_auto_update_use_schedule(self->current));
  }
  if (self->entry_auto_update_time != NULL) {
    g_autofree char *update_time =
      g_strdup_printf("%02d:%02d",
                      pumpkin_server_get_auto_update_hour(self->current),
                      pumpkin_server_get_auto_update_minute(self->current));
    gtk_editable_set_text(GTK_EDITABLE(self->entry_auto_update_time), update_time);
  }

  g_autofree char *rcon_port = g_strdup_printf("%d", pumpkin_server_get_rcon_port(self->current));
  gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_port), rcon_port);

  const char *password = pumpkin_server_get_rcon_password(self->current);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_rcon_password), password ? password : "");
  self->settings_loading = FALSE;
  update_auto_update_controls_sensitivity(self);
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
    player_states_mark_all_offline(self);
    player_states_save(self, self->current);
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
  apply_stats_sample_msec(self,
                          server != NULL ? pumpkin_server_get_stats_sample_msec(server)
                                         : DEFAULT_STATS_SAMPLE_MSEC,
                          FALSE);
  self->query_valid = FALSE;
  self->query_in_flight = FALSE;
  self->query_players = 0;
  self->query_max_players = 0;
  self->query_updated_at = 0;
  self->last_tps_request_at = 0;
  self->last_player_list_request_at = 0;
  self->last_player_state_flush_at = 0;
  self->last_auto_update_eval_at = 0;
  self->pending_auto_tps_lines = 0;
  self->pending_auto_list_lines = 0;
  self->pending_java_platform_hints = 0;
  self->pending_bedrock_platform_hints = 0;
  if (self->player_head_downloads != NULL) {
    g_hash_table_remove_all(self->player_head_downloads);
  }
  invalidate_player_list_signature(self);

  if (server != NULL) {
    player_states_load(self, server);
  } else {
    player_states_clear(self);
  }

  if (self->log_view != NULL) {
    GtkTextBuffer *buffer = NULL;
    if (server != NULL) {
      buffer = g_hash_table_lookup(self->console_buffers, server);
      if (buffer == NULL) {
        buffer = gtk_text_buffer_new(NULL);
        g_hash_table_insert(self->console_buffers, g_object_ref(server), buffer);
      }
      ensure_console_buffer_tags(self, buffer);
    } else {
      buffer = gtk_text_buffer_new(NULL);
      ensure_console_buffer_tags(self, buffer);
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
  set_stats_graphs_disabled(self, server == NULL || !pumpkin_server_get_running(server));
  if (server != NULL) {
    ensure_default_server_icon(server);
  }
  maybe_trigger_auto_update(self);
}

static void
select_server_row(PumpkinWindow *self, PumpkinServer *server)
{
  if (self->server_list == NULL) {
    select_server(self, server);
    return;
  }

  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->server_list));
  while (child != NULL) {
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(child);
    PumpkinServer *row_server = g_object_get_data(G_OBJECT(row), "server");
    if (row_server == server) {
      gtk_list_box_select_row(self->server_list, row);
      return;
    }
    child = gtk_widget_get_next_sibling(child);
  }

  select_server(self, server);
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
  const unsigned char *p = (const unsigned char *)line;
  while (*p != '\0') {
    if (*p == 0x1B) {
      p++;
      if (*p == '[') {
        p++;
        while (*p != '\0' && (*p < 0x40 || *p > 0x7E)) {
          p++;
        }
        if (*p != '\0') {
          p++;
        }
        continue;
      }
      if (*p == ']') {
        p++;
        while (*p != '\0') {
          if (*p == '\a') {
            p++;
            break;
          }
          if (*p == 0x1B && p[1] == '\\') {
            p += 2;
            break;
          }
          p++;
        }
        continue;
      }
      continue;
    }
    g_string_append_c(out, (char)*p);
    p++;
  }
  return g_string_free(out, FALSE);
}

static const char *
console_timestamp_pattern_for_config(PumpkinWindow *self)
{
  PumpkinDateFormat format = PUMPKIN_DATE_FORMAT_DMY;
  PumpkinTimeFormat time_format = PUMPKIN_TIME_FORMAT_24H;
  if (self != NULL && self->config != NULL) {
    format = pumpkin_config_get_date_format(self->config);
    time_format = pumpkin_config_get_time_format(self->config);
  }
  if (format == PUMPKIN_DATE_FORMAT_YMD) {
    return time_format == PUMPKIN_TIME_FORMAT_12H
             ? "%Y-%m-%d %I:%M:%S %p"
             : "%Y-%m-%d %H:%M:%S";
  }
  if (format == PUMPKIN_DATE_FORMAT_MDY) {
    return time_format == PUMPKIN_TIME_FORMAT_12H
             ? "%m/%d/%Y %I:%M:%S %p"
             : "%m/%d/%Y %H:%M:%S";
  }
  return time_format == PUMPKIN_TIME_FORMAT_12H
           ? "%d.%m.%Y %I:%M:%S %p"
           : "%d.%m.%Y %H:%M:%S";
}

static ConsoleLevel
console_level_from_text(const char *level_text)
{
  if (level_text == NULL || *level_text == '\0') {
    return CONSOLE_LEVEL_OTHER;
  }
  g_autofree char *lower = g_ascii_strdown(level_text, -1);
  if (g_str_has_prefix(lower, "trace")) {
    return CONSOLE_LEVEL_TRACE;
  }
  if (g_str_has_prefix(lower, "debug")) {
    return CONSOLE_LEVEL_DEBUG;
  }
  if (g_str_has_prefix(lower, "info")) {
    return CONSOLE_LEVEL_INFO;
  }
  if (g_str_has_prefix(lower, "warn")) {
    return CONSOLE_LEVEL_WARN;
  }
  if (g_str_has_prefix(lower, "error")) {
    return CONSOLE_LEVEL_ERROR;
  }
  if (g_str_has_prefix(lower, "smpk")) {
    return CONSOLE_LEVEL_SMPK;
  }
  return CONSOLE_LEVEL_OTHER;
}

static const char *
console_level_tag_name(ConsoleLevel level)
{
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      return "console-level-trace";
    case CONSOLE_LEVEL_DEBUG:
      return "console-level-debug";
    case CONSOLE_LEVEL_INFO:
      return "console-level-info";
    case CONSOLE_LEVEL_WARN:
      return "console-level-warn";
    case CONSOLE_LEVEL_ERROR:
      return "console-level-error";
    case CONSOLE_LEVEL_SMPK:
      return "console-level-smpk";
    case CONSOLE_LEVEL_OTHER:
    default:
      return "console-level-other";
  }
}

static gboolean
console_level_enabled(PumpkinWindow *self, ConsoleLevel level)
{
  if (self == NULL) {
    return TRUE;
  }
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      return self->check_console_trace == NULL ||
             gtk_check_button_get_active(self->check_console_trace);
    case CONSOLE_LEVEL_DEBUG:
      return self->check_console_debug == NULL ||
             gtk_check_button_get_active(self->check_console_debug);
    case CONSOLE_LEVEL_INFO:
      return self->check_console_info == NULL ||
             gtk_check_button_get_active(self->check_console_info);
    case CONSOLE_LEVEL_WARN:
      return self->check_console_warn == NULL ||
             gtk_check_button_get_active(self->check_console_warn);
    case CONSOLE_LEVEL_ERROR:
      return self->check_console_error == NULL ||
             gtk_check_button_get_active(self->check_console_error);
    case CONSOLE_LEVEL_SMPK:
      return self->check_console_smpk == NULL ||
             gtk_check_button_get_active(self->check_console_smpk);
    case CONSOLE_LEVEL_OTHER:
    default:
      return self->check_console_other == NULL ||
             gtk_check_button_get_active(self->check_console_other);
  }
}

static void
apply_console_filters_to_buffer(PumpkinWindow *self, GtkTextBuffer *buffer)
{
  if (self == NULL || buffer == NULL) {
    return;
  }
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  if (table == NULL) {
    return;
  }
  ConsoleLevel levels[] = {
    CONSOLE_LEVEL_TRACE,
    CONSOLE_LEVEL_DEBUG,
    CONSOLE_LEVEL_INFO,
    CONSOLE_LEVEL_WARN,
    CONSOLE_LEVEL_ERROR,
    CONSOLE_LEVEL_SMPK,
    CONSOLE_LEVEL_OTHER
  };
  for (guint i = 0; i < G_N_ELEMENTS(levels); i++) {
    const char *tag_name = console_level_tag_name(levels[i]);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, tag_name);
    if (tag != NULL) {
      gboolean visible = console_level_enabled(self, levels[i]);
      g_object_set(tag, "invisible", visible ? FALSE : TRUE, NULL);
    }
  }
}

static void
ensure_console_buffer_tags(PumpkinWindow *self, GtkTextBuffer *buffer)
{
  if (buffer == NULL) {
    return;
  }
  ConsoleLevel levels[] = {
    CONSOLE_LEVEL_TRACE,
    CONSOLE_LEVEL_DEBUG,
    CONSOLE_LEVEL_INFO,
    CONSOLE_LEVEL_WARN,
    CONSOLE_LEVEL_ERROR,
    CONSOLE_LEVEL_SMPK,
    CONSOLE_LEVEL_OTHER
  };
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  if (table == NULL) {
    return;
  }
  for (guint i = 0; i < G_N_ELEMENTS(levels); i++) {
    const char *tag_name = console_level_tag_name(levels[i]);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, tag_name);
    if (tag == NULL) {
      tag = gtk_text_buffer_create_tag(buffer, tag_name, NULL);
    }
    if (tag == NULL) {
      continue;
    }
    /* Keep level tags neutral; they are used for filtering visibility only. */
    g_object_set(tag, "foreground-set", FALSE, "weight-set", FALSE, NULL);
  }

  struct {
    const char *name;
    const char *color;
    int weight;
  } token_tags[] = {
    {"console-token-trace", "#7a828a", PANGO_WEIGHT_BOLD},
    {"console-token-debug", "#6f767e", PANGO_WEIGHT_BOLD},
    {"console-token-info", "#2f8f46", PANGO_WEIGHT_BOLD},
    {"console-token-warn", "#b7791f", PANGO_WEIGHT_BOLD},
    {"console-token-error", "#c93434", PANGO_WEIGHT_BOLD},
    {"console-token-smpk", "#4b6cb7", PANGO_WEIGHT_BOLD},
    {"console-token-startup-ms", NULL, PANGO_WEIGHT_BOLD}
  };
  for (guint i = 0; i < G_N_ELEMENTS(token_tags); i++) {
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, token_tags[i].name);
    if (tag == NULL) {
      tag = gtk_text_buffer_create_tag(buffer, token_tags[i].name, NULL);
    }
    if (tag == NULL) {
      continue;
    }
    if (token_tags[i].color != NULL) {
      g_object_set(tag, "foreground", token_tags[i].color, "weight", token_tags[i].weight, NULL);
    } else {
      g_object_set(tag, "weight", token_tags[i].weight, NULL);
    }
  }
  apply_console_filters_to_buffer(self, buffer);
}

static void
apply_console_filters(PumpkinWindow *self)
{
  if (self == NULL || self->console_buffers == NULL) {
    return;
  }
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->console_buffers);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(value);
    ensure_console_buffer_tags(self, buffer);
    apply_console_filters_to_buffer(self, buffer);
  }
}

static void
on_console_filter_toggled(GtkCheckButton *button, PumpkinWindow *self)
{
  (void)button;
  apply_console_filters(self);
}

static void
on_console_filter_all_clicked(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self == NULL) {
    return;
  }

  GtkCheckButton *checks[] = {
    self->check_console_trace,
    self->check_console_debug,
    self->check_console_info,
    self->check_console_warn,
    self->check_console_error,
    self->check_console_smpk,
    self->check_console_other
  };

  gboolean all_enabled = TRUE;
  for (guint i = 0; i < G_N_ELEMENTS(checks); i++) {
    if (checks[i] == NULL) {
      continue;
    }
    if (!gtk_check_button_get_active(checks[i])) {
      all_enabled = FALSE;
      break;
    }
  }

  gboolean next_state = !all_enabled;
  for (guint i = 0; i < G_N_ELEMENTS(checks); i++) {
    if (checks[i] == NULL) {
      continue;
    }
    gtk_check_button_set_active(checks[i], next_state);
  }
  apply_console_filters(self);
}

static char *
sanitize_console_text(const char *line)
{
  if (line == NULL) {
    return NULL;
  }
  g_autofree char *without_ansi = strip_ansi(line);
  const char *src = without_ansi != NULL ? without_ansi : line;
  GString *out = g_string_sized_new(strlen(src) + 8);
  for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
    unsigned char c = *p;
    if (c == '\r' || c == '\n') {
      continue;
    }
    if (c == '\t') {
      g_string_append_c(out, ' ');
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      continue;
    }
    g_string_append_c(out, (char)c);
  }
  char *text = g_string_free(out, FALSE);
  g_strstrip(text);
  return text;
}

static char *
format_console_line(PumpkinWindow *self, const char *line, ConsoleLevel *out_level)
{
  if (out_level != NULL) {
    *out_level = CONSOLE_LEVEL_OTHER;
  }
  g_autofree char *clean = sanitize_console_text(line);
  if (clean == NULL || *clean == '\0') {
    return NULL;
  }

  static GRegex *prefix_re = NULL;
  if (prefix_re == NULL) {
    prefix_re = g_regex_new(
      "^([0-9]{4})-([0-9]{2})-([0-9]{2})[ T]([0-9]{2}):([0-9]{2}):([0-9]{2})\\s+([A-Za-z]+)\\s+(.+)$",
      G_REGEX_OPTIMIZE, 0, NULL);
  }

  g_autofree char *timestamp_text = NULL;
  g_autofree char *message = NULL;
  g_autofree char *level = NULL;
  g_autofree char *target = NULL;

  if (g_str_has_prefix(clean, "[SMPK]")) {
    const char *rest = clean + strlen("[SMPK]");
    while (*rest == ' ') {
      rest++;
    }
    level = g_strdup("SMPK");
    message = g_strdup(rest);
    if (out_level != NULL) {
      *out_level = CONSOLE_LEVEL_SMPK;
    }
  } else if (g_str_has_prefix(clean, "SMPK:")) {
    const char *rest = clean + strlen("SMPK:");
    while (*rest == ' ') {
      rest++;
    }
    level = g_strdup("SMPK");
    message = g_strdup(rest);
    if (out_level != NULL) {
      *out_level = CONSOLE_LEVEL_SMPK;
    }
  }

  if (prefix_re != NULL && level == NULL) {
    g_autoptr(GMatchInfo) match = NULL;
    if (g_regex_match(prefix_re, clean, 0, &match) && g_match_info_matches(match)) {
      g_autofree char *year_txt = g_match_info_fetch(match, 1);
      g_autofree char *month_txt = g_match_info_fetch(match, 2);
      g_autofree char *day_txt = g_match_info_fetch(match, 3);
      g_autofree char *hour_txt = g_match_info_fetch(match, 4);
      g_autofree char *minute_txt = g_match_info_fetch(match, 5);
      g_autofree char *second_txt = g_match_info_fetch(match, 6);
      level = g_match_info_fetch(match, 7);
      g_autofree char *rest = g_match_info_fetch(match, 8);

      int year = year_txt != NULL ? (int)strtol(year_txt, NULL, 10) : 0;
      int month = month_txt != NULL ? (int)strtol(month_txt, NULL, 10) : 0;
      int day = day_txt != NULL ? (int)strtol(day_txt, NULL, 10) : 0;
      int hour = hour_txt != NULL ? (int)strtol(hour_txt, NULL, 10) : 0;
      int minute = minute_txt != NULL ? (int)strtol(minute_txt, NULL, 10) : 0;
      int second = second_txt != NULL ? (int)strtol(second_txt, NULL, 10) : 0;
      g_autoptr(GDateTime) dt_utc = g_date_time_new_utc(year, month, day, hour, minute, (gdouble)second);
      if (dt_utc != NULL) {
        g_autoptr(GDateTime) dt_local = g_date_time_to_local(dt_utc);
        if (dt_local != NULL) {
          timestamp_text = g_date_time_format(dt_local, console_timestamp_pattern_for_config(self));
        }
      }
      if (out_level != NULL) {
        *out_level = console_level_from_text(level);
      }

      if (rest != NULL) {
        const char *split = strstr(rest, ": ");
        if (split != NULL) {
          g_autofree char *prefix = g_strndup(rest, (gsize)(split - rest));
          message = g_strdup(split + 2);
          g_strstrip(prefix);
          if (prefix[0] != '\0') {
            const char *last_space = strrchr(prefix, ' ');
            target = (last_space != NULL && last_space[1] != '\0')
                       ? g_strdup(last_space + 1)
                       : g_strdup(prefix);
          }
        } else {
          message = g_strdup(rest);
        }
      }
    }
  }

  if (timestamp_text == NULL) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    if (now != NULL) {
      timestamp_text = g_date_time_format(now, console_timestamp_pattern_for_config(self));
    }
  }
  if (timestamp_text == NULL) {
    timestamp_text = g_strdup("--");
  }

  if (message == NULL || *message == '\0') {
    message = g_strdup(clean);
  }
  g_strstrip(message);

  if (level != NULL && *level != '\0') {
    g_autofree char *level_upper = g_ascii_strup(level, -1);
    if (target != NULL && *target != '\0') {
      return g_strdup_printf("[%s] [%s] [%s] %s", timestamp_text, level_upper, target, message);
    }
    return g_strdup_printf("[%s] [%s] %s", timestamp_text, level_upper, message);
  }

  return g_strdup_printf("[%s] %s", timestamp_text, message);
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
  g_clear_pointer(&ctx->used_build_id, g_free);
  g_clear_pointer(&ctx->used_build_label, g_free);
  g_clear_pointer(&ctx->dest_path, g_free);
  g_clear_pointer(&ctx->tmp_path, g_free);
  g_clear_pointer(&ctx->server_bin, g_free);
  g_free(ctx);
}

static void
start_download_for_server(PumpkinWindow *self,
                          PumpkinServer *server,
                          const char *url,
                          gboolean force_no_cache,
                          gboolean restart_after_download)
{
  if (server == NULL) {
    return;
  }

  gboolean using_latest = (self->latest_url != NULL && g_strcmp0(url, self->latest_url) == 0);
  const char *resolved_build_id = using_latest ? self->latest_build_id : NULL;
  const char *resolved_build_label = using_latest ? self->latest_build_label : NULL;

  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  gboolean use_cache = !force_no_cache && use_download_cache(self);
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
      pumpkin_server_set_installed_build_id(server, resolved_build_id);
      if (resolved_build_label != NULL && *resolved_build_label != '\0') {
        g_autofree char *normalized = normalize_build_label(self, resolved_build_label);
        pumpkin_server_set_installed_build_label(server, normalized);
      } else {
        g_autofree char *local_label = build_label_from_binary_path(self, bin);
        pumpkin_server_set_installed_build_label(server, local_label);
      }
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
  ctx->used_build_id = g_strdup(resolved_build_id);
  ctx->used_build_label = normalize_build_label(self, resolved_build_label);
  ctx->dest_path = g_strdup(cache_path != NULL ? cache_path : bin);
  ctx->tmp_path = g_strdup(tmp);
  ctx->server_bin = g_strdup(bin);
  ctx->use_cache = (cache_path != NULL);
  ctx->restart_after_download = restart_after_download;

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
  pumpkin_server_set_installed_build_id(server, ctx->used_build_id);
  if (ctx->used_build_label != NULL && *ctx->used_build_label != '\0') {
    pumpkin_server_set_installed_build_label(server, ctx->used_build_label);
  } else {
    g_autofree char *local_label = build_label_from_binary_path(self, ctx->server_bin);
    pumpkin_server_set_installed_build_label(server, local_label);
  }
  pumpkin_server_save(server, NULL);
  if (ctx->restart_after_download) {
    if (pumpkin_server_get_running(server)) {
      if (self->current == server) {
        self->restart_requested = TRUE;
        self->restart_pending = TRUE;
        self->ui_state = UI_STATE_RESTARTING;
        self->user_stop_requested = TRUE;
      }
      pumpkin_server_stop(server);
      set_details_status_for_server(self, server, "Update installed, restarting server...", 4);
    } else {
      g_autoptr(GError) start_error = NULL;
      if (self->current == server) {
        self->ui_state = UI_STATE_STARTING;
        self->user_stop_requested = FALSE;
        set_console_warning(self, NULL, FALSE);
      }
      if (!pumpkin_server_start(server, &start_error)) {
        set_details_status_for_server(self, server, "Update installed but restart failed", 5);
        if (self->current == server) {
          set_details_error(self, start_error != NULL ? start_error->message : "Failed to restart after update");
          self->ui_state = UI_STATE_ERROR;
        }
      } else {
        set_details_status_for_server(self, server, "Updated and restarting server...", 4);
        if (self->current == server) {
          refresh_plugin_list(self);
          refresh_world_list(self);
          refresh_player_list(self);
          refresh_log_files(self);
        }
      }
    }
  }
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
  self->latest_resolve_in_flight = FALSE;
  g_autoptr(GError) error = NULL;

  PumpkinDownloadResult result_code = PUMPKIN_DOWNLOAD_OK;
  PumpkinResolvedDownload *resolved = pumpkin_resolve_latest_finish(res, &result_code, &error);
  if (resolved == NULL || resolved->url == NULL) {
    if (self->current != NULL) {
      set_details_status(self, "Update check failed", 4);
    }
    update_check_updates_badge(self);
    update_details(self);
    return;
  }
  if (result_code == PUMPKIN_DOWNLOAD_FALLBACK_USED) {
    set_details_status(self, "Using fallback download URL", 4);
  }

  g_clear_pointer(&self->latest_url, g_free);
  self->latest_url = g_strdup(resolved->url);
  g_clear_pointer(&self->latest_build_id, g_free);
  self->latest_build_id = g_strdup(resolved->build_id);
  g_clear_pointer(&self->latest_build_label, g_free);
  self->latest_build_label = normalize_build_label(self, resolved->build_label);
  refresh_overview_list(self);
  update_check_updates_badge(self);
  update_details(self);
  maybe_trigger_auto_update(self);

  if (self->config != NULL) {
    pumpkin_config_set_default_download_url(self->config, resolved->url);
    pumpkin_config_save(self->config, NULL);
  }

  pumpkin_resolved_download_free(resolved);
}

static void
trigger_latest_resolve(PumpkinWindow *self)
{
  if (self == NULL || self->latest_resolve_in_flight) {
    return;
  }
  self->latest_resolve_in_flight = TRUE;
  update_check_updates_badge(self);
  pumpkin_resolve_latest_async(NULL, on_latest_only_resolve_done, self);
}

static gboolean
poll_latest_release_tick(gpointer data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(data);
  trigger_latest_resolve(self);
  return G_SOURCE_CONTINUE;
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
      g_autofree char *msg = g_strdup_printf("Port %d may already be in use (%s). Trying to start anyway.",
                                             port, port_error->message);
      append_log(self, msg);
    } else {
      g_autofree char *msg = g_strdup_printf("Port %d may already be in use. Trying to start anyway.", port);
      append_log(self, msg);
    }
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
  if (self->auto_update_server == self->current) {
    clear_auto_update_countdown(self);
  }
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
  if (self->auto_update_server == self->current) {
    clear_auto_update_countdown(self);
  }
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
  start_download_for_server(self, self->current, url, FALSE, FALSE);
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

  if (!is_update_available_for_server(self, self->current, TRUE)) {
    set_details_status(self, "Up to date", 3);
    return;
  }

  const char *from_label = pumpkin_server_get_installed_build_label(self->current);
  const char *from_id = pumpkin_server_get_installed_build_id(self->current);
  const char *to_label = self->latest_build_label;
  const char *to_id = self->latest_build_id;
  g_autofree char *from_label_norm = normalize_build_label(self, from_label);
  g_autofree char *to_label_norm = normalize_build_label(self, to_label);
  const char *from = (from_label_norm != NULL && *from_label_norm != '\0') ? from_label_norm :
                     ((from_id != NULL && *from_id != '\0') ? from_id : "installed");
  const char *to = (to_label_norm != NULL && *to_label_norm != '\0') ? to_label_norm :
                   ((to_id != NULL && *to_id != '\0') ? to_id : "latest");
  g_autofree char *status = g_strdup_printf("Updating: %s -> %s", from, to);
  set_details_status(self, status, 6);

  start_download_for_server(self, self->current, self->latest_url, TRUE, FALSE);
}

static void
on_details_check_updates(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
    return;
  }

  g_autofree char *bin = pumpkin_server_get_bin_path(self->current);
  gboolean installed = g_file_test(bin, G_FILE_TEST_EXISTS);
  if (installed && is_update_available_for_server(self, self->current, TRUE)) {
    on_details_update(NULL, self);
    return;
  }
  set_details_status(self, "Up to date", 3);
}

static void
command_history_reset_navigation(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  self->command_history_index = -1;
  g_clear_pointer(&self->command_history_draft, g_free);
}

static void
command_history_push(PumpkinWindow *self, const char *command)
{
  if (self == NULL || self->command_history == NULL || command == NULL || *command == '\0') {
    return;
  }

  if (self->command_history->len > 0) {
    const char *last = g_ptr_array_index(self->command_history, self->command_history->len - 1);
    if (g_strcmp0(last, command) == 0) {
      command_history_reset_navigation(self);
      return;
    }
  }

  g_ptr_array_add(self->command_history, g_strdup(command));
  if (self->command_history->len > COMMAND_HISTORY_MAX) {
    g_ptr_array_remove_index(self->command_history, 0);
  }
  command_history_reset_navigation(self);
}

static gboolean
on_command_entry_key_pressed(GtkEventControllerKey *controller,
                             guint keyval,
                             guint keycode,
                             GdkModifierType state,
                             PumpkinWindow *self)
{
  (void)controller;
  (void)keycode;
  if (self == NULL || self->entry_command == NULL || self->command_history == NULL ||
      self->command_history->len == 0) {
    return FALSE;
  }

  if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) != 0) {
    return FALSE;
  }

  gboolean up = (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up);
  gboolean down = (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down);
  if (!up && !down) {
    return FALSE;
  }

  const char *current_text = gtk_editable_get_text(GTK_EDITABLE(self->entry_command));
  if (up) {
    if (self->command_history_index < 0) {
      self->command_history_index = (int)self->command_history->len - 1;
      g_clear_pointer(&self->command_history_draft, g_free);
      self->command_history_draft = g_strdup(current_text != NULL ? current_text : "");
    } else if (self->command_history_index > 0) {
      self->command_history_index--;
    }
  } else {
    if (self->command_history_index < 0) {
      return FALSE;
    }
    if (self->command_history_index < (int)self->command_history->len - 1) {
      self->command_history_index++;
    } else {
      self->command_history_index = -1;
    }
  }

  const char *replacement = "";
  if (self->command_history_index >= 0 &&
      self->command_history_index < (int)self->command_history->len) {
    replacement = g_ptr_array_index(self->command_history, self->command_history_index);
  } else if (self->command_history_draft != NULL) {
    replacement = self->command_history_draft;
  }
  gtk_editable_set_text(GTK_EDITABLE(self->entry_command), replacement);
  gtk_editable_set_position(GTK_EDITABLE(self->entry_command), -1);
  if (self->command_history_index < 0) {
    g_clear_pointer(&self->command_history_draft, g_free);
  }
  return TRUE;
}

static void
on_send_command(GtkWidget *widget, PumpkinWindow *self)
{
  (void)widget;
  if (self->current == NULL) {
    return;
  }

  const char *cmd = gtk_editable_get_text(GTK_EDITABLE(self->entry_command));
  g_autofree char *command = g_strdup(cmd != NULL ? cmd : "");
  g_strstrip(command);
  if (command[0] == '\0') {
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_send_command(self->current, command, &error)) {
    append_log(self, error->message);
    return;
  }

  command_history_push(self, command);
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
      g_autofree char *when = g_date_time_format(mtime, date_time_pattern_for_config(self));
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
    if (is_auto_poll_noise_line(line)) {
      line = strtok_r(NULL, "\n", &saveptr);
      continue;
    }

    ConsoleLevel level = CONSOLE_LEVEL_OTHER;
    g_autofree char *display = format_console_line(self, line, &level);
    if (display == NULL || *display == '\0') {
      line = strtok_r(NULL, "\n", &saveptr);
      continue;
    }

    if (console_level_matches_log_filter(level, level_index) &&
        log_line_matches_level(display, level_index)) {
      gtk_text_buffer_insert(buffer, &end, display, -1);
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
on_clear_all_logs_response(AdwAlertDialog *dialog, const char *response, PumpkinWindow *self)
{
  (void)dialog;
  if (self == NULL || g_strcmp0(response, "clear") != 0 || self->current == NULL) {
    return;
  }

  g_autofree char *logs_dir = pumpkin_server_get_logs_dir(self->current);
  GDir *dir = g_dir_open(logs_dir, 0, NULL);
  if (dir == NULL) {
    set_details_status(self, "No log folder found", 3);
    return;
  }

  guint removed = 0;
  guint64 removed_bytes = 0;
  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *path = g_build_filename(logs_dir, entry, NULL);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
      continue;
    }
    GStatBuf st;
    if (g_stat(path, &st) == 0 && st.st_size > 0) {
      removed_bytes += (guint64)st.st_size;
    }
    if (g_remove(path) == 0) {
      removed++;
    }
  }
  g_dir_close(dir);

  if (self->current_log_path != NULL && !g_file_test(self->current_log_path, G_FILE_TEST_EXISTS)) {
    g_clear_pointer(&self->current_log_path, g_free);
    if (self->log_file_view != NULL) {
      GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_file_view);
      gtk_text_buffer_set_text(buffer, "", -1);
    }
  }

  refresh_log_files(self);

  if (removed == 0) {
    set_details_status(self, "No log files removed", 3);
    return;
  }

  g_autofree char *size_label = g_format_size_full(removed_bytes, G_FORMAT_SIZE_IEC_UNITS);
  g_autofree char *status = g_strdup_printf("Removed %u log files (%s)", removed, size_label);
  set_details_status(self, status, 4);
}

static void
on_clear_all_logs(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self == NULL || self->current == NULL) {
    return;
  }

  AdwDialog *dialog = adw_alert_dialog_new(
    "Clear All Logs?",
    "This deletes all log files in the selected server logs folder.");
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_response(alert, "cancel", "Cancel");
  adw_alert_dialog_add_response(alert, "clear", "Clear All Logs");
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  adw_alert_dialog_set_response_appearance(alert, "clear", ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect(dialog, "response", G_CALLBACK(on_clear_all_logs_response), self);
  adw_dialog_present(dialog, GTK_WIDGET(self));
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
save_settings_impl(PumpkinWindow *self, gboolean restart_server)
{
  gboolean restart_after_save = FALSE;

  if (self->config != NULL) {
    if (self->switch_use_cache != NULL) {
      pumpkin_config_set_use_cache(self->config, gtk_switch_get_active(self->switch_use_cache));
    }
    if (self->switch_run_in_background != NULL) {
      pumpkin_config_set_run_in_background(self->config, gtk_switch_get_active(self->switch_run_in_background));
    }
    if (self->switch_autostart_on_boot != NULL) {
      gboolean autostart = gtk_switch_get_active(self->switch_autostart_on_boot);
      pumpkin_config_set_autostart_on_boot(self->config, autostart);
      pumpkin_config_manage_autostart_desktop(autostart);
    }
    if (self->switch_start_minimized != NULL) {
      pumpkin_config_set_start_minimized(self->config, gtk_switch_get_active(self->switch_start_minimized));
    }
    if (self->switch_auto_start_servers != NULL) {
      pumpkin_config_set_auto_start_servers_enabled(self->config, gtk_switch_get_active(self->switch_auto_start_servers));
    }
    /* Save per-server auto-start settings from list */
    if (self->autostart_server_list != NULL && self->store != NULL) {
      GListModel *model = pumpkin_server_store_get_model(self->store);
      guint n = g_list_model_get_n_items(model);
      for (guint i = 0; i < n; i++) {
        PumpkinServer *server = g_list_model_get_item(model, i);
        pumpkin_server_save(server, NULL);
        g_object_unref(server);
      }
    }
    pumpkin_config_save(self->config, NULL);
    if (self->switch_run_in_background != NULL) {
      GApplication *app = g_application_get_default();
      if (PUMPKIN_IS_APP(app)) {
        pumpkin_app_set_tray_enabled(PUMPKIN_APP(app),
                                     gtk_switch_get_active(self->switch_run_in_background));
      }
    }
  }

  if (restart_server && self->current != NULL && pumpkin_server_get_running(self->current)) {
    restart_after_save = TRUE;
  }

  if (self->current == NULL) {
    append_log(self, "Settings saved");
    set_details_status(self, "Settings saved", 3);
    self->settings_dirty = FALSE;
    update_save_button(self);
    return;
  }

  pumpkin_server_set_name(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_server_name)));
  pumpkin_server_set_download_url(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_download_url)));
  pumpkin_server_set_port(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_server_port))));
  pumpkin_server_set_bedrock_port(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_bedrock_port))));
  pumpkin_server_set_max_players(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_max_players))));
  pumpkin_server_set_stats_sample_msec(
    self->current,
    atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_stats_sample_msec))));
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
  if (self->switch_auto_update != NULL) {
    pumpkin_server_set_auto_update_enabled(self->current, gtk_switch_get_active(self->switch_auto_update));
  }
  if (self->switch_auto_update_schedule != NULL) {
    pumpkin_server_set_auto_update_use_schedule(self->current,
                                                gtk_switch_get_active(self->switch_auto_update_schedule));
  }
  if (self->entry_auto_update_time != NULL) {
    int hour = pumpkin_server_get_auto_update_hour(self->current);
    int minute = pumpkin_server_get_auto_update_minute(self->current);
    if (parse_clock_time_entry(self->entry_auto_update_time, &hour, &minute)) {
      pumpkin_server_set_auto_update_hour(self->current, hour);
      pumpkin_server_set_auto_update_minute(self->current, minute);
    }
  }
  pumpkin_server_set_rcon_host(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_host)));
  pumpkin_server_set_rcon_port(self->current, atoi(gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_port))));
  pumpkin_server_set_rcon_password(self->current, gtk_editable_get_text(GTK_EDITABLE(self->entry_rcon_password)));

  g_autoptr(GError) error = NULL;
  gboolean saved_ok = FALSE;
  if (!pumpkin_server_save(self->current, &error)) {
    append_log(self, error != NULL ? error->message : "Failed to save settings");
    set_details_status(self, "Failed to save settings", 4);
    update_save_button(self);
    return;
  } else {
    saved_ok = TRUE;
    if (restart_after_save) {
      append_log(self, "Settings saved, restarting server...");
      set_details_status(self, "Settings saved, restarting server...", 3);
    } else {
      append_log(self, "Settings saved");
      set_details_status(self, "Settings saved", 3);
    }
  }

  if (self->config != NULL) {
    const char *download_url = gtk_editable_get_text(GTK_EDITABLE(self->entry_download_url));
    pumpkin_config_set_default_download_url(self->config, download_url);
    pumpkin_config_save(self->config, NULL);
  }

  self->settings_dirty = FALSE;
  update_save_button(self);

  if (self->current != NULL) {
    apply_stats_sample_msec(self, pumpkin_server_get_stats_sample_msec(self->current), TRUE);
  }

  if (saved_ok && restart_after_save) {
    on_details_restart(NULL, self);
  }

  clear_list_box(self->server_list);
  load_server_list(self);
  update_overview(self);
}

static void
on_restart_required_save_confirmed(AdwAlertDialog *dialog, const char *response, PumpkinWindow *self)
{
  (void)dialog;
  if (self == NULL) {
    return;
  }

  if (g_strcmp0(response, "discard") == 0) {
    discard_settings_changes(self);
  } else if (g_strcmp0(response, "save_restart") == 0) {
    save_settings_impl(self, TRUE);
  } else {
    return;
  }

  if (!self->settings_dirty) {
    complete_pending_settings_navigation(self);
  }
}

static gboolean
server_settings_require_restart(PumpkinWindow *self)
{
  if (self == NULL || self->current == NULL) {
    return FALSE;
  }

  PumpkinServer *server = self->current;
  int sys_cores = 0;
  int sys_ram_mb = 0;
  get_system_limits(&sys_cores, &sys_ram_mb);

  if (!entry_matches_int(self->entry_server_port, pumpkin_server_get_port(server))) {
    return TRUE;
  }
  if (!entry_matches_int(self->entry_bedrock_port, pumpkin_server_get_bedrock_port(server))) {
    return TRUE;
  }
  if (!entry_matches_int(self->entry_max_players, pumpkin_server_get_max_players(server))) {
    return TRUE;
  }
  if (parse_limit_entry(self->entry_max_cpu_cores, sys_cores) != pumpkin_server_get_max_cpu_cores(server)) {
    return TRUE;
  }
  if (parse_limit_entry(self->entry_max_ram_mb, sys_ram_mb) != pumpkin_server_get_max_ram_mb(server)) {
    return TRUE;
  }
  if (!entry_matches_string(self->entry_rcon_host, pumpkin_server_get_rcon_host(server))) {
    return TRUE;
  }
  if (!entry_matches_int(self->entry_rcon_port, pumpkin_server_get_rcon_port(server))) {
    return TRUE;
  }
  GtkEntry *rcon_password_entry = self->entry_rcon_password != NULL ? GTK_ENTRY(self->entry_rcon_password) : NULL;
  if (!entry_matches_string(rcon_password_entry, pumpkin_server_get_rcon_password(server))) {
    return TRUE;
  }

  return FALSE;
}

static void
on_save_settings(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->settings_invalid) {
    set_details_status(self, "Fix invalid settings first", 3);
    return;
  }

  if (self->current != NULL &&
      pumpkin_server_get_running(self->current) &&
      server_settings_require_restart(self)) {
    AdwDialog *dialog = adw_alert_dialog_new(
      "Restart Required",
      "These changes require a server restart. Discard changes or save and restart now?");
    AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
    adw_alert_dialog_add_response(alert, "discard", "Discard");
    adw_alert_dialog_add_response(alert, "save_restart", "Save & Restart");
    adw_alert_dialog_set_default_response(alert, "save_restart");
    adw_alert_dialog_set_response_appearance(alert, "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(alert, "discard");
    g_signal_connect(dialog, "response", G_CALLBACK(on_restart_required_save_confirmed), self);
    adw_dialog_present(dialog, GTK_WIDGET(self));
    return;
  }

  save_settings_impl(self, FALSE);
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
  g_signal_connect(self->entry_command, "activate", G_CALLBACK(on_send_command), self);
  if (self->entry_command != NULL) {
    GtkEventController *entry_keys = gtk_event_controller_key_new();
    g_signal_connect(entry_keys, "key-pressed", G_CALLBACK(on_command_entry_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->entry_command), entry_keys);
  }
  g_signal_connect(self->btn_choose_icon, "clicked", G_CALLBACK(on_choose_icon), self);
  g_signal_connect(self->btn_reset_icon, "clicked", G_CALLBACK(on_reset_icon), self);
  g_signal_connect(self->player_list, "row-activated", G_CALLBACK(on_player_row_activated), self);
  if (self->player_search != NULL) {
    g_signal_connect(self->player_search, "search-changed", G_CALLBACK(on_player_search_changed), self);
  }
  if (self->btn_player_sort_last_online != NULL) {
    g_object_set_data(G_OBJECT(self->btn_player_sort_last_online), "sort-field", GINT_TO_POINTER(0));
    g_signal_connect(self->btn_player_sort_last_online, "clicked",
                     G_CALLBACK(on_player_sort_button_clicked), self);
  }
  if (self->btn_player_sort_playtime != NULL) {
    g_object_set_data(G_OBJECT(self->btn_player_sort_playtime), "sort-field", GINT_TO_POINTER(1));
    g_signal_connect(self->btn_player_sort_playtime, "clicked",
                     G_CALLBACK(on_player_sort_button_clicked), self);
  }
  if (self->btn_player_sort_first_joined != NULL) {
    g_object_set_data(G_OBJECT(self->btn_player_sort_first_joined), "sort-field", GINT_TO_POINTER(2));
    g_signal_connect(self->btn_player_sort_first_joined, "clicked",
                     G_CALLBACK(on_player_sort_button_clicked), self);
  }
  if (self->btn_player_sort_name != NULL) {
    g_object_set_data(G_OBJECT(self->btn_player_sort_name), "sort-field", GINT_TO_POINTER(3));
    g_signal_connect(self->btn_player_sort_name, "clicked",
                     G_CALLBACK(on_player_sort_button_clicked), self);
  }
  self->player_sort_field = 0;
  self->player_sort_ascending = FALSE;
  update_player_sort_buttons(self);
  if (self->whitelist_search != NULL) {
    g_signal_connect(self->whitelist_search, "search-changed",
                     G_CALLBACK(on_whitelist_search_changed), self);
  }
  if (self->banned_search != NULL) {
    g_signal_connect(self->banned_search, "search-changed",
                     G_CALLBACK(on_banned_search_changed), self);
  }
  g_signal_connect(self->btn_open_plugins, "clicked", G_CALLBACK(on_open_plugins), self);
  g_signal_connect(self->btn_open_players, "clicked", G_CALLBACK(on_open_players), self);
  g_signal_connect(self->btn_open_worlds, "clicked", G_CALLBACK(on_open_worlds), self);
  g_signal_connect(self->btn_save_settings, "clicked", G_CALLBACK(on_save_settings), self);

  /* Click-to-unfocus: clear entry focus when clicking anywhere else */
  {
    GtkGesture *click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click_gesture), GTK_PHASE_BUBBLE);
    g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_window_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click_gesture));
  }

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
  if (self->entry_stats_sample_msec != NULL) {
    g_signal_connect(self->entry_stats_sample_msec, "changed", G_CALLBACK(on_settings_changed), self);
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
  if (self->entry_auto_update_time != NULL) {
    g_signal_connect(self->entry_auto_update_time, "changed", G_CALLBACK(on_settings_changed), self);
  }
  if (self->switch_auto_restart != NULL) {
    g_signal_connect(self->switch_auto_restart, "notify::active",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->switch_auto_update != NULL) {
    g_signal_connect(self->switch_auto_update, "notify::active",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->switch_auto_update_schedule != NULL) {
    g_signal_connect(self->switch_auto_update_schedule, "notify::active",
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
  if (self->switch_autostart_on_boot != NULL) {
    g_signal_connect(self->switch_autostart_on_boot, "notify::active",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->switch_start_minimized != NULL) {
    g_signal_connect(self->switch_start_minimized, "notify::active",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->switch_auto_start_servers != NULL) {
    g_signal_connect(self->switch_auto_start_servers, "notify::active",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->drop_date_format != NULL) {
    g_signal_connect(self->drop_date_format, "notify::selected",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->drop_time_format != NULL) {
    g_signal_connect(self->drop_time_format, "notify::selected",
                     G_CALLBACK(on_settings_switch_changed), self);
  }
  if (self->btn_add_autostart_server != NULL) {
    g_signal_connect(self->btn_add_autostart_server, "clicked",
                     G_CALLBACK(on_add_autostart_server), self);
  }
  if (self->btn_save_general_settings != NULL) {
    g_signal_connect(self->btn_save_general_settings, "clicked",
                     G_CALLBACK(on_save_general_settings), self);
  }
  if (self->btn_reset_general_settings != NULL) {
    g_signal_connect(self->btn_reset_general_settings, "clicked",
                     G_CALLBACK(on_reset_general_settings), self);
  }
  if (self->btn_reset_server_settings != NULL) {
    g_signal_connect(self->btn_reset_server_settings, "clicked",
                     G_CALLBACK(on_reset_server_settings), self);
  }
  if (self->btn_clear_cache != NULL) {
    g_signal_connect(self->btn_clear_cache, "clicked",
                     G_CALLBACK(on_clear_cache), self);
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
  if (self->btn_clear_all_logs != NULL) {
    g_signal_connect(self->btn_clear_all_logs, "clicked", G_CALLBACK(on_clear_all_logs), self);
  }
  if (self->btn_console_copy != NULL) {
    g_signal_connect(self->btn_console_copy, "clicked", G_CALLBACK(on_console_copy), self);
  }
  if (self->btn_console_send != NULL) {
    g_signal_connect(self->btn_console_send, "clicked", G_CALLBACK(on_send_command), self);
  }
  if (self->btn_console_clear != NULL) {
    g_signal_connect(self->btn_console_clear, "clicked", G_CALLBACK(on_console_clear), self);
  }
  if (self->btn_console_filter_all != NULL) {
    g_signal_connect(self->btn_console_filter_all, "clicked",
                     G_CALLBACK(on_console_filter_all_clicked), self);
  }
  if (self->check_console_trace != NULL) {
    g_signal_connect(self->check_console_trace, "toggled", G_CALLBACK(on_console_filter_toggled), self);
  }
  if (self->check_console_debug != NULL) {
    g_signal_connect(self->check_console_debug, "toggled", G_CALLBACK(on_console_filter_toggled), self);
  }
  if (self->check_console_info != NULL) {
    g_signal_connect(self->check_console_info, "toggled", G_CALLBACK(on_console_filter_toggled), self);
  }
  if (self->check_console_warn != NULL) {
    g_signal_connect(self->check_console_warn, "toggled", G_CALLBACK(on_console_filter_toggled), self);
  }
  if (self->check_console_error != NULL) {
    g_signal_connect(self->check_console_error, "toggled", G_CALLBACK(on_console_filter_toggled), self);
  }
  if (self->check_console_smpk != NULL) {
    g_signal_connect(self->check_console_smpk, "toggled", G_CALLBACK(on_console_filter_toggled), self);
  }
  if (self->check_console_other != NULL) {
    g_signal_connect(self->check_console_other, "toggled", G_CALLBACK(on_console_filter_toggled), self);
  }

  self->config = pumpkin_config_load(NULL);
  self->live_player_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->platform_hint_by_ip = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->player_states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)player_state_free);
  self->player_states_by_uuid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->player_states_by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->deleted_player_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->player_head_downloads = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->player_state_dirty = FALSE;
  self->last_player_state_flush_at = g_get_monotonic_time();
  self->console_buffers = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, g_object_unref);
  self->command_history = g_ptr_array_new_with_free_func(g_free);
  self->command_history_index = -1;
  self->command_history_draft = NULL;
  self->download_progress_state = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                        g_object_unref, (GDestroyNotify)download_progress_state_free);
  apply_console_filters(self);
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
    if (self->switch_autostart_on_boot != NULL) {
      gtk_switch_set_active(self->switch_autostart_on_boot,
                            pumpkin_config_get_autostart_on_boot(self->config));
    }
    if (self->switch_start_minimized != NULL) {
      gtk_switch_set_active(self->switch_start_minimized,
                            pumpkin_config_get_start_minimized(self->config));
    }
    if (self->switch_auto_start_servers != NULL) {
      gtk_switch_set_active(self->switch_auto_start_servers,
                            pumpkin_config_get_auto_start_servers_enabled(self->config));
    }
    if (self->drop_date_format != NULL) {
      gtk_drop_down_set_selected(self->drop_date_format, (guint)pumpkin_config_get_date_format(self->config));
    }
    if (self->drop_time_format != NULL) {
      gtk_drop_down_set_selected(self->drop_time_format, (guint)pumpkin_config_get_time_format(self->config));
    }
    /* Populate autostart server list */
    populate_autostart_server_list(self);
    update_autostart_sensitivity(self);
  }
  update_auto_update_controls_sensitivity(self);

  update_check_updates_badge(self);
  trigger_latest_resolve(self);
  if (self->latest_poll_id == 0) {
    self->latest_poll_id = g_timeout_add_seconds(60, poll_latest_release_tick, self);
  }

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
  self->stats_sample_msec = DEFAULT_STATS_SAMPLE_MSEC;
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
  restart_stats_refresh_timer(self);
}

static void
pumpkin_window_dispose(GObject *object)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(object);
  if (self->current != NULL) {
    player_states_mark_all_offline(self);
    player_states_save(self, self->current);
  }
  if (self->config != NULL) {
    pumpkin_config_free(self->config);
    self->config = NULL;
  }
  if (self->live_player_names != NULL) {
    g_hash_table_destroy(self->live_player_names);
    self->live_player_names = NULL;
  }
  if (self->platform_hint_by_ip != NULL) {
    g_hash_table_destroy(self->platform_hint_by_ip);
    self->platform_hint_by_ip = NULL;
  }
  if (self->console_buffers != NULL) {
    g_hash_table_destroy(self->console_buffers);
    self->console_buffers = NULL;
  }
  if (self->command_history != NULL) {
    g_ptr_array_unref(self->command_history);
    self->command_history = NULL;
  }
  self->command_history_index = -1;
  g_clear_pointer(&self->command_history_draft, g_free);
  if (self->download_progress_state != NULL) {
    g_hash_table_destroy(self->download_progress_state);
    self->download_progress_state = NULL;
  }
  if (self->player_states != NULL) {
    g_hash_table_destroy(self->player_states);
    self->player_states = NULL;
  }
  if (self->player_states_by_uuid != NULL) {
    g_hash_table_destroy(self->player_states_by_uuid);
    self->player_states_by_uuid = NULL;
  }
  if (self->player_states_by_name != NULL) {
    g_hash_table_destroy(self->player_states_by_name);
    self->player_states_by_name = NULL;
  }
  if (self->deleted_player_keys != NULL) {
    g_hash_table_destroy(self->deleted_player_keys);
    self->deleted_player_keys = NULL;
  }
  if (self->player_head_downloads != NULL) {
    g_hash_table_destroy(self->player_head_downloads);
    self->player_head_downloads = NULL;
  }
  g_clear_pointer(&self->pending_details_page, g_free);
  g_clear_pointer(&self->pending_view_page, g_free);
  g_clear_pointer(&self->current_log_path, g_free);
  g_clear_pointer(&self->last_details_page, g_free);
  g_clear_pointer(&self->latest_url, g_free);
  g_clear_pointer(&self->latest_build_id, g_free);
  g_clear_pointer(&self->latest_build_label, g_free);
  g_clear_pointer(&self->auto_update_last_schedule_server_id, g_free);
  g_clear_pointer(&self->auto_update_last_attempt_server_id, g_free);
  g_clear_pointer(&self->auto_update_last_attempt_build_id, g_free);
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
  if (self->latest_poll_id != 0) {
    g_source_remove(self->latest_poll_id);
    self->latest_poll_id = 0;
  }
  clear_auto_update_countdown(self);
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
  if (self == NULL || self->live_player_names == NULL || self->player_states == NULL || line == NULL) {
    return;
  }

  g_autofree char *clean = strip_ansi(line);
  const char *check = clean != NULL ? clean : line;
  PlayerPlatform platform_hint = platform_from_line(check);

  g_autoptr(GRegex) accepted_java_re =
    g_regex_new("accepted\\s+connection\\s+from\\s+java\\s+edition:\\s*([^\\s]+)",
                G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) accepted_java_match = NULL;
  if (g_regex_match(accepted_java_re, check, 0, &accepted_java_match)) {
    g_autofree char *addr = g_match_info_fetch(accepted_java_match, 1);
    g_autofree char *ip = extract_ip_from_socket_text(addr);
    remember_platform_hint_for_ip(self, ip, PLAYER_PLATFORM_JAVA);
    if (self->pending_java_platform_hints < 1024) {
      self->pending_java_platform_hints++;
    }
    return;
  }

  g_autoptr(GRegex) accepted_bedrock_re =
    g_regex_new("accepted\\s+connection\\s+from\\s+bedrock\\s+edition:\\s*([^\\s]+)|\\bbedrock\\b.*\\bfrom\\s+([^\\s]+)",
                G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) accepted_bedrock_match = NULL;
  if (g_regex_match(accepted_bedrock_re, check, 0, &accepted_bedrock_match)) {
    g_autofree char *addr1 = g_match_info_fetch(accepted_bedrock_match, 1);
    g_autofree char *addr2 = g_match_info_fetch(accepted_bedrock_match, 2);
    const char *addr = (addr1 != NULL && *addr1 != '\0') ? addr1 : addr2;
    g_autofree char *ip = extract_ip_from_socket_text(addr);
    remember_platform_hint_for_ip(self, ip, PLAYER_PLATFORM_BEDROCK);
    if (self->pending_bedrock_platform_hints < 1024) {
      self->pending_bedrock_platform_hints++;
    }
    return;
  }

  g_autofree char *lower_check = g_ascii_strdown(check, -1);
  if (lower_check != NULL &&
      strstr(lower_check, "bedrock") != NULL &&
      (strstr(lower_check, "status_have_all_packs") != NULL ||
       strstr(lower_check, "login") != NULL)) {
    if (self->pending_bedrock_platform_hints < 1024) {
      self->pending_bedrock_platform_hints++;
    }
  }

  g_autoptr(GRegex) login_addr_re =
    g_regex_new("(?:^|\\s|:\\s*)([^\\s\\[]+)\\[/([^\\]]+)\\]\\s+logged\\s+in\\b",
                G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) login_addr_match = NULL;
  if (g_regex_match(login_addr_re, check, 0, &login_addr_match)) {
    g_autofree char *name = g_match_info_fetch(login_addr_match, 1);
    g_autofree char *addr = g_match_info_fetch(login_addr_match, 2);
    if (name != NULL) {
      g_strstrip(name);
    }
    g_autofree char *ip = extract_ip_from_socket_text(addr);
    if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
      platform_hint = platform_hint_for_ip(self, ip);
    }
    if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
      platform_hint = take_pending_platform_hint(self);
    }
    if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
      platform_hint = PLAYER_PLATFORM_JAVA;
    }
    if (name != NULL && *name != '\0') {
      allow_deleted_player_tracking(self, NULL, name);
      PlayerState *state = ensure_player_state(self, NULL, name, TRUE);
      if (state != NULL && ip != NULL && *ip != '\0' && g_strcmp0(state->last_ip, ip) != 0) {
        g_free(state->last_ip);
        state->last_ip = g_strdup(ip);
        player_states_set_dirty(self);
      }
      player_state_mark_online(self, state, platform_hint);
      return;
    }
  }

  if (is_player_list_snapshot_line(check)) {
    g_autoptr(GHashTable) present = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int count_value = -1;
    g_autofree char *names_csv = NULL;
    gboolean parsed_snapshot = parse_player_list_snapshot_line(check, &count_value, &names_csv);

    if (!parsed_snapshot) {
      g_autoptr(GRegex) count_re = g_regex_new("there\\s+are\\s+([0-9]+)", G_REGEX_CASELESS, 0, NULL);
      g_autoptr(GMatchInfo) count_match = NULL;
      if (g_regex_match(count_re, check, 0, &count_match)) {
        g_autofree char *count_txt = g_match_info_fetch(count_match, 1);
        if (count_txt != NULL) {
          count_value = (int)strtol(count_txt, NULL, 10);
        }
      }
    }

    gboolean has_name_list = names_csv != NULL && *names_csv != '\0';
    if (has_name_list) {
      g_auto(GStrv) split = g_strsplit(names_csv, ",", -1);
      for (int i = 0; split[i] != NULL; i++) {
        g_autofree char *name = g_strdup(split[i]);
        g_strstrip(name);
        if (name[0] == '\0') {
          continue;
        }
        g_autofree char *name_key = normalized_key(name);
        if (name_key == NULL) {
          continue;
        }
        g_hash_table_add(present, g_strdup(name_key));
        allow_deleted_player_tracking(self, NULL, name);
        PlayerState *state = ensure_player_state(self, NULL, name, TRUE);
        PlayerPlatform state_platform = platform_hint;
        if (state_platform == PLAYER_PLATFORM_UNKNOWN) {
          state_platform = take_pending_platform_hint(self);
        }
        if (state_platform == PLAYER_PLATFORM_UNKNOWN) {
          state_platform = PLAYER_PLATFORM_JAVA;
        }
        player_state_mark_online(self, state, state_platform);
      }
    }

    if (count_value == 0 || has_name_list) {
      GHashTableIter iter;
      gpointer key = NULL;
      gpointer value = NULL;
      g_hash_table_iter_init(&iter, self->player_states);
      while (g_hash_table_iter_next(&iter, &key, &value)) {
        PlayerState *state = value;
        if (state == NULL || !state->online) {
          continue;
        }
        g_autofree char *name_key = normalized_key(state->name);
        if (name_key == NULL || !g_hash_table_contains(present, name_key)) {
          player_state_mark_offline(self, state);
        }
      }
    }
    return;
  }

  if (strstr(check, "UUID: ") != NULL) {
    const char *uuid_pos = strstr(check, "UUID: ");
    const char *name_pos = strstr(check, "name=");
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
        if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
          platform_hint = platform_guess_from_uuid(uuid);
        }
        if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
          platform_hint = take_pending_platform_hint(self);
        }
        if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
          platform_hint = PLAYER_PLATFORM_JAVA;
        }
        allow_deleted_player_tracking(self, uuid, name);
        PlayerState *state = ensure_player_state(self, uuid, name, TRUE);
        player_state_mark_online(self, state, platform_hint);
        return;
      }
    }
  }

  g_autofree char *joined_name = extract_name_before_suffix(check, " joined the game");
  if (joined_name != NULL && *joined_name != '\0') {
    if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
      platform_hint = take_pending_platform_hint(self);
    }
    if (platform_hint == PLAYER_PLATFORM_UNKNOWN) {
      platform_hint = PLAYER_PLATFORM_JAVA;
    }
    allow_deleted_player_tracking(self, NULL, joined_name);
    PlayerState *state = ensure_player_state(self, NULL, joined_name, TRUE);
    player_state_mark_online(self, state, platform_hint);
    return;
  }

  g_autofree char *left_name = extract_name_before_suffix(check, " left the game");
  if (left_name != NULL && *left_name != '\0') {
    PlayerState *state = ensure_player_state(self, NULL, left_name, FALSE);
    if (state != NULL) {
      player_state_mark_offline(self, state);
    } else {
      g_hash_table_remove(self->live_player_names, left_name);
    }
    return;
  }

  /* Do not infer players from generic chat-like "<name>" fragments.
   * It can match command or diagnostic lines and create fake players. */
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
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, player_search);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_player_sort_last_online);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_player_sort_playtime);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_player_sort_first_joined);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_player_sort_name);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, whitelist_search);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, banned_search);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, whitelist_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, banned_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_files_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_file_view);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_filter);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_level_filter);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, log_search);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_open_logs);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_clear_all_logs);
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
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_console_send);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_console_copy);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_console_clear);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_console_filter);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_console_filter_all);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, check_console_trace);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, check_console_debug);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, check_console_info);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, check_console_warn);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, check_console_error);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, check_console_smpk);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, check_console_other);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_open_server_root);

  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_server_name);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_download_url);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_choose_icon);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_reset_icon);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_server_port);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_bedrock_port);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_max_players);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_stats_sample_msec);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_max_cpu_cores);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_max_ram_mb);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_java_port_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_bedrock_port_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_max_players_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_stats_sample_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_max_cpu_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_max_ram_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_auto_restart);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_auto_restart_delay);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_auto_update);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_auto_update_schedule);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_auto_update_time);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_auto_update_time_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_host);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_port);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, entry_rcon_password);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_rcon_host_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_rcon_port_hint);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, label_resource_limits);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_use_cache);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_run_in_background);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_autostart_on_boot);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_start_minimized);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, switch_auto_start_servers);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, drop_date_format);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, drop_time_format);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, autostart_server_list);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_add_autostart_server);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_save_general_settings);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_reset_general_settings);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_reset_server_settings);
  gtk_widget_class_bind_template_child(widget_class, PumpkinWindow, btn_clear_cache);
}

GtkWindow *
pumpkin_window_new(AdwApplication *app)
{
  return GTK_WINDOW(g_object_new(PUMPKIN_TYPE_WINDOW, "application", app, NULL));
}

void
pumpkin_window_stop_all_servers(PumpkinWindow *self)
{
  if (self == NULL || self->store == NULL) {
    return;
  }
  if (self->auto_update_server != NULL) {
    clear_auto_update_countdown(self);
  }

  GListModel *model = pumpkin_server_store_get_model(self->store);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(model, i);
    if (server == NULL) {
      continue;
    }
    if (pumpkin_server_get_running(server)) {
      if (server == self->current) {
        self->user_stop_requested = TRUE;
      }
      pumpkin_server_stop(server);
    }
    g_object_unref(server);
  }
}

void
pumpkin_window_select_server(PumpkinWindow *self, const char *id_or_name)
{
  if (self == NULL || id_or_name == NULL || *id_or_name == '\0') {
    return;
  }
  if (self->store == NULL) {
    return;
  }

  GListModel *model = pumpkin_server_store_get_model(self->store);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(model, i);
    if (server == NULL) {
      continue;
    }
    const char *sid = pumpkin_server_get_id(server);
    const char *sname = pumpkin_server_get_name(server);
    gboolean match = (sid != NULL && g_strcmp0(sid, id_or_name) == 0) ||
                     (sname != NULL && g_strcmp0(sname, id_or_name) == 0);
    if (match) {
      select_server_row(self, server);
      g_object_unref(server);
      return;
    }
    g_object_unref(server);
  }
}
