#pragma once

#include "window.h"
#include "app-config.h"
#include "server-store.h"

#define DEFAULT_STATS_SAMPLE_MSEC 200
#define STATS_SAMPLE_MSEC_MIN 2
#define STATS_SAMPLE_MSEC_MAX 2000
#define STATS_HISTORY_SECONDS 180
#define STATS_SAMPLES ((STATS_HISTORY_SECONDS * 1000) / DEFAULT_STATS_SAMPLE_MSEC)
#define PLAYER_STATE_FLUSH_INTERVAL_USEC (15 * G_USEC_PER_SEC)
#define CONSOLE_MAX_LINES 5000
#define NETWORK_PROXY_JAVA_PORT 25565
#define NETWORK_PROXY_BEDROCK_PORT 19132
#define NETWORK_PROXY_RCON_PORT 25575
#define MIN_VALID_PUMPKIN_BINARY_BYTES (512 * 1024)
#define DDNS_TICK_SECONDS 30
#define DDNS_INTERVAL_SECONDS_MIN 30
#define DDNS_INTERVAL_SECONDS_MAX 86400
#define DDNS_INTERVAL_SECONDS_DEFAULT 300
#define DDNS_API_IPIFY_URL "https://api.ipify.org?format=text"
#define DDNS_API_CLOUDFLARE_BASE "https://api.cloudflare.com/client/v4"

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
  goffset current;
  goffset total;
  gboolean active;
  GtkWidget *overview_bar;
} DownloadProgressState;

typedef struct {
  PumpkinWindow *self;
  PumpkinServer *server;
  char *url;
  char *build_id;
  char *build_label;
  char *cache_path;
  gboolean restart_after_download;
  guint attempts;
} CacheWaitContext;

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
  gboolean ip_banned_hint;
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
  gint64 now_unix;
} PlayerSortSettings;

typedef struct {
  PumpkinWindow *self;
  PumpkinServer *server;
  char *uuid_key;
  char *cache_path;
} PlayerHeadDownloadContext;

typedef struct {
  char *id;
  char *name;
  char *icon_name;
  char *proxy_server_id;
  GPtrArray *member_server_ids;
} ServerNetwork;

typedef struct {
  char *server_id;
  GtkCheckButton *check;
} NetworkServerChoice;

typedef struct {
  char *network_id;
  GPtrArray *choices;
  AdwDialog *owner_dialog;
} NetworkProxyDialogContext;

typedef struct {
  PumpkinWindow *self;
  PumpkinServer *server;
} AutoStartContext;

typedef struct {
  char *server_id;
  char *server_name;
  char *domain;
  char *provider;
  char *api_token;
  char *zone_id;
  char *record_id;
  gboolean proxied;
} DdnsServerConfig;

typedef struct {
  char *server_id;
  gboolean success;
  gboolean changed;
  char *record_id;
  char *message;
} DdnsServerResult;

typedef struct {
  GPtrArray *servers;
} DdnsSyncRequest;

typedef struct {
  char *public_ipv4;
  GPtrArray *results;
} DdnsSyncResult;

char *normalized_key(const char *text);
void invalidate_player_list_signature(PumpkinWindow *self);

struct _PumpkinWindow {
  AdwApplicationWindow parent_instance;

  AdwViewStack *view_stack;
  AdwViewStack *details_stack;
  AdwViewSwitcher *details_switcher;
  AdwViewSwitcher *players_switcher;
  AdwViewStack *players_stack;
  GtkBox *overview_page_box;
  GtkBox *overview_header_row;
  GtkBox *details_page_box;
  GtkBox *details_header_row;
  GtkBox *details_identity_row;
  GtkBox *console_command_row;
  GtkBox *servers_page_box;
  GtkBox *servers_header_row;
  GtkBox *domains_page_box;

  GtkListBox *server_list;
  GtkTextView *log_view;
  GtkListBox *overview_list;
  GtkListBox *overview_network_list;
  char *latest_url;
  char *latest_build_id;
  char *latest_build_label;
  guint latest_poll_id;
  gboolean latest_resolve_in_flight;

  GtkButton *btn_add_server;
  GtkButton *btn_remove_server;
  GtkButton *btn_add_server_overview;
  GtkButton *btn_add_network_overview;
  GtkButton *btn_servers_back;
  GtkButton *btn_import_server;
  GtkButton *btn_import_server_overview;
  GtkButton *btn_sponsor_pumpkin;
  GtkLabel *label_sponsor_heart;
  GtkButton *btn_open_plugins;
  GtkButton *btn_plugins_marketplace;
  GtkButton *btn_open_players;
  GtkButton *btn_open_worlds;
  GtkButton *btn_save_settings;

  GtkListBox *plugin_list;
  GtkListBox *world_list;
  GtkListBox *player_list;
  GtkSearchEntry *player_search;
  GtkDropDown *player_sort_dropdown;
  GtkToggleButton *btn_player_sort_order;
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
  GtkBox *details_action_row;
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
  guint console_scroll_idle_id;
  guint log_file_scroll_idle_id;
  guint auto_update_countdown_id;
  GHashTable *download_progress_state;
  gboolean close_while_download_confirmed;
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
  double last_proc_cpu_smoothed;
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
  int list_snapshot_players;
  int list_snapshot_max_players;
  gint64 list_snapshot_updated_at;
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
  GtkEntry *entry_domain;
  GtkLabel *label_domains_target;
  GtkButton *btn_save_domains;
  GtkSwitch *switch_ddns_enabled;
  GtkEntry *entry_ddns_cf_zone_id;
  GtkPasswordEntry *entry_ddns_cf_api_token;
  GtkEntry *entry_ddns_cf_record_id;
  GtkSwitch *switch_ddns_cf_proxied;
  GtkEntry *entry_ddns_interval_seconds;
  GtkButton *btn_ddns_sync_now;
  GtkLabel *label_ddns_status;
  GtkSwitch *switch_use_cache;
  GtkSwitch *switch_run_in_background;
  GtkSwitch *switch_detailed_overview_cards;
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
  gboolean player_list_pointer_inside;
  gint64 player_list_interaction_until;
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
  GHashTable *server_running_hints;
  GPtrArray *command_history;
  int command_history_index;
  char *command_history_draft;
  GPtrArray *networks;
  GHashTable *expanded_network_ids;
  AdwDialog *network_details_dialog;
  GtkBox *network_details_members_box;
  GtkProgressBar *network_details_progress;
  char *network_details_network_id;
  guint overview_refresh_idle_id;
  gboolean overview_refresh_needs_details;
  guint network_details_refresh_idle_id;
  gint64 suppress_warning_beep_until;
  guint ddns_timer_id;
  gboolean ddns_sync_in_flight;
  GHashTable *ddns_last_sync_by_server;
  GHashTable *ddns_status_by_server;
};

DownloadProgressState *get_download_progress_state(PumpkinWindow *self, PumpkinServer *server, gboolean create);
gboolean server_has_installation(PumpkinWindow *self, PumpkinServer *server);
void ensure_server_log_handler(PumpkinWindow *self, PumpkinServer *server);
void append_log(PumpkinWindow *self, const char *line);
void append_log_for_server(PumpkinWindow *self, PumpkinServer *server, const char *line);
void set_details_error(PumpkinWindow *self, const char *message);
void clear_auto_update_countdown(PumpkinWindow *self);
void queue_overview_refresh(PumpkinWindow *self, gboolean include_details);
gboolean start_after_delay(gpointer data);
void set_console_warning(PumpkinWindow *self, const char *message, gboolean visible);
GListModel *get_server_model(PumpkinWindow *self);
char *normalized_key(const char *text);
void update_settings_form(PumpkinWindow *self);
