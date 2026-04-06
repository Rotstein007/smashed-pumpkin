#define _GNU_SOURCE
#include "server.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#if defined(G_OS_WIN32)
#include <gio-win32-2.0/gio/gwin32inputstream.h>
#include <gio-win32-2.0/gio/gwin32outputstream.h>
#include <windows.h>
#else
#include <sys/resource.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <sched.h>
#include <sys/prctl.h>
#endif
#endif

enum {
  SERVER_STATS_SAMPLE_MSEC_DEFAULT = 200,
  SERVER_STATS_SAMPLE_MSEC_MIN = 2,
  SERVER_STATS_SAMPLE_MSEC_MAX = 2000,
  SERVER_DDNS_INTERVAL_SECONDS_DEFAULT = 300,
  SERVER_DDNS_INTERVAL_SECONDS_MIN = 30,
  SERVER_DDNS_INTERVAL_SECONDS_MAX = 86400
};

static const char *
default_download_url(void)
{
#if defined(G_OS_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
  return "https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-ARM64-Windows.exe";
#else
  return "https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-X64-Windows.exe";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-ARM64-macOS";
#else
  return "https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-X64-macOS";
#endif
#else
#if defined(__aarch64__) || defined(__arm64__)
  return "https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-ARM64-Linux";
#else
  return "https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-X64-Linux";
#endif
#endif
}

struct _PumpkinServer {
  GObject parent_instance;

  char *id;
  char *name;
  char *root_dir;
  char *download_url;
  char *installed_url;
  char *installed_build_id;
  char *installed_build_label;
  char *rcon_host;
  char *rcon_password;
  char *domain;
  gboolean ddns_enabled;
  char *ddns_provider;
  char *ddns_cf_api_token;
  char *ddns_cf_zone_id;
  char *ddns_cf_record_id;
  gboolean ddns_cf_proxied;
  gboolean ddns_update_ipv6;
  int ddns_interval_seconds;
  int rcon_port;
  int port;
  int bedrock_port;
  int max_players;
  int max_cpu_cores;
  int max_ram_mb;
  int stats_sample_msec;
  gboolean auto_restart;
  int auto_restart_delay;
  gboolean auto_update_enabled;
  gboolean auto_update_use_schedule;
  int auto_update_hour;
  int auto_update_minute;
  gboolean auto_start_on_launch;
  int auto_start_delay;
  gboolean stop_requested;
  guint restart_source_id;
#if defined(G_OS_WIN32)
  HANDLE job_handle;
  HANDLE process_handle;
  guint process_watch_source_id;
#endif

  GSubprocess *process;
  GDataInputStream *stdout_dis;
  GDataInputStream *stderr_dis;
  GOutputStream *stdin_stream;
  GOutputStream *log_stream;
  char *log_path;
  int pid;
};

G_DEFINE_FINAL_TYPE(PumpkinServer, pumpkin_server, G_TYPE_OBJECT)

enum {
  LOG_LINE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void
pumpkin_server_finalize(GObject *object)
{
  PumpkinServer *self = PUMPKIN_SERVER(object);

  g_clear_pointer(&self->id, g_free);
  g_clear_pointer(&self->name, g_free);
  g_clear_pointer(&self->root_dir, g_free);
  g_clear_pointer(&self->download_url, g_free);
  g_clear_pointer(&self->installed_url, g_free);
  g_clear_pointer(&self->installed_build_id, g_free);
  g_clear_pointer(&self->installed_build_label, g_free);
  g_clear_pointer(&self->rcon_host, g_free);
  g_clear_pointer(&self->rcon_password, g_free);
  g_clear_pointer(&self->domain, g_free);
  g_clear_pointer(&self->ddns_provider, g_free);
  g_clear_pointer(&self->ddns_cf_api_token, g_free);
  g_clear_pointer(&self->ddns_cf_zone_id, g_free);
  g_clear_pointer(&self->ddns_cf_record_id, g_free);
  g_clear_object(&self->process);
  g_clear_object(&self->stdout_dis);
  g_clear_object(&self->stderr_dis);
  g_clear_object(&self->log_stream);
  g_clear_pointer(&self->log_path, g_free);
#if defined(G_OS_WIN32)
  if (self->job_handle != NULL) {
    CloseHandle(self->job_handle);
    self->job_handle = NULL;
  }
  if (self->process_handle != NULL) {
    CloseHandle(self->process_handle);
    self->process_handle = NULL;
  }
  if (self->process_watch_source_id != 0) {
    g_source_remove(self->process_watch_source_id);
    self->process_watch_source_id = 0;
  }
#endif
  if (self->restart_source_id != 0) {
    g_source_remove(self->restart_source_id);
    self->restart_source_id = 0;
  }

  G_OBJECT_CLASS(pumpkin_server_parent_class)->finalize(object);
}

static void
pumpkin_server_class_init(PumpkinServerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS(class);

  object_class->finalize = pumpkin_server_finalize;

  signals[LOG_LINE] = g_signal_new(
    "log-line",
    G_TYPE_FROM_CLASS(class),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__STRING,
    G_TYPE_NONE,
    1,
    G_TYPE_STRING
  );
}

static void
pumpkin_server_init(PumpkinServer *self)
{
  self->rcon_port = 25575;
  self->port = 25565;
  self->bedrock_port = 19132;
  self->max_players = 20;
  self->max_cpu_cores = 0;
  self->max_ram_mb = 0;
  self->stats_sample_msec = SERVER_STATS_SAMPLE_MSEC_DEFAULT;
  self->auto_restart = FALSE;
  self->auto_restart_delay = 10000;
  self->auto_update_enabled = FALSE;
  self->auto_update_use_schedule = FALSE;
  self->auto_update_hour = 1;
  self->auto_update_minute = 0;
  self->auto_start_on_launch = FALSE;
  self->auto_start_delay = 10;
  self->ddns_enabled = FALSE;
  self->ddns_provider = g_strdup("cloudflare");
  self->ddns_cf_api_token = NULL;
  self->ddns_cf_zone_id = NULL;
  self->ddns_cf_record_id = NULL;
  self->ddns_cf_proxied = FALSE;
  self->ddns_update_ipv6 = FALSE;
  self->ddns_interval_seconds = SERVER_DDNS_INTERVAL_SECONDS_DEFAULT;
  self->pid = 0;
#if defined(G_OS_WIN32)
  self->process_handle = NULL;
  self->process_watch_source_id = 0;
#endif
}

static int
get_system_max_cores(void)
{
  int cores = g_get_num_processors();
  return cores > 0 ? cores : 0;
}

static int
get_system_max_ram_mb(void)
{
#if defined(G_OS_WIN32)
  MEMORYSTATUSEX mem = { 0 };
  mem.dwLength = sizeof(mem);
  if (!GlobalMemoryStatusEx(&mem)) {
    return 0;
  }
  return (int)(mem.ullTotalPhys / (1024ULL * 1024ULL));
#elif defined(__APPLE__)
  uint64_t total = 0;
  size_t total_len = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &total_len, NULL, 0) != 0) {
    return 0;
  }
  return (int)(total / (1024ULL * 1024ULL));
#else
  g_autofree char *contents = NULL;
  if (!g_file_get_contents("/proc/meminfo", &contents, NULL, NULL)) {
    return 0;
  }
  unsigned long long total_kb = 0;
  char **lines = g_strsplit(contents, "\n", -1);
  for (int i = 0; lines[i] != NULL; i++) {
    if (g_str_has_prefix(lines[i], "MemTotal:")) {
      sscanf(lines[i], "MemTotal: %llu kB", &total_kb);
      break;
    }
  }
  g_strfreev(lines);
  if (total_kb == 0) {
    return 0;
  }
  return (int)(total_kb / 1024ULL);
#endif
}

static int
clamp_cpu_cores(int requested)
{
  if (requested <= 0) {
    return 0;
  }
  int max = get_system_max_cores();
  if (max > 0 && requested > max) {
    return max;
  }
  return requested;
}

static int
clamp_ram_mb(int requested)
{
  if (requested <= 0) {
    return 0;
  }
  int max = get_system_max_ram_mb();
  if (max > 0 && requested > max) {
    return max;
  }
  return requested;
}

static int
clamp_stats_sample_msec(int requested)
{
  if (requested < SERVER_STATS_SAMPLE_MSEC_MIN || requested > SERVER_STATS_SAMPLE_MSEC_MAX) {
    return SERVER_STATS_SAMPLE_MSEC_DEFAULT;
  }
  return requested;
}

static int
clamp_ddns_interval_seconds(int requested)
{
  if (requested < SERVER_DDNS_INTERVAL_SECONDS_MIN || requested > SERVER_DDNS_INTERVAL_SECONDS_MAX) {
    return SERVER_DDNS_INTERVAL_SECONDS_DEFAULT;
  }
  return requested;
}

#if !defined(G_OS_WIN32)
typedef struct {
  int max_cpu_cores;
  int max_ram_mb;
  int parent_pid;
} ChildLimits;

static void
child_setup_cb(gpointer data)
{
  ChildLimits *limits = data;
#if defined(__linux__) && defined(PR_SET_PDEATHSIG)
  if (limits->parent_pid > 0) {
    /* Ensure child exits when smashed-pumpkin parent dies unexpectedly. */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if ((int)getppid() != limits->parent_pid) {
      _exit(0);
    }
  }
#endif
  if (limits->max_ram_mb > 0) {
    struct rlimit lim;
    rlim_t bytes = (rlim_t)limits->max_ram_mb * 1024ULL * 1024ULL;
    lim.rlim_cur = bytes;
    lim.rlim_max = bytes;
    setrlimit(RLIMIT_AS, &lim);
  }
#if defined(__linux__)
  if (limits->max_cpu_cores > 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    int cores = limits->max_cpu_cores;
    for (int i = 0; i < cores; i++) {
      CPU_SET(i, &set);
    }
    sched_setaffinity(0, sizeof(set), &set);
  }
#endif
}
#endif

static char *
server_ini_path(const char *dir)
{
  return g_build_filename(dir, "server.ini", NULL);
}

static gboolean
ensure_dir(const char *path, GError **error);

static char *
toml_replace_or_append(const char *contents, const char *key, const char *value_literal)
{
  if (key == NULL || *key == '\0' || value_literal == NULL) {
    return g_strdup(contents != NULL ? contents : "");
  }
  g_autofree char *base = g_strdup(contents != NULL ? contents : "");
  g_autofree char *escaped_key = g_regex_escape_string(key, -1);
  g_autofree char *pattern = g_strdup_printf("(?m)^\\s*%s\\s*=\\s*[^\\n\\r]*$", escaped_key);
  g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
  g_autofree char *replacement = g_strdup_printf("%s = %s", key, value_literal);

  if (regex != NULL && g_regex_match(regex, base, 0, NULL)) {
    return g_regex_replace_literal(regex, base, -1, 0, replacement, 0, NULL);
  }

  gboolean needs_nl = base[0] != '\0' && base[strlen(base) - 1] != '\n';
  return g_strdup_printf("%s%s%s\n", base, needs_nl ? "\n" : "", replacement);
}

static char *
toml_replace_or_append_in_section(const char *contents,
                                  const char *section,
                                  const char *key,
                                  const char *value_literal)
{
  if (section == NULL || *section == '\0' || key == NULL || *key == '\0' || value_literal == NULL) {
    return g_strdup(contents != NULL ? contents : "");
  }

  g_auto(GStrv) lines = g_strsplit(contents != NULL ? contents : "", "\n", -1);
  GString *out = g_string_new(NULL);
  g_autofree char *section_header = g_strdup_printf("[%s]", section);
  g_autofree char *replacement = g_strdup_printf("%s = %s", key, value_literal);
  gboolean in_section = FALSE;
  gboolean section_found = FALSE;
  gboolean key_replaced = FALSE;

  for (guint i = 0; lines[i] != NULL; i++) {
    const char *line = lines[i];
    g_autofree char *trimmed = g_strdup(line);
    g_strstrip(trimmed);
    gboolean is_section_header = trimmed[0] == '[';

    if (in_section && is_section_header) {
      if (!key_replaced) {
        g_string_append_printf(out, "%s\n", replacement);
        key_replaced = TRUE;
      }
      in_section = FALSE;
    }

    if (g_strcmp0(trimmed, section_header) == 0) {
      section_found = TRUE;
      in_section = TRUE;
      g_string_append_printf(out, "%s\n", line);
      continue;
    }

    if (in_section) {
      g_autofree char *escaped_key = g_regex_escape_string(key, -1);
      g_autofree char *pattern = g_strdup_printf("^\\s*%s\\s*=\\s*.*$", escaped_key);
      g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
      if (regex != NULL && g_regex_match(regex, line, 0, NULL)) {
        g_string_append_printf(out, "%s\n", replacement);
        key_replaced = TRUE;
        continue;
      }
    }

    g_string_append_printf(out, "%s\n", line);
  }

  if (in_section && !key_replaced) {
    g_string_append_printf(out, "%s\n", replacement);
    key_replaced = TRUE;
  }

  if (!section_found) {
    if (out->len > 0 && out->str[out->len - 1] != '\n') {
      g_string_append_c(out, '\n');
    }
    if (out->len > 1) {
      g_string_append_c(out, '\n');
    }
    g_string_append_printf(out, "%s\n%s\n", section_header, replacement);
  }

  return g_string_free(out, FALSE);
}

static gboolean
sync_pumpkin_basic_configuration(PumpkinServer *self, GError **error)
{
  if (self == NULL || self->root_dir == NULL) {
    return TRUE;
  }

  g_autofree char *data_dir = g_build_filename(self->root_dir, "data", NULL);
  g_autofree char *config_dir = g_build_filename(data_dir, "config", NULL);
  if (!ensure_dir(config_dir, error)) {
    return FALSE;
  }

  g_autofree char *path = g_build_filename(config_dir, "configuration.toml", NULL);
  g_autofree char *contents = NULL;
  g_file_get_contents(path, &contents, NULL, NULL);
  if (contents == NULL) {
    contents = g_strdup("");
  }

  g_autofree char *java_addr = g_strdup_printf("\"0.0.0.0:%d\"", self->port > 0 ? self->port : 25565);
  g_autofree char *bedrock_addr = g_strdup_printf("\"0.0.0.0:%d\"", self->bedrock_port > 0 ? self->bedrock_port : 19132);
  g_autofree char *max_players = g_strdup_printf("%d", self->max_players > 0 ? self->max_players : 20);

  g_autofree char *step1 = toml_replace_or_append(contents, "java_edition_address", java_addr);
  g_autofree char *step2 = toml_replace_or_append(step1, "bedrock_edition_address", bedrock_addr);
  g_autofree char *step3 = toml_replace_or_append(step2, "max_players", max_players);
  if (!g_file_set_contents(path, step3, -1, error)) {
    return FALSE;
  }

  g_autofree char *features_path = g_build_filename(config_dir, "features.toml", NULL);
  g_autofree char *features_contents = NULL;
  g_file_get_contents(features_path, &features_contents, NULL, NULL);
  if (features_contents == NULL) {
    features_contents = g_strdup("");
  }

  g_autofree char *query_addr = g_strdup_printf("\"0.0.0.0:%d\"", self->port > 0 ? self->port : 25565);
  g_autofree char *rcon_addr = g_strdup_printf("\"0.0.0.0:%d\"", self->rcon_port > 0 ? self->rcon_port : 25575);
  g_autofree char *features_step1 =
    toml_replace_or_append_in_section(features_contents, "networking.query", "address", query_addr);
  g_autofree char *features_step2 =
    toml_replace_or_append_in_section(features_step1, "networking.rcon", "address", rcon_addr);
  return g_file_set_contents(features_path, features_step2, -1, error);
}

static gboolean
ensure_dir(const char *path, GError **error)
{
  if (g_mkdir_with_parents(path, 0755) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Failed to create directory %s", path);
    return FALSE;
  }
  return TRUE;
}

PumpkinServer *
pumpkin_server_new(const char *id, const char *name)
{
  PumpkinServer *self = g_object_new(PUMPKIN_TYPE_SERVER, NULL);
  self->id = g_strdup(id);
  self->name = g_strdup(name);
  self->download_url = g_strdup(default_download_url());
  self->rcon_host = g_strdup("127.0.0.1");
  return self;
}

static void
pumpkin_server_apply_defaults(PumpkinServer *self, const char *root)
{
  g_clear_pointer(&self->root_dir, g_free);
  self->root_dir = g_strdup(root);

  g_autofree char *bin_dir = g_build_filename(root, "bin", NULL);
  g_autofree char *data_dir = g_build_filename(root, "data", NULL);
  g_autofree char *plugins_dir = g_build_filename(data_dir, "plugins", NULL);
  g_autofree char *worlds_dir = g_build_filename(data_dir, "worlds", NULL);
  g_autofree char *players_dir = g_build_filename(data_dir, "players", NULL);

  ensure_dir(bin_dir, NULL);
  ensure_dir(plugins_dir, NULL);
  ensure_dir(worlds_dir, NULL);
  ensure_dir(players_dir, NULL);
}

PumpkinServer *
pumpkin_server_load(const char *dir, GError **error)
{
  g_autofree char *ini = server_ini_path(dir);
  g_autoptr(GKeyFile) keyfile = g_key_file_new();

  if (!g_key_file_load_from_file(keyfile, ini, G_KEY_FILE_NONE, error)) {
    return NULL;
  }

  g_autofree char *id = g_key_file_get_string(keyfile, "server", "id", NULL);
  g_autofree char *name = g_key_file_get_string(keyfile, "server", "name", NULL);

  PumpkinServer *self = pumpkin_server_new(id ? id : "default", name ? name : "Pumpkin Server");
  pumpkin_server_apply_defaults(self, dir);

  g_clear_pointer(&self->download_url, g_free);
  self->download_url = g_key_file_get_string(keyfile, "server", "download_url", NULL);
  if (self->download_url == NULL) {
    self->download_url = g_strdup(default_download_url());
  }

  g_clear_pointer(&self->installed_url, g_free);
  self->installed_url = g_key_file_get_string(keyfile, "server", "installed_url", NULL);

  g_clear_pointer(&self->installed_build_id, g_free);
  self->installed_build_id = g_key_file_get_string(keyfile, "server", "installed_build_id", NULL);

  g_clear_pointer(&self->installed_build_label, g_free);
  self->installed_build_label = g_key_file_get_string(keyfile, "server", "installed_build_label", NULL);

  g_clear_pointer(&self->rcon_host, g_free);
  self->rcon_host = g_key_file_get_string(keyfile, "rcon", "host", NULL);
  if (self->rcon_host == NULL) {
    self->rcon_host = g_strdup("127.0.0.1");
  }

  g_clear_pointer(&self->rcon_password, g_free);
  self->rcon_password = g_key_file_get_string(keyfile, "rcon", "password", NULL);

  self->rcon_port = g_key_file_get_integer(keyfile, "rcon", "port", NULL);
  if (self->rcon_port <= 0) {
    self->rcon_port = 25575;
  }

  self->port = g_key_file_get_integer(keyfile, "server", "port", NULL);
  if (self->port <= 0) {
    self->port = 25565;
  }

  self->bedrock_port = g_key_file_get_integer(keyfile, "server", "bedrock_port", NULL);
  if (self->bedrock_port <= 0) {
    self->bedrock_port = 19132;
  }

  self->max_players = g_key_file_get_integer(keyfile, "server", "max_players", NULL);
  if (self->max_players <= 0) {
    self->max_players = 20;
  }

  g_clear_pointer(&self->domain, g_free);
  self->domain = g_key_file_get_string(keyfile, "server", "domain", NULL);
  if (self->domain != NULL && self->domain[0] == '\0') {
    g_clear_pointer(&self->domain, g_free);
  }
  if (self->domain == NULL) {
    self->domain = g_key_file_get_string(keyfile, "server", "java_domain", NULL);
    if (self->domain != NULL && self->domain[0] == '\0') {
      g_clear_pointer(&self->domain, g_free);
    }
  }
  if (self->domain == NULL) {
    self->domain = g_key_file_get_string(keyfile, "server", "bedrock_domain", NULL);
    if (self->domain != NULL && self->domain[0] == '\0') {
      g_clear_pointer(&self->domain, g_free);
    }
  }

  if (g_key_file_has_key(keyfile, "ddns", "enabled", NULL)) {
    self->ddns_enabled = g_key_file_get_boolean(keyfile, "ddns", "enabled", NULL);
  }
  g_clear_pointer(&self->ddns_provider, g_free);
  self->ddns_provider = g_key_file_get_string(keyfile, "ddns", "provider", NULL);
  if (self->ddns_provider == NULL || self->ddns_provider[0] == '\0') {
    g_clear_pointer(&self->ddns_provider, g_free);
    self->ddns_provider = g_strdup("cloudflare");
  }
  g_clear_pointer(&self->ddns_cf_api_token, g_free);
  self->ddns_cf_api_token = g_key_file_get_string(keyfile, "ddns", "cloudflare_api_token", NULL);
  if (self->ddns_cf_api_token != NULL && self->ddns_cf_api_token[0] == '\0') {
    g_clear_pointer(&self->ddns_cf_api_token, g_free);
  }
  g_clear_pointer(&self->ddns_cf_zone_id, g_free);
  self->ddns_cf_zone_id = g_key_file_get_string(keyfile, "ddns", "cloudflare_zone_id", NULL);
  if (self->ddns_cf_zone_id != NULL && self->ddns_cf_zone_id[0] == '\0') {
    g_clear_pointer(&self->ddns_cf_zone_id, g_free);
  }
  g_clear_pointer(&self->ddns_cf_record_id, g_free);
  self->ddns_cf_record_id = g_key_file_get_string(keyfile, "ddns", "cloudflare_record_id", NULL);
  if (self->ddns_cf_record_id != NULL && self->ddns_cf_record_id[0] == '\0') {
    g_clear_pointer(&self->ddns_cf_record_id, g_free);
  }
  if (g_key_file_has_key(keyfile, "ddns", "cloudflare_proxied", NULL)) {
    self->ddns_cf_proxied = g_key_file_get_boolean(keyfile, "ddns", "cloudflare_proxied", NULL);
  }
  if (g_key_file_has_key(keyfile, "ddns", "update_ipv6", NULL)) {
    self->ddns_update_ipv6 = g_key_file_get_boolean(keyfile, "ddns", "update_ipv6", NULL);
  }
  self->ddns_interval_seconds = clamp_ddns_interval_seconds(
    g_key_file_get_integer(keyfile, "ddns", "interval_seconds", NULL));

  self->max_cpu_cores = g_key_file_get_integer(keyfile, "server", "max_cpu_cores", NULL);
  if (self->max_cpu_cores < 0) {
    self->max_cpu_cores = 0;
  }

  self->max_ram_mb = g_key_file_get_integer(keyfile, "server", "max_ram_mb", NULL);
  if (self->max_ram_mb < 0) {
    self->max_ram_mb = 0;
  }
  self->stats_sample_msec =
    clamp_stats_sample_msec(g_key_file_get_integer(keyfile, "server", "stats_sample_msec", NULL));

  if (g_key_file_has_key(keyfile, "server", "auto_restart", NULL)) {
    self->auto_restart = g_key_file_get_boolean(keyfile, "server", "auto_restart", NULL);
  }

  self->auto_restart_delay = g_key_file_get_integer(keyfile, "server", "auto_restart_delay", NULL);
  if (self->auto_restart_delay > 0 && self->auto_restart_delay < 1000) {
    self->auto_restart_delay *= 1000;
  }
  if (self->auto_restart_delay <= 0) {
    self->auto_restart_delay = 10000;
  }

  if (g_key_file_has_key(keyfile, "server", "auto_update_enabled", NULL)) {
    self->auto_update_enabled = g_key_file_get_boolean(keyfile, "server", "auto_update_enabled", NULL);
  }
  if (g_key_file_has_key(keyfile, "server", "auto_update_use_schedule", NULL)) {
    self->auto_update_use_schedule = g_key_file_get_boolean(keyfile, "server", "auto_update_use_schedule", NULL);
  }
  if (g_key_file_has_key(keyfile, "server", "auto_update_hour", NULL)) {
    self->auto_update_hour = g_key_file_get_integer(keyfile, "server", "auto_update_hour", NULL);
  }
  if (self->auto_update_hour < 0 || self->auto_update_hour > 23) {
    self->auto_update_hour = 1;
  }
  if (g_key_file_has_key(keyfile, "server", "auto_update_minute", NULL)) {
    self->auto_update_minute = g_key_file_get_integer(keyfile, "server", "auto_update_minute", NULL);
  }
  if (self->auto_update_minute < 0 || self->auto_update_minute > 59) {
    self->auto_update_minute = 0;
  }

  if (g_key_file_has_key(keyfile, "server", "auto_start_on_launch", NULL)) {
    self->auto_start_on_launch = g_key_file_get_boolean(keyfile, "server", "auto_start_on_launch", NULL);
  }

  self->auto_start_delay = g_key_file_get_integer(keyfile, "server", "auto_start_delay", NULL);
  if (self->auto_start_delay <= 0) {
    self->auto_start_delay = 10;
  }

  return self;
}

gboolean
pumpkin_server_save(PumpkinServer *self, GError **error)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new();

  g_key_file_set_string(keyfile, "server", "id", self->id);
  g_key_file_set_string(keyfile, "server", "name", self->name);
  g_key_file_set_string(keyfile, "server", "download_url", self->download_url);
  g_key_file_set_integer(keyfile, "server", "port", self->port);
  g_key_file_set_integer(keyfile, "server", "bedrock_port", self->bedrock_port);
  g_key_file_set_integer(keyfile, "server", "max_players", self->max_players);
  if (self->domain != NULL && self->domain[0] != '\0') {
    g_key_file_set_string(keyfile, "server", "domain", self->domain);
  }
  g_key_file_set_boolean(keyfile, "ddns", "enabled", self->ddns_enabled);
  if (self->ddns_provider != NULL && self->ddns_provider[0] != '\0') {
    g_key_file_set_string(keyfile, "ddns", "provider", self->ddns_provider);
  } else {
    g_key_file_set_string(keyfile, "ddns", "provider", "cloudflare");
  }
  if (self->ddns_cf_api_token != NULL && self->ddns_cf_api_token[0] != '\0') {
    g_key_file_set_string(keyfile, "ddns", "cloudflare_api_token", self->ddns_cf_api_token);
  }
  if (self->ddns_cf_zone_id != NULL && self->ddns_cf_zone_id[0] != '\0') {
    g_key_file_set_string(keyfile, "ddns", "cloudflare_zone_id", self->ddns_cf_zone_id);
  }
  if (self->ddns_cf_record_id != NULL && self->ddns_cf_record_id[0] != '\0') {
    g_key_file_set_string(keyfile, "ddns", "cloudflare_record_id", self->ddns_cf_record_id);
  }
  g_key_file_set_boolean(keyfile, "ddns", "cloudflare_proxied", self->ddns_cf_proxied);
  g_key_file_set_boolean(keyfile, "ddns", "update_ipv6", self->ddns_update_ipv6);
  g_key_file_set_integer(keyfile, "ddns", "interval_seconds", clamp_ddns_interval_seconds(self->ddns_interval_seconds));
  g_key_file_set_integer(keyfile, "server", "max_cpu_cores", self->max_cpu_cores);
  g_key_file_set_integer(keyfile, "server", "max_ram_mb", self->max_ram_mb);
  g_key_file_set_integer(keyfile, "server", "stats_sample_msec", self->stats_sample_msec);
  g_key_file_set_boolean(keyfile, "server", "auto_restart", self->auto_restart);
  g_key_file_set_integer(keyfile, "server", "auto_restart_delay", self->auto_restart_delay);
  g_key_file_set_boolean(keyfile, "server", "auto_update_enabled", self->auto_update_enabled);
  g_key_file_set_boolean(keyfile, "server", "auto_update_use_schedule", self->auto_update_use_schedule);
  g_key_file_set_integer(keyfile, "server", "auto_update_hour", self->auto_update_hour);
  g_key_file_set_integer(keyfile, "server", "auto_update_minute", self->auto_update_minute);
  g_key_file_set_boolean(keyfile, "server", "auto_start_on_launch", self->auto_start_on_launch);
  g_key_file_set_integer(keyfile, "server", "auto_start_delay", self->auto_start_delay);
  if (self->installed_url != NULL) {
    g_key_file_set_string(keyfile, "server", "installed_url", self->installed_url);
  }
  if (self->installed_build_id != NULL) {
    g_key_file_set_string(keyfile, "server", "installed_build_id", self->installed_build_id);
  }
  if (self->installed_build_label != NULL) {
    g_key_file_set_string(keyfile, "server", "installed_build_label", self->installed_build_label);
  }

  g_key_file_set_string(keyfile, "rcon", "host", self->rcon_host);
  g_key_file_set_integer(keyfile, "rcon", "port", self->rcon_port);
  if (self->rcon_password != NULL) {
    g_key_file_set_string(keyfile, "rcon", "password", self->rcon_password);
  }

  g_autofree char *ini = server_ini_path(self->root_dir);
  g_autofree char *data = g_key_file_to_data(keyfile, NULL, NULL);
  if (!g_file_set_contents(ini, data, -1, error)) {
    return FALSE;
  }
  return sync_pumpkin_basic_configuration(self, error);
}

const char *
pumpkin_server_get_id(PumpkinServer *self)
{
  return self->id;
}

const char *
pumpkin_server_get_name(PumpkinServer *self)
{
  return self->name;
}

const char *
pumpkin_server_get_root_dir(PumpkinServer *self)
{
  return self->root_dir;
}

const char *
pumpkin_server_get_download_url(PumpkinServer *self)
{
  return self->download_url;
}

const char *
pumpkin_server_get_installed_url(PumpkinServer *self)
{
  return self->installed_url;
}

const char *
pumpkin_server_get_installed_build_id(PumpkinServer *self)
{
  return self->installed_build_id;
}

const char *
pumpkin_server_get_installed_build_label(PumpkinServer *self)
{
  return self->installed_build_label;
}

const char *
pumpkin_server_get_rcon_host(PumpkinServer *self)
{
  return self->rcon_host;
}

const char *
pumpkin_server_get_rcon_password(PumpkinServer *self)
{
  return self->rcon_password;
}

int
pumpkin_server_get_rcon_port(PumpkinServer *self)
{
  return self->rcon_port;
}

const char *
pumpkin_server_get_domain(PumpkinServer *self)
{
  return self->domain;
}

gboolean
pumpkin_server_get_ddns_enabled(PumpkinServer *self)
{
  return self->ddns_enabled;
}

const char *
pumpkin_server_get_ddns_provider(PumpkinServer *self)
{
  return self->ddns_provider;
}

const char *
pumpkin_server_get_ddns_cf_api_token(PumpkinServer *self)
{
  return self->ddns_cf_api_token;
}

const char *
pumpkin_server_get_ddns_cf_zone_id(PumpkinServer *self)
{
  return self->ddns_cf_zone_id;
}

const char *
pumpkin_server_get_ddns_cf_record_id(PumpkinServer *self)
{
  return self->ddns_cf_record_id;
}

gboolean
pumpkin_server_get_ddns_cf_proxied(PumpkinServer *self)
{
  return self->ddns_cf_proxied;
}

gboolean
pumpkin_server_get_ddns_update_ipv6(PumpkinServer *self)
{
  return self->ddns_update_ipv6;
}

int
pumpkin_server_get_ddns_interval_seconds(PumpkinServer *self)
{
  return clamp_ddns_interval_seconds(self->ddns_interval_seconds);
}

int
pumpkin_server_get_port(PumpkinServer *self)
{
  return self->port;
}

int
pumpkin_server_get_bedrock_port(PumpkinServer *self)
{
  return self->bedrock_port;
}

int
pumpkin_server_get_max_players(PumpkinServer *self)
{
  return self->max_players;
}

int
pumpkin_server_get_max_cpu_cores(PumpkinServer *self)
{
  return self->max_cpu_cores;
}

int
pumpkin_server_get_max_ram_mb(PumpkinServer *self)
{
  return self->max_ram_mb;
}

int
pumpkin_server_get_stats_sample_msec(PumpkinServer *self)
{
  return self->stats_sample_msec;
}

int
pumpkin_server_get_pid(PumpkinServer *self)
{
  return self->pid;
}

gboolean
pumpkin_server_get_auto_restart(PumpkinServer *self)
{
  return self->auto_restart;
}

int
pumpkin_server_get_auto_restart_delay(PumpkinServer *self)
{
  return self->auto_restart_delay;
}

gboolean
pumpkin_server_get_auto_update_enabled(PumpkinServer *self)
{
  return self->auto_update_enabled;
}

gboolean
pumpkin_server_get_auto_update_use_schedule(PumpkinServer *self)
{
  return self->auto_update_use_schedule;
}

int
pumpkin_server_get_auto_update_hour(PumpkinServer *self)
{
  return self->auto_update_hour;
}

int
pumpkin_server_get_auto_update_minute(PumpkinServer *self)
{
  return self->auto_update_minute;
}

void
pumpkin_server_set_name(PumpkinServer *self, const char *name)
{
  g_free(self->name);
  self->name = g_strdup(name);
}

void
pumpkin_server_set_download_url(PumpkinServer *self, const char *url)
{
  g_free(self->download_url);
  self->download_url = g_strdup(url);
}

void
pumpkin_server_set_installed_url(PumpkinServer *self, const char *url)
{
  g_free(self->installed_url);
  self->installed_url = g_strdup(url);
}

void
pumpkin_server_set_installed_build_id(PumpkinServer *self, const char *build_id)
{
  g_free(self->installed_build_id);
  self->installed_build_id = g_strdup(build_id);
}

void
pumpkin_server_set_installed_build_label(PumpkinServer *self, const char *build_label)
{
  g_free(self->installed_build_label);
  self->installed_build_label = g_strdup(build_label);
}

void
pumpkin_server_set_auto_restart(PumpkinServer *self, gboolean enabled)
{
  self->auto_restart = enabled;
}

void
pumpkin_server_set_auto_restart_delay(PumpkinServer *self, int seconds)
{
  if (seconds > 0) {
    self->auto_restart_delay = seconds;
  }
}

void
pumpkin_server_set_auto_update_enabled(PumpkinServer *self, gboolean enabled)
{
  self->auto_update_enabled = enabled;
}

void
pumpkin_server_set_auto_update_use_schedule(PumpkinServer *self, gboolean enabled)
{
  self->auto_update_use_schedule = enabled;
}

void
pumpkin_server_set_auto_update_hour(PumpkinServer *self, int hour)
{
  if (hour >= 0 && hour <= 23) {
    self->auto_update_hour = hour;
  }
}

void
pumpkin_server_set_auto_update_minute(PumpkinServer *self, int minute)
{
  if (minute >= 0 && minute <= 59) {
    self->auto_update_minute = minute;
  }
}

void
pumpkin_server_set_rcon_host(PumpkinServer *self, const char *host)
{
  g_free(self->rcon_host);
  self->rcon_host = g_strdup(host);
}

void
pumpkin_server_set_rcon_port(PumpkinServer *self, int port)
{
  self->rcon_port = port;
}

void
pumpkin_server_set_rcon_password(PumpkinServer *self, const char *password)
{
  g_free(self->rcon_password);
  self->rcon_password = g_strdup(password);
}

void
pumpkin_server_set_domain(PumpkinServer *self, const char *domain)
{
  g_free(self->domain);
  if (domain != NULL && *domain != '\0') {
    self->domain = g_strdup(domain);
  } else {
    self->domain = NULL;
  }
}

void
pumpkin_server_set_ddns_enabled(PumpkinServer *self, gboolean enabled)
{
  self->ddns_enabled = enabled;
}

void
pumpkin_server_set_ddns_provider(PumpkinServer *self, const char *provider)
{
  g_clear_pointer(&self->ddns_provider, g_free);
  if (provider != NULL && *provider != '\0') {
    self->ddns_provider = g_strdup(provider);
  } else {
    self->ddns_provider = g_strdup("cloudflare");
  }
}

void
pumpkin_server_set_ddns_cf_api_token(PumpkinServer *self, const char *token)
{
  g_clear_pointer(&self->ddns_cf_api_token, g_free);
  if (token != NULL && *token != '\0') {
    self->ddns_cf_api_token = g_strdup(token);
  }
}

void
pumpkin_server_set_ddns_cf_zone_id(PumpkinServer *self, const char *zone_id)
{
  g_clear_pointer(&self->ddns_cf_zone_id, g_free);
  if (zone_id != NULL && *zone_id != '\0') {
    self->ddns_cf_zone_id = g_strdup(zone_id);
  }
}

void
pumpkin_server_set_ddns_cf_record_id(PumpkinServer *self, const char *record_id)
{
  g_clear_pointer(&self->ddns_cf_record_id, g_free);
  if (record_id != NULL && *record_id != '\0') {
    self->ddns_cf_record_id = g_strdup(record_id);
  }
}

void
pumpkin_server_set_ddns_cf_proxied(PumpkinServer *self, gboolean proxied)
{
  self->ddns_cf_proxied = proxied;
}

void
pumpkin_server_set_ddns_update_ipv6(PumpkinServer *self, gboolean enabled)
{
  self->ddns_update_ipv6 = enabled;
}

void
pumpkin_server_set_ddns_interval_seconds(PumpkinServer *self, int seconds)
{
  self->ddns_interval_seconds = clamp_ddns_interval_seconds(seconds);
}

void
pumpkin_server_set_port(PumpkinServer *self, int port)
{
  if (port > 0) {
    self->port = port;
  }
}

void
pumpkin_server_set_bedrock_port(PumpkinServer *self, int port)
{
  if (port > 0) {
    self->bedrock_port = port;
  }
}

void
pumpkin_server_set_max_players(PumpkinServer *self, int max_players)
{
  if (max_players > 0) {
    self->max_players = max_players;
  }
}

void
pumpkin_server_set_max_cpu_cores(PumpkinServer *self, int max_cpu_cores)
{
  if (max_cpu_cores >= 0) {
    self->max_cpu_cores = max_cpu_cores;
  }
}

void
pumpkin_server_set_max_ram_mb(PumpkinServer *self, int max_ram_mb)
{
  if (max_ram_mb >= 0) {
    self->max_ram_mb = max_ram_mb;
  }
}

void
pumpkin_server_set_stats_sample_msec(PumpkinServer *self, int msec)
{
  self->stats_sample_msec = clamp_stats_sample_msec(msec);
}

void
pumpkin_server_set_root_dir(PumpkinServer *self, const char *dir)
{
  pumpkin_server_apply_defaults(self, dir);
}

gboolean
pumpkin_server_get_auto_start_on_launch(PumpkinServer *self)
{
  return self->auto_start_on_launch;
}

int
pumpkin_server_get_auto_start_delay(PumpkinServer *self)
{
  return self->auto_start_delay;
}

void
pumpkin_server_set_auto_start_on_launch(PumpkinServer *self, gboolean enabled)
{
  self->auto_start_on_launch = enabled;
}

void
pumpkin_server_set_auto_start_delay(PumpkinServer *self, int seconds)
{
  if (seconds > 0) {
    self->auto_start_delay = seconds;
  }
}

char *
pumpkin_server_get_bin_path(PumpkinServer *self)
{
#if defined(G_OS_WIN32)
  return g_build_filename(self->root_dir, "bin", "pumpkin.exe", NULL);
#else
  return g_build_filename(self->root_dir, "bin", "pumpkin", NULL);
#endif
}

char *
pumpkin_server_get_data_dir(PumpkinServer *self)
{
  return g_build_filename(self->root_dir, "data", NULL);
}

char *
pumpkin_server_get_plugins_dir(PumpkinServer *self)
{
  return g_build_filename(self->root_dir, "data", "plugins", NULL);
}

char *
pumpkin_server_get_worlds_dir(PumpkinServer *self)
{
  return g_build_filename(self->root_dir, "data", "worlds", NULL);
}

char *
pumpkin_server_get_players_dir(PumpkinServer *self)
{
  return g_build_filename(self->root_dir, "data", "players", NULL);
}

char *
pumpkin_server_get_logs_dir(PumpkinServer *self)
{
  return g_build_filename(self->root_dir, "logs", NULL);
}

static void
read_stream_line(GDataInputStream *dis, PumpkinServer *self, const char *label);

static gboolean
auto_restart_cb(gpointer data)
{
  PumpkinServer *srv = PUMPKIN_SERVER(data);
  srv->restart_source_id = 0;
  srv->stop_requested = FALSE;
  g_autoptr(GError) err = NULL;
  if (!pumpkin_server_start(srv, &err)) {
    if (err != NULL) {
      g_signal_emit(srv, signals[LOG_LINE], 0, err->message);
    }
  }
  g_object_unref(srv);
  return G_SOURCE_REMOVE;
}

static void
ensure_log_stream(PumpkinServer *self)
{
  if (self->log_stream != NULL) {
    return;
  }

  g_autofree char *logs_dir = pumpkin_server_get_logs_dir(self);
  g_mkdir_with_parents(logs_dir, 0755);

  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree char *timestamp = g_date_time_format(now, "%Y%m%d-%H%M%S-%f");
  g_autofree char *filename = g_strdup_printf("session-%s.log", timestamp);
  g_autofree char *path = g_build_filename(logs_dir, filename, NULL);

  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GError) error = NULL;
  GFileOutputStream *out = g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error);
  if (out == NULL) {
    return;
  }

  self->log_stream = G_OUTPUT_STREAM(out);
  g_clear_pointer(&self->log_path, g_free);
  self->log_path = g_strdup(path);
}

static void
append_log_line(PumpkinServer *self, const char *line)
{
  if (line == NULL) {
    return;
  }

  ensure_log_stream(self);
  if (self->log_stream == NULL) {
    return;
  }

  g_autoptr(GError) error = NULL;
  g_output_stream_write_all(self->log_stream, line, strlen(line), NULL, NULL, &error);
  g_output_stream_write_all(self->log_stream, "\n", 1, NULL, NULL, &error);
  g_output_stream_flush(self->log_stream, NULL, NULL);
}

static char *
normalize_process_output_line(const char *line, gsize length)
{
  if (line == NULL) {
    return NULL;
  }

#if defined(G_OS_WIN32)
  if (length >= 4) {
    const guint8 *bytes = (const guint8 *)line;
    gboolean looks_like_utf16le = FALSE;

    if (bytes[0] == 0xFF && bytes[1] == 0xFE) {
      looks_like_utf16le = TRUE;
      bytes += 2;
      length -= 2;
    } else {
      gsize sample = length > 128 ? 128 : length;
      sample -= sample % 2;
      gsize pairs = 0;
      gsize odd_nuls = 0;
      gsize even_nuls = 0;
      for (gsize i = 0; i + 1 < sample; i += 2) {
        pairs++;
        if (bytes[i] == 0) {
          even_nuls++;
        }
        if (bytes[i + 1] == 0) {
          odd_nuls++;
        }
      }

      if (pairs >= 4 && odd_nuls * 4 >= pairs * 3 && even_nuls * 4 <= pairs) {
        looks_like_utf16le = TRUE;
      }
    }

    if (looks_like_utf16le && length >= 2) {
      g_autofree gunichar2 *utf16 = g_malloc(length);
      memcpy(utf16, bytes, length);
      glong items = (glong)(length / 2);
      char *utf8 = g_utf16_to_utf8((const gunichar2 *)utf16, items, NULL, NULL, NULL);
      if (utf8 != NULL) {
        return utf8;
      }
    }
  }
#endif

  if (g_utf8_validate(line, (gssize)length, NULL)) {
    return g_strndup(line, length);
  }

  g_autoptr(GError) error = NULL;
  char *utf8 = g_locale_to_utf8(line, (gssize)length, NULL, NULL, &error);
  if (utf8 != NULL) {
    return utf8;
  }

  return g_strndup(line, length);
}

static void
read_line_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GDataInputStream *dis = G_DATA_INPUT_STREAM(source);
  PumpkinServer *self = PUMPKIN_SERVER(user_data);
  gsize length = 0;
  g_autofree char *raw_line = g_data_input_stream_read_line_finish(dis, res, &length, NULL);
  if (raw_line == NULL) {
    g_object_unref(self);
    return;
  }

  g_autofree char *line = normalize_process_output_line(raw_line, length);
  if (line == NULL) {
    read_stream_line(dis, self, NULL);
    g_object_unref(self);
    return;
  }

  append_log_line(self, line);
  g_signal_emit(self, signals[LOG_LINE], 0, line);
  read_stream_line(dis, self, NULL);
  g_object_unref(self);
}

static void
read_stream_line(GDataInputStream *dis, PumpkinServer *self, const char *label)
{
  (void)label;
  g_data_input_stream_read_line_async(dis, G_PRIORITY_DEFAULT, NULL, read_line_cb, g_object_ref(self));
}

static void
pumpkin_server_attach_output(PumpkinServer *self)
{
  GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(self->process);
  GInputStream *stderr_stream = g_subprocess_get_stderr_pipe(self->process);

  if (stdout_stream != NULL) {
    self->stdout_dis = g_data_input_stream_new(stdout_stream);
    read_stream_line(self->stdout_dis, self, "stdout");
  }

  if (stderr_stream != NULL) {
    self->stderr_dis = g_data_input_stream_new(stderr_stream);
    read_stream_line(self->stderr_dis, self, "stderr");
  }
}

#if defined(G_OS_WIN32)
static void
pumpkin_server_handle_exit(PumpkinServer *self, const char *message)
{
  g_clear_object(&self->process);
  g_clear_object(&self->stdout_dis);
  g_clear_object(&self->stderr_dis);
  g_clear_object(&self->stdin_stream);
  self->pid = 0;

  if (self->process_handle != NULL) {
    CloseHandle(self->process_handle);
    self->process_handle = NULL;
  }
  if (self->job_handle != NULL) {
    CloseHandle(self->job_handle);
    self->job_handle = NULL;
  }
  if (self->process_watch_source_id != 0) {
    g_source_remove(self->process_watch_source_id);
    self->process_watch_source_id = 0;
  }
  if (self->log_stream != NULL) {
    g_output_stream_flush(self->log_stream, NULL, NULL);
    g_clear_object(&self->log_stream);
  }
  if (self->restart_source_id != 0) {
    g_source_remove(self->restart_source_id);
    self->restart_source_id = 0;
  }

  if (self->auto_restart && !self->stop_requested) {
    guint delay = self->auto_restart_delay > 0 ? (guint)self->auto_restart_delay : 10000;
    g_signal_emit(self, signals[LOG_LINE], 0, "Auto-restart scheduled");
    self->restart_source_id = g_timeout_add(delay, auto_restart_cb, g_object_ref(self));
  }

  g_signal_emit(self, signals[LOG_LINE], 0, message != NULL ? message : "Server process exited");
}

static gboolean
process_watch_cb(gpointer data)
{
  PumpkinServer *self = PUMPKIN_SERVER(data);
  if (self->process_handle == NULL) {
    self->process_watch_source_id = 0;
    g_object_unref(self);
    return G_SOURCE_REMOVE;
  }

  DWORD wait = WaitForSingleObject(self->process_handle, 0);
  if (wait == WAIT_TIMEOUT) {
    return G_SOURCE_CONTINUE;
  }

  self->process_watch_source_id = 0;
  pumpkin_server_handle_exit(self, "Server process exited");
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static gboolean
pumpkin_server_start_windows(PumpkinServer *self,
                             const char *bin,
                             const char *data_dir,
                             int max_cpu,
                             int max_ram,
                             GError **error)
{
  SECURITY_ATTRIBUTES sa = {0};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE stdout_read = NULL;
  HANDLE stdout_write = NULL;
  HANDLE stderr_read = NULL;
  HANDLE stderr_write = NULL;
  HANDLE stdin_read = NULL;
  HANDLE stdin_write = NULL;

  if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
      !CreatePipe(&stderr_read, &stderr_write, &sa, 0) ||
      !CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create Windows pipes");
    goto fail;
  }

  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

  g_autofree gunichar2 *cmdline = NULL;
  g_autofree gunichar2 *cwd = NULL;
  g_autofree char *cmdline_utf8 = NULL;
  cmdline_utf8 = g_strdup_printf("\"%s\"", bin);
  cmdline = g_utf8_to_utf16(cmdline_utf8, -1, NULL, NULL, NULL);
  cwd = g_utf8_to_utf16(data_dir, -1, NULL, NULL, NULL);
  if (cmdline == NULL || cwd == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to prepare Windows command line");
    goto fail;
  }

  STARTUPINFOW si = {0};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = stdin_read;
  si.hStdOutput = stdout_write;
  si.hStdError = stderr_write;

  PROCESS_INFORMATION pi = {0};
  if (!CreateProcessW(NULL,
                      (LPWSTR)cmdline,
                      NULL,
                      NULL,
                      TRUE,
                      CREATE_NO_WINDOW,
                      NULL,
                      (LPCWSTR)cwd,
                      &si,
                      &pi)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start Pumpkin server on Windows");
    goto fail;
  }

  CloseHandle(pi.hThread);
  CloseHandle(stdout_write);
  CloseHandle(stderr_write);
  CloseHandle(stdin_read);
  stdout_write = NULL;
  stderr_write = NULL;
  stdin_read = NULL;

  self->process_handle = pi.hProcess;
  self->pid = (int)pi.dwProcessId;
  self->stdin_stream = g_win32_output_stream_new(stdin_write, TRUE);
  self->stdout_dis = g_data_input_stream_new(g_win32_input_stream_new(stdout_read, TRUE));
  self->stderr_dis = g_data_input_stream_new(g_win32_input_stream_new(stderr_read, TRUE));
  stdin_write = NULL;
  stdout_read = NULL;
  stderr_read = NULL;

  read_stream_line(self->stdout_dis, self, "stdout");
  read_stream_line(self->stderr_dis, self, "stderr");
  self->process_watch_source_id = g_timeout_add(500, process_watch_cb, g_object_ref(self));

  if (max_cpu > 0 || max_ram > 0) {
    HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)self->pid);
    if (process != NULL) {
      if (max_cpu > 0) {
        DWORD_PTR mask = 0;
        int max_bits = (int)(sizeof(DWORD_PTR) * 8);
        int count = max_cpu > max_bits ? max_bits : max_cpu;
        for (int i = 0; i < count; i++) {
          mask |= ((DWORD_PTR)1 << i);
        }
        if (mask == 0) {
          mask = (DWORD_PTR)-1;
        }
        SetProcessAffinityMask(process, mask);
      }
      if (max_ram > 0) {
        HANDLE job = CreateJobObject(NULL, NULL);
        if (job != NULL) {
          JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = { 0 };
          info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
          info.ProcessMemoryLimit = (SIZE_T)max_ram * 1024ULL * 1024ULL;
          if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            if (AssignProcessToJobObject(job, process)) {
              self->job_handle = job;
            } else {
              CloseHandle(job);
            }
          } else {
            CloseHandle(job);
          }
        }
      }
      CloseHandle(process);
    }
  }

  return TRUE;

fail:
  if (stdout_read != NULL) CloseHandle(stdout_read);
  if (stdout_write != NULL) CloseHandle(stdout_write);
  if (stderr_read != NULL) CloseHandle(stderr_read);
  if (stderr_write != NULL) CloseHandle(stderr_write);
  if (stdin_read != NULL) CloseHandle(stdin_read);
  if (stdin_write != NULL) CloseHandle(stdin_write);
  return FALSE;
}
#endif

#if !defined(G_OS_WIN32)
static void
process_wait_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
  PumpkinServer *self = PUMPKIN_SERVER(user_data);
  g_autoptr(GError) error = NULL;

  g_clear_object(&self->process);
  self->stdin_stream = NULL;
  self->pid = 0;
  if (self->log_stream != NULL) {
    g_output_stream_flush(self->log_stream, NULL, NULL);
    g_clear_object(&self->log_stream);
  }

  if (self->restart_source_id != 0) {
    g_source_remove(self->restart_source_id);
    self->restart_source_id = 0;
  }

  if (self->auto_restart && !self->stop_requested) {
    guint delay = self->auto_restart_delay > 0 ? (guint)self->auto_restart_delay : 10000;
    g_signal_emit(self, signals[LOG_LINE], 0, "Auto-restart scheduled");
    self->restart_source_id = g_timeout_add(delay, auto_restart_cb, g_object_ref(self));
  }

  if (!g_subprocess_wait_finish(G_SUBPROCESS(source), res, &error)) {
    if (error != NULL) {
      g_signal_emit(self, signals[LOG_LINE], 0, error->message);
    }
  } else {
    g_signal_emit(self, signals[LOG_LINE], 0, "Server process exited");
  }
}
#endif

gboolean
pumpkin_server_start(PumpkinServer *self, GError **error)
{
  if (self->process != NULL
#if defined(G_OS_WIN32)
      || self->process_handle != NULL
#endif
  ) {
    return TRUE;
  }

  self->stop_requested = FALSE;
  if (self->restart_source_id != 0) {
    g_source_remove(self->restart_source_id);
    self->restart_source_id = 0;
  }

  g_autofree char *bin = pumpkin_server_get_bin_path(self);
  if (!g_file_test(bin, G_FILE_TEST_EXISTS)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Pumpkin binary not installed");
    return FALSE;
  }

  if (!sync_pumpkin_basic_configuration(self, error)) {
    return FALSE;
  }

  ensure_log_stream(self);
  g_autofree char *data_dir = pumpkin_server_get_data_dir(self);
  int max_cpu = clamp_cpu_cores(self->max_cpu_cores);
  int max_ram = clamp_ram_mb(self->max_ram_mb);

#if defined(G_OS_WIN32)
  return pumpkin_server_start_windows(self, bin, data_dir, max_cpu, max_ram, error);
#else
  const char *argv[] = { bin, NULL };
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
    G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE | G_SUBPROCESS_FLAGS_STDIN_PIPE
  );
  g_subprocess_launcher_set_cwd(launcher, data_dir);
  if (max_cpu > 0) {
    g_autofree char *num = g_strdup_printf("%d", max_cpu);
    g_subprocess_launcher_setenv(launcher, "RAYON_NUM_THREADS", num, TRUE);
    g_subprocess_launcher_setenv(launcher, "OMP_NUM_THREADS", num, TRUE);
    g_subprocess_launcher_setenv(launcher, "TOKIO_WORKER_THREADS", num, TRUE);
  }
  ChildLimits *limits = g_new0(ChildLimits, 1);
  limits->max_cpu_cores = max_cpu;
  limits->max_ram_mb = max_ram;
  limits->parent_pid = (int)getpid();
  g_subprocess_launcher_set_child_setup(launcher, child_setup_cb, limits, g_free);

  self->process = g_subprocess_launcher_spawnv(launcher, argv, error);
  g_object_unref(launcher);

  if (self->process == NULL) {
    return FALSE;
  }

  self->stdin_stream = g_subprocess_get_stdin_pipe(self->process);
  const char *pid_str = g_subprocess_get_identifier(self->process);
  if (pid_str != NULL) {
    self->pid = (int)g_ascii_strtoll(pid_str, NULL, 10);
  } else {
    self->pid = 0;
  }
  pumpkin_server_attach_output(self);
  g_subprocess_wait_async(self->process, NULL, process_wait_cb, self);
  return TRUE;
#endif
}

void
pumpkin_server_stop(PumpkinServer *self)
{
  if (self->process == NULL
#if defined(G_OS_WIN32)
      && self->process_handle == NULL
#endif
  ) {
    return;
  }

  self->stop_requested = TRUE;
  if (self->restart_source_id != 0) {
    g_source_remove(self->restart_source_id);
    self->restart_source_id = 0;
  }

#if defined(G_OS_WIN32)
  if (self->process_handle != NULL) {
    TerminateProcess(self->process_handle, 0);
  }
#else
  g_subprocess_send_signal(self->process, SIGTERM);
#endif
}

gboolean
pumpkin_server_get_running(PumpkinServer *self)
{
#if defined(G_OS_WIN32)
  return self->process_handle != NULL;
#else
  return self->process != NULL;
#endif
}

gboolean
pumpkin_server_send_command(PumpkinServer *self, const char *command, GError **error)
{
  if (self->stdin_stream == NULL
#if defined(G_OS_WIN32)
      || self->process_handle == NULL
#else
      || self->process == NULL
#endif
  ) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Server is not running");
    return FALSE;
  }
  if (command == NULL || *command == '\0') {
    return TRUE;
  }

  g_autofree char *line = g_strconcat(command, "\n", NULL);
  gsize written = 0;
  if (!g_output_stream_write_all(self->stdin_stream, line, strlen(line), &written, NULL, error)) {
    return FALSE;
  }
  g_output_stream_flush(self->stdin_stream, NULL, NULL);
  return TRUE;
}
