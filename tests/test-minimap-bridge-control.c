#include "minimap-bridge-control.h"
#include "minimap-store.h"
#include "smashed-pumpkin-resources.h"
#include <glib/gstdio.h>

static const char *fake_binary;

static void remove_tree(const char *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
    g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
    const char *name;
    while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(path, name, NULL);
      remove_tree(child);
    }
    g_rmdir(path);
  } else g_remove(path);
}

static void spin(guint milliseconds)
{
  gint64 until = g_get_monotonic_time() + milliseconds * (gint64)1000;
  do {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  } while (g_get_monotonic_time() < until);
}

static void test_bridge_control(void)
{
  g_autofree char *root = g_dir_make_tmp("smashed-bridge-control-XXXXXX", NULL);
  g_autoptr(PumpkinServer) server = pumpkin_server_new("bridge", "Bridge fixture");
  pumpkin_server_set_root_dir(server, root);
  g_autofree char *binary = pumpkin_server_get_bin_path(server), *bytes = NULL;
  gsize size = 0;
  g_assert_true(g_file_get_contents(fake_binary, &bytes, &size, NULL));
  g_assert_true(g_file_set_contents(binary, bytes, (gssize)size, NULL));
  g_assert_cmpint(g_chmod(binary, 0700), ==, 0);
  g_autofree char *data = pumpkin_server_get_data_dir(server);
  g_autofree char *plugins = pumpkin_server_get_plugins_dir(server);
  g_autofree char *bridge = g_build_filename(plugins, PUMPKIN_MAP_BRIDGE_FILENAME, NULL);
  g_autofree char *parked = g_strconcat(bridge, ".disabled", NULL);
  g_clear_pointer(&bytes, g_free);
  g_assert_true(g_file_get_contents(g_getenv("SMASHED_BUNDLED_BRIDGE_SAMPLE"), &bytes, &size, NULL));
  g_assert_true(g_file_set_contents(bridge, bytes, (gssize)size, NULL));
  pumpkin_minimap_bridge_set_enabled(server, FALSE);
  g_assert_true(g_file_test(parked, G_FILE_TEST_EXISTS));
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_start(server, &error)); g_assert_no_error(error);
  g_autofree char *journal = g_build_filename(data, "bridge-commands", NULL);
  spin(150); g_assert_false(g_file_test(journal, G_FILE_TEST_EXISTS));
  pumpkin_minimap_bridge_set_enabled(server, TRUE); spin(150);
  /* The real host acknowledges loads with the relative filename, not metadata name.
   * Changing the desired state during an unload must wait for that acknowledgement. */
  pumpkin_minimap_bridge_set_enabled(server, FALSE);
  pumpkin_minimap_bridge_set_enabled(server, TRUE); spin(300);
  g_autofree char *commands = NULL;
  g_assert_true(g_file_get_contents(journal, &commands, NULL, NULL));
  g_assert_cmpstr(commands, ==, "plugin load plugins/map-bridge.wasm\nplugin unload map-bridge\nplugin load plugins/map-bridge.wasm\n");
  g_assert_true(g_file_test(bridge, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(parked, G_FILE_TEST_EXISTS));
  pumpkin_minimap_bridge_set_enabled(server, FALSE); spin(200);
  g_clear_pointer(&commands, g_free);
  g_assert_true(g_file_get_contents(journal, &commands, NULL, NULL));
  g_assert_true(g_str_has_suffix(commands, "plugin unload map-bridge\n"));
  g_assert_true(g_file_test(parked, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(bridge, G_FILE_TEST_EXISTS));
  pumpkin_server_stop(server);
  gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  while (pumpkin_server_get_running(server) && g_get_monotonic_time() < deadline) spin(5);
  g_assert_false(pumpkin_server_get_running(server));
  g_assert_true(g_file_test(parked, G_FILE_TEST_EXISTS));
  remove_tree(root);
}

static void count_raw_output(PumpkinServer *server, const char *line, guint *count)
{
  (void)server;
  (void)line;
  ++*count;
}

static void capture_notice(PumpkinServer *server, const char *message, char **notice)
{
  (void)server;
  g_free(*notice);
  *notice = g_strdup(message);
}

static void test_bridge_failure_notice(void)
{
  g_autofree char *root = g_dir_make_tmp("smashed-bridge-notice-XXXXXX", NULL);
  g_autoptr(PumpkinServer) server = pumpkin_server_new("notice", "Bridge notice");
  pumpkin_server_set_root_dir(server, root);
  g_autofree char *plugins = pumpkin_server_get_plugins_dir(server);
  g_autofree char *bridge = g_build_filename(plugins, PUMPKIN_MAP_BRIDGE_FILENAME, NULL);
  /* An unverified local plugin deterministically exercises the error path,
   * including when tests run with permissions that bypass filesystem modes. */
  const char *unverified = "Unverified local bridge";
  g_assert_true(g_file_set_contents(bridge, unverified, -1, NULL));
  guint raw_output = 0;
  g_autofree char *notice = NULL;
  g_signal_connect(server, "log-line", G_CALLBACK(count_raw_output), &raw_output);
  g_signal_connect(server, "console-notice", G_CALLBACK(capture_notice), &notice);
  pumpkin_minimap_bridge_set_enabled(server, FALSE);
  g_assert_nonnull(notice);
  g_assert_cmpstr(notice, !=, "");
  g_assert_cmpuint(raw_output, ==, 0);
  g_assert_false(pumpkin_server_get_running(server));
  g_autofree char *contents = NULL;
  g_assert_true(g_file_get_contents(bridge, &contents, NULL, NULL));
  g_assert_cmpstr(contents, ==, unverified);
  g_signal_handlers_disconnect_by_data(server, &raw_output);
  g_signal_handlers_disconnect_by_data(server, &notice);
  remove_tree(root);
}

int main(int argc, char **argv)
{
  g_assert_cmpint(argc, >=, 2);
  fake_binary = argv[1];
  g_test_init(&argc, &argv, NULL);
  smashed_pumpkin_register_resource();
  g_test_add_func("/minimap/bridge-runtime-control", test_bridge_control);
  g_test_add_func("/minimap/bridge-failure-notice", test_bridge_failure_notice);
  int result = g_test_run();
  smashed_pumpkin_unregister_resource();
  return result;
}
