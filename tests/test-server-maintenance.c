#include "server-maintenance.h"
#include "server-command-tracker.h"

#include <glib/gstdio.h>

static const char *fake_binary;
static char *test_root;

static void
remove_tree(const char *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
    g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
    const char *name;
    while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(path, name, NULL);
      remove_tree(child);
    }
    g_rmdir(path);
  } else {
    g_remove(path);
  }
}

static void
completed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GAsyncResult **stored = data;
  *stored = g_object_ref(result);
}

static void
wait_result(GAsyncResult **result)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (*result == NULL && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  }
  g_assert_nonnull(*result);
}

static void
wait_exit(PumpkinServer *server)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (pumpkin_server_get_running(server) && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  }
  g_assert_false(pumpkin_server_get_running(server));
  while (g_main_context_iteration(NULL, FALSE)) {}
}

static PumpkinServer *
create_server(const char *mode)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp("smashed-maintenance-case-XXXXXX", &error);
  g_assert_no_error(error);
  PumpkinServer *server = pumpkin_server_new("test", "Test server");
  pumpkin_server_set_root_dir(server, root);
  g_autofree char *bin = pumpkin_server_get_bin_path(server);
  g_autofree char *dir = g_path_get_dirname(bin);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  g_autoptr(GFile) src = g_file_new_for_path(fake_binary);
  g_autoptr(GFile) dst = g_file_new_for_path(bin);
  g_assert_true(g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
  g_setenv("SMASHED_TEST_SERVER_MODE", mode, TRUE);
  g_assert_true(pumpkin_server_start(server, &error));
  g_assert_no_error(error);
  return server;
}

static void
cleanup_server(PumpkinServer *server)
{
  pumpkin_server_stop(server);
  if (pumpkin_server_get_running(server)) {
    pumpkin_server_force_stop(server);
    wait_exit(server);
  }
  remove_tree(pumpkin_server_get_root_dir(server));
}

static void
spin(guint milliseconds)
{
  gint64 until = g_get_monotonic_time() + milliseconds * (gint64)1000;
  do {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  } while (g_get_monotonic_time() < until);
}

typedef struct {
  guint exits;
  guint restarts;
  guint last_restart_delay;
  guint lifecycle_logs;
} LifecycleEvents;

static void
on_process_exited(PumpkinServer *server, LifecycleEvents *events)
{
  g_assert_false(pumpkin_server_get_running(server));
  g_assert_false(pumpkin_server_get_stopping(server));
  events->exits++;
}

static void
on_restart_scheduled(PumpkinServer *server, guint delay_ms, LifecycleEvents *events)
{
  g_assert_false(pumpkin_server_get_running(server));
  events->restarts++;
  events->last_restart_delay = delay_ms;
}

static void
on_lifecycle_log(PumpkinServer *server, const char *line, LifecycleEvents *events)
{
  (void)server;
  if (g_strcmp0(line, "Server process exited") == 0 ||
      g_strcmp0(line, "Auto-restart scheduled") == 0 ||
      g_strcmp0(line, "Serverprozess beendet") == 0) events->lifecycle_logs++;
}

static void
connect_lifecycle_events(PumpkinServer *server, LifecycleEvents *events)
{
  g_signal_connect(server, "process-exited", G_CALLBACK(on_process_exited), events);
  g_signal_connect(server, "restart-scheduled", G_CALLBACK(on_restart_scheduled), events);
  g_signal_connect(server, "log-line", G_CALLBACK(on_lifecycle_log), events);
}

static void
test_lifecycle_log_collision(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  LifecycleEvents events = {0};
  connect_lifecycle_events(server, &events);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_send_command(server, "lifecycle-log-collision", &error));
  g_assert_no_error(error);
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (events.lifecycle_logs < 3 && g_get_monotonic_time() < deadline) spin(5);
  g_assert_cmpuint(events.lifecycle_logs, ==, 3);
  g_assert_cmpuint(events.exits, ==, 0);
  g_assert_cmpuint(events.restarts, ==, 0);
  g_assert_true(pumpkin_server_get_running(server));
  pumpkin_server_stop(server);
  wait_exit(server);
  g_assert_cmpuint(events.exits, ==, 1);
  g_assert_cmpuint(events.restarts, ==, 0);
  /* The model emits lifecycle signals, not magic log lines, for its own exit. */
  g_assert_cmpuint(events.lifecycle_logs, ==, 3);
  cleanup_server(server);
  g_signal_handlers_disconnect_by_data(server, &events);
}

static void
collect_console_message(PumpkinServer *server, const char *message, GPtrArray *messages)
{
  (void)server;
  g_ptr_array_add(messages, g_strdup(message));
}

static void
assert_separate_channels(GPtrArray *notices, GPtrArray *output)
{
  for (guint n = 0; n < notices->len; ++n) {
    const char *notice = g_ptr_array_index(notices, n);
    g_assert_nonnull(notice);
    g_assert_cmpstr(notice, !=, "");
    for (guint o = 0; o < output->len; ++o) {
      g_assert_cmpstr(g_ptr_array_index(output, o), !=, notice);
    }
  }
}

static void
test_application_notice_channels(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  g_autoptr(GPtrArray) notices = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) output = g_ptr_array_new_with_free_func(g_free);
  g_signal_connect(server, "console-notice", G_CALLBACK(collect_console_message), notices);
  g_signal_connect(server, "log-line", G_CALLBACK(collect_console_message), output);
  pumpkin_server_stop(server);
  wait_exit(server);
  g_assert_cmpuint(notices->len, ==, 1);
  assert_separate_channels(notices, output);

  /* The archive retains stable diagnostics independently of console wording. */
  g_autofree char *logs = pumpkin_server_get_logs_dir(server);
  g_autoptr(GDir) directory = g_dir_open(logs, 0, NULL);
  g_assert_nonnull(directory);
  const char *name = g_dir_read_name(directory);
  g_assert_nonnull(name);
  g_autofree char *path = g_build_filename(logs, name, NULL);
  g_autofree char *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_assert_nonnull(strstr(contents, "smashed_pumpkin: Stopping server: waiting for saved data and process exit."));
  cleanup_server(server);
  g_signal_handlers_disconnect_by_data(server, notices);
  g_signal_handlers_disconnect_by_data(server, output);
}

static void
test_failed_restart_notice(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  pumpkin_server_set_auto_restart_delay(server, 500);
  g_autoptr(GPtrArray) notices = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) output = g_ptr_array_new_with_free_func(g_free);
  g_signal_connect(server, "console-notice", G_CALLBACK(collect_console_message), notices);
  g_signal_connect(server, "log-line", G_CALLBACK(collect_console_message), output);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_send_command(server, "crash", &error));
  g_assert_no_error(error);
  wait_exit(server);
  g_autofree char *binary = pumpkin_server_get_bin_path(server);
  g_assert_cmpint(g_remove(binary), ==, 0);
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (notices->len == 0 && g_get_monotonic_time() < deadline) spin(5);
  g_assert_cmpuint(notices->len, ==, 1);
  assert_separate_channels(notices, output);
  g_assert_false(pumpkin_server_get_running(server));
  cleanup_server(server);
  g_signal_handlers_disconnect_by_data(server, notices);
  g_signal_handlers_disconnect_by_data(server, output);
}

typedef struct {
  gboolean exited;
  guint logs_after_exit;
} BufferedExitEvents;

static void
on_buffered_exit(PumpkinServer *server, BufferedExitEvents *events)
{
  g_assert_false(pumpkin_server_get_running(server));
  events->exited = TRUE;
}

static void
on_buffered_output(PumpkinServer *server, const char *line, BufferedExitEvents *events)
{
  (void)server;
  if (events->exited && g_str_has_prefix(line, "Buffered exit output:")) events->logs_after_exit++;
}

static void
test_buffered_exit_output(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  pumpkin_server_set_auto_restart(server, FALSE);
  BufferedExitEvents events = {0};
  g_signal_connect(server, "process-exited", G_CALLBACK(on_buffered_exit), &events);
  g_signal_connect(server, "log-line", G_CALLBACK(on_buffered_output), &events);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_send_command(server, "buffered-exit-output", &error));
  g_assert_no_error(error);
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  g_autofree char *marker = g_build_filename(data_dir, "buffered-output-written", NULL);
  /* Deliberately withhold main-context dispatch until the child has written its
   * tail. The read and process-wait callbacks will both have pending work. */
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!g_file_test(marker, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) g_usleep(1000);
  g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
  g_usleep(50000);
  g_autofree char *logs_dir = pumpkin_server_get_logs_dir(server);
  g_autoptr(GDir) dir = g_dir_open(logs_dir, 0, &error);
  g_assert_no_error(error);
  g_assert_nonnull(dir);
  const char *name = g_dir_read_name(dir);
  g_assert_nonnull(name);
  g_autofree char *log_path = g_build_filename(logs_dir, name, NULL);
  g_autofree char *contents = NULL;
  deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  do {
    spin(5);
    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(log_path, &contents, NULL, &error));
    g_assert_no_error(error);
  } while ((!events.exited || strstr(contents, "Buffered exit output: 199") == NULL) &&
           g_get_monotonic_time() < deadline);
  g_assert_true(events.exited);
  g_assert_false(pumpkin_server_get_running(server));
  /* Draining the original OutputReader must not revive the live UI, but every
   * trailing line still belongs in the original session's persistent log. */
  g_assert_cmpuint(events.logs_after_exit, ==, 0);
  for (guint i = 0; i < 200; ++i) {
    g_autofree char *line = g_strdup_printf("Buffered exit output: %03u\n", i);
    g_assert_nonnull(strstr(contents, line));
  }
  cleanup_server(server);
  g_signal_handlers_disconnect_by_data(server, &events);
}

static void
test_restart_defaults(void)
{
  g_autofree char *root = g_dir_make_tmp("smashed-restart-defaults-XXXXXX", NULL);
  g_autoptr(PumpkinServer) server = pumpkin_server_new("defaults", "Defaults");
  pumpkin_server_set_root_dir(server, root);
  g_assert_true(pumpkin_server_get_auto_restart(server));
  g_assert_cmpint(pumpkin_server_get_auto_restart_delay(server), ==, 4000);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_save(server, &error)); g_assert_no_error(error);
  g_autoptr(PumpkinServer) loaded = pumpkin_server_load(root, &error);
  g_assert_no_error(error); g_assert_nonnull(loaded);
  g_assert_true(pumpkin_server_get_auto_restart(loaded));
  g_assert_cmpint(pumpkin_server_get_auto_restart_delay(loaded), ==, 4000);
  pumpkin_server_set_auto_restart(server, FALSE);
  pumpkin_server_set_auto_restart_delay(server, 17000);
  g_assert_true(pumpkin_server_save(server, &error)); g_assert_no_error(error);
  g_clear_object(&loaded); loaded = pumpkin_server_load(root, &error);
  g_assert_no_error(error); g_assert_false(pumpkin_server_get_auto_restart(loaded));
  g_assert_cmpint(pumpkin_server_get_auto_restart_delay(loaded), ==, 17000);
  g_autofree char *path = g_build_filename(root, "server.ini", NULL);
  g_assert_true(g_file_set_contents(path, "[server]\nid=legacy\nname=Legacy\n", -1, NULL));
  g_clear_object(&loaded); loaded = pumpkin_server_load(root, &error);
  g_assert_no_error(error); g_assert_true(pumpkin_server_get_auto_restart(loaded));
  g_assert_cmpint(pumpkin_server_get_auto_restart_delay(loaded), ==, 4000);
  remove_tree(root);
}

static void
test_crash_restart(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  g_autoptr(GError) error = NULL;
  int original_pid = pumpkin_server_get_pid(server);
  LifecycleEvents events = {0};
  connect_lifecycle_events(server, &events);
  g_assert_true(pumpkin_server_send_command(server, "crash", &error));
  g_assert_no_error(error); wait_exit(server);
  g_assert_cmpuint(events.exits, ==, 1);
  g_assert_cmpuint(events.restarts, ==, 1);
  g_assert_cmpuint(events.last_restart_delay, ==, 4000);
  gint64 exited = g_get_monotonic_time(), deadline = exited + 5 * G_USEC_PER_SEC;
  while (!pumpkin_server_get_running(server) && g_get_monotonic_time() < deadline) spin(5);
  g_assert_true(pumpkin_server_get_running(server));
  g_assert_cmpint(pumpkin_server_get_pid(server), !=, original_pid);
  g_assert_cmpint(g_get_monotonic_time() - exited, >=, 3900000);
  /* Explicit stops and switching restart off during the delay must not restart. */
  pumpkin_server_set_auto_restart_delay(server, 100);
  pumpkin_server_stop(server); wait_exit(server); spin(200);
  g_assert_false(pumpkin_server_get_running(server));
  g_assert_true(pumpkin_server_start(server, &error)); g_assert_no_error(error);
  g_assert_true(pumpkin_server_send_command(server, "crash", &error));
  g_assert_no_error(error); wait_exit(server);
  pumpkin_server_set_auto_restart(server, FALSE); spin(200);
  g_assert_false(pumpkin_server_get_running(server));
  g_assert_cmpuint(events.exits, ==, 3);
  g_assert_cmpuint(events.restarts, ==, 2);
  g_assert_cmpuint(events.last_restart_delay, ==, 100);
  g_assert_cmpuint(events.lifecycle_logs, ==, 0);
  cleanup_server(server);
  g_signal_handlers_disconnect_by_data(server, &events);
}

static void
test_graceful_stop(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  g_autoptr(GError) error = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_assert_true(pumpkin_server_send_poll_command(server, "list uuids"));
  g_assert_false(pumpkin_server_send_poll_command(server, "list uuids"));
  g_assert_true(pumpkin_server_send_command(server, "list", &error));
  g_assert_no_error(error);
  g_assert_true(pumpkin_command_consume_reply(G_OBJECT(server), "list"));
  g_assert_false(pumpkin_command_consume_reply(G_OBJECT(server), "list"));
  g_assert_true(pumpkin_server_send_poll_command(server, "tps"));
  g_assert_false(pumpkin_server_send_poll_command(server, "tps"));
  pumpkin_server_stop_async(server, NULL, completed, &result);
  pumpkin_server_stop(server);
  g_assert_true(pumpkin_server_get_running(server));
  g_assert_true(pumpkin_server_get_stopping(server));
  g_assert_false(pumpkin_server_start(server, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&error);
  wait_result(&result);
  g_assert_true(pumpkin_server_stop_finish(server, result, &error));
  g_assert_no_error(error);
  g_assert_false(pumpkin_server_get_running(server));
  g_assert_false(pumpkin_server_get_stopping(server));
  g_autofree char *data = pumpkin_server_get_data_dir(server);
  g_autofree char *marker = g_build_filename(data, "saved-marker", NULL);
  g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
  pumpkin_server_stop(server);
  g_assert_true(pumpkin_server_start(server, &error));
  g_assert_no_error(error);
  g_assert_false(pumpkin_server_get_stopping(server));
  g_assert_true(pumpkin_command_poll_ready(G_OBJECT(server), "tps", g_get_monotonic_time()));
  cleanup_server(server);
}

static void
test_timeout_requires_force(void)
{
  g_autoptr(PumpkinServer) server = create_server("ignore-stop");
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GError) error = NULL;
  pumpkin_server_stop_async(server, NULL, completed, &result);
  wait_result(&result);
  g_assert_false(pumpkin_server_stop_finish(server, result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_true(pumpkin_server_get_running(server));
  g_assert_true(pumpkin_server_get_stop_timed_out(server));
  pumpkin_server_force_stop(server);
  wait_exit(server);
  g_assert_false(pumpkin_server_get_stop_timed_out(server));
  cleanup_server(server);
}

static void
test_cancelled_wait(void)
{
  g_autoptr(PumpkinServer) server = create_server("ignore-stop");
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);
  pumpkin_server_stop_async(server, cancellable, completed, &result);
  wait_result(&result);
  g_assert_false(pumpkin_server_stop_finish(server, result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false(pumpkin_server_get_stopping(server));
  g_clear_error(&error);
  g_clear_object(&result);
  g_cancellable_reset(cancellable);
  pumpkin_server_stop_async(server, cancellable, completed, &result);
  g_cancellable_cancel(cancellable);
  wait_result(&result);
  g_assert_false(pumpkin_server_stop_finish(server, result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_true(pumpkin_server_get_running(server));
  cleanup_server(server);
}

static void
test_maintenance_lock(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_begin_maintenance(server, &error));
  g_assert_no_error(error);
  g_assert_false(pumpkin_server_begin_maintenance(server, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&error);
  g_assert_false(pumpkin_server_start(server, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  pumpkin_server_end_maintenance(server);
  cleanup_server(server);
}

static void
prepare_start(PumpkinServer *server, guint *count)
{
  g_assert_false(pumpkin_server_get_running(server));
  g_assert_false(pumpkin_server_get_stopping(server));
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_begin_maintenance(server, &error));
  g_assert_no_error(error);
  ++*count;
  pumpkin_server_end_maintenance(server);
}

static void
test_start_preparation(void)
{
  g_autoptr(PumpkinServer) server = create_server("normal");
  guint count = 0;
  g_signal_connect(server, "prepare-start", G_CALLBACK(prepare_start), &count);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_start(server, &error));
  g_assert_no_error(error); g_assert_cmpuint(count, ==, 0);
  pumpkin_server_stop(server); wait_exit(server);
  g_assert_true(pumpkin_server_begin_maintenance(server, &error));
  g_assert_false(pumpkin_server_start(server, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
  g_assert_cmpuint(count, ==, 0); pumpkin_server_end_maintenance(server);
  for (guint i = 1; i <= 2; ++i) {
    g_assert_true(pumpkin_server_start(server, &error));
    g_assert_no_error(error); g_assert_cmpuint(count, ==, i);
    pumpkin_server_stop(server); wait_exit(server);
  }
  cleanup_server(server);
}

static void
assert_file(const char *path, const char *expected)
{
  g_autofree char *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_assert_cmpstr(contents, ==, expected);
}

static void
test_binary_install(void)
{
  g_autofree char *source = g_build_filename(test_root, "source", NULL);
  g_autofree char *destination = g_build_filename(test_root, "pumpkin", NULL);
  g_autofree char *backup = g_strconcat(destination, ".previous", NULL);
  g_assert_true(g_file_set_contents(source, "new", -1, NULL));
  g_assert_true(g_file_set_contents(destination, "old", -1, NULL));
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GError) error = NULL;
  pumpkin_binary_install_async(source, destination, NULL, completed, &result);
  wait_result(&result);
  g_assert_true(pumpkin_binary_install_finish(result, &error));
  g_assert_no_error(error);
  assert_file(destination, "new");
  assert_file(backup, "old");
  g_clear_object(&result);
  g_assert_true(g_file_set_contents(source, "newer", -1, NULL));
  pumpkin_binary_install_async(source, destination, NULL, completed, &result);
  wait_result(&result);
  g_assert_true(pumpkin_binary_install_finish(result, &error));
  assert_file(destination, "newer");
  assert_file(backup, "new");
  g_clear_object(&result);
  g_remove(source);
  pumpkin_binary_install_async(source, destination, NULL, completed, &result);
  wait_result(&result);
  g_assert_false(pumpkin_binary_install_finish(result, &error));
  assert_file(destination, "newer");
  assert_file(backup, "new");
  g_clear_error(&error);
  g_clear_object(&result);
  g_assert_true(g_file_set_contents(source, "not-committed", -1, NULL));
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);
  pumpkin_binary_install_async(source, destination, cancellable, completed, &result);
  wait_result(&result);
  g_assert_false(pumpkin_binary_install_finish(result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  assert_file(destination, "newer");
  g_clear_error(&error);
  g_clear_object(&result);
  g_remove(backup);
  g_assert_cmpint(g_mkdir(backup, 0700), ==, 0);
  pumpkin_binary_install_async(source, destination, NULL, completed, &result);
  wait_result(&result);
  g_assert_false(pumpkin_binary_install_finish(result, &error));
  assert_file(destination, "newer");
  g_clear_error(&error);
  g_clear_object(&result);
  g_remove(source);
  g_autoptr(GFile) src = g_file_new_for_path(source);
  g_assert_true(g_file_make_symbolic_link(src, destination, NULL, &error));
  pumpkin_binary_install_async(source, destination, NULL, completed, &result);
  wait_result(&result);
  g_assert_false(pumpkin_binary_install_finish(result, &error));
  assert_file(destination, "newer");
  g_autoptr(GDir) dir = g_dir_open(test_root, 0, NULL);
  const char *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    g_assert_null(strstr(name, ".stage-"));
  }
}

int
main(int argc, char **argv)
{
  g_assert_cmpint(argc, >=, 2);
  fake_binary = argv[1];
  g_test_init(&argc, &argv, NULL);
  test_root = g_dir_make_tmp("smashed-maintenance-tests-XXXXXX", NULL);
  g_assert_nonnull(test_root);
  g_test_add_func("/maintenance/restart-defaults", test_restart_defaults);
  g_test_add_func("/maintenance/crash-restart", test_crash_restart);
  g_test_add_func("/maintenance/lifecycle-log-collision", test_lifecycle_log_collision);
  g_test_add_func("/maintenance/application-notice-channels", test_application_notice_channels);
  g_test_add_func("/maintenance/failed-restart-notice", test_failed_restart_notice);
  g_test_add_func("/maintenance/buffered-exit-output", test_buffered_exit_output);
  g_test_add_func("/maintenance/graceful-stop", test_graceful_stop);
  g_test_add_func("/maintenance/timeout-force", test_timeout_requires_force);
  g_test_add_func("/maintenance/cancelled-wait", test_cancelled_wait);
  g_test_add_func("/maintenance/lock", test_maintenance_lock);
  g_test_add_func("/maintenance/start-preparation", test_start_preparation);
  g_test_add_func("/maintenance/binary-install", test_binary_install);
  int status = g_test_run();
  remove_tree(test_root);
  g_free(test_root);
  return status;
}
