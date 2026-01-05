#include "server.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <signal.h>

struct _PumpkinServer {
  GObject parent_instance;

  char *id;
  char *name;
  char *root_dir;
  char *download_url;
  char *installed_url;
  char *rcon_host;
  char *rcon_password;
  int rcon_port;
  int port;
  int max_players;
  gboolean auto_restart;
  int auto_restart_delay;
  gboolean stop_requested;
  guint restart_source_id;

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
  g_clear_pointer(&self->rcon_host, g_free);
  g_clear_pointer(&self->rcon_password, g_free);
  g_clear_object(&self->process);
  g_clear_object(&self->stdout_dis);
  g_clear_object(&self->stderr_dis);
  g_clear_object(&self->log_stream);
  g_clear_pointer(&self->log_path, g_free);
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
  self->max_players = 20;
  self->auto_restart = FALSE;
  self->auto_restart_delay = 10;
  self->pid = 0;
}

static char *
server_ini_path(const char *dir)
{
  return g_build_filename(dir, "server.ini", NULL);
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
  self->download_url = g_strdup("https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-X64-Linux");
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
    self->download_url = g_strdup("https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/pumpkin-X64-Linux");
  }

  g_clear_pointer(&self->installed_url, g_free);
  self->installed_url = g_key_file_get_string(keyfile, "server", "installed_url", NULL);

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

  self->max_players = g_key_file_get_integer(keyfile, "server", "max_players", NULL);
  if (self->max_players <= 0) {
    self->max_players = 20;
  }

  if (g_key_file_has_key(keyfile, "server", "auto_restart", NULL)) {
    self->auto_restart = g_key_file_get_boolean(keyfile, "server", "auto_restart", NULL);
  }

  self->auto_restart_delay = g_key_file_get_integer(keyfile, "server", "auto_restart_delay", NULL);
  if (self->auto_restart_delay <= 0) {
    self->auto_restart_delay = 10;
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
  g_key_file_set_integer(keyfile, "server", "max_players", self->max_players);
  g_key_file_set_boolean(keyfile, "server", "auto_restart", self->auto_restart);
  g_key_file_set_integer(keyfile, "server", "auto_restart_delay", self->auto_restart_delay);
  if (self->installed_url != NULL) {
    g_key_file_set_string(keyfile, "server", "installed_url", self->installed_url);
  }

  g_key_file_set_string(keyfile, "rcon", "host", self->rcon_host);
  g_key_file_set_integer(keyfile, "rcon", "port", self->rcon_port);
  if (self->rcon_password != NULL) {
    g_key_file_set_string(keyfile, "rcon", "password", self->rcon_password);
  }

  g_autofree char *ini = server_ini_path(self->root_dir);
  g_autofree char *data = g_key_file_to_data(keyfile, NULL, NULL);
  return g_file_set_contents(ini, data, -1, error);
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

int
pumpkin_server_get_port(PumpkinServer *self)
{
  return self->port;
}

int
pumpkin_server_get_max_players(PumpkinServer *self)
{
  return self->max_players;
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
pumpkin_server_set_port(PumpkinServer *self, int port)
{
  if (port > 0) {
    self->port = port;
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
pumpkin_server_set_root_dir(PumpkinServer *self, const char *dir)
{
  pumpkin_server_apply_defaults(self, dir);
}

char *
pumpkin_server_get_bin_path(PumpkinServer *self)
{
  return g_build_filename(self->root_dir, "bin", "pumpkin", NULL);
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

static void
read_line_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GDataInputStream *dis = G_DATA_INPUT_STREAM(source);
  PumpkinServer *self = PUMPKIN_SERVER(user_data);
  gsize length = 0;
  g_autofree char *line = g_data_input_stream_read_line_finish(dis, res, &length, NULL);
  if (line == NULL) {
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

static void
process_wait_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
  PumpkinServer *self = PUMPKIN_SERVER(user_data);
  g_autoptr(GError) error = NULL;

  if (!g_subprocess_wait_finish(G_SUBPROCESS(source), res, &error)) {
    if (error != NULL) {
      g_signal_emit(self, signals[LOG_LINE], 0, error->message);
    }
  } else {
    g_signal_emit(self, signals[LOG_LINE], 0, "Server process exited");
  }

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
    guint delay = self->auto_restart_delay > 0 ? (guint)self->auto_restart_delay : 10;
    g_signal_emit(self, signals[LOG_LINE], 0, "Auto-restart scheduled");
    self->restart_source_id = g_timeout_add_seconds(delay, auto_restart_cb, g_object_ref(self));
  }
}

gboolean
pumpkin_server_start(PumpkinServer *self, GError **error)
{
  if (self->process != NULL) {
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

  ensure_log_stream(self);
  g_autofree char *data_dir = pumpkin_server_get_data_dir(self);
  const char *argv[] = { bin, NULL };

  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
    G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE | G_SUBPROCESS_FLAGS_STDIN_PIPE
  );
  g_subprocess_launcher_set_cwd(launcher, data_dir);

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
}

void
pumpkin_server_stop(PumpkinServer *self)
{
  if (self->process == NULL) {
    return;
  }

  self->stop_requested = TRUE;
  if (self->restart_source_id != 0) {
    g_source_remove(self->restart_source_id);
    self->restart_source_id = 0;
  }

  g_subprocess_send_signal(self->process, SIGTERM);
}

gboolean
pumpkin_server_get_running(PumpkinServer *self)
{
  return self->process != NULL;
}

gboolean
pumpkin_server_send_command(PumpkinServer *self, const char *command, GError **error)
{
  if (self->process == NULL || self->stdin_stream == NULL) {
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
