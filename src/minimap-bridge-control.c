#include "minimap-bridge-control.h"
#include "minimap-store.h"
#include <string.h>

typedef enum { BRIDGE_IDLE, BRIDGE_LOADING, BRIDGE_UNLOADING } BridgeOperation;
typedef struct {
  gboolean enabled, loaded, failed;
  int pid;
  char *name, *pending_target;
  BridgeOperation pending;
  guint timeout;
} BridgeControl;

static void sync_bridge(PumpkinServer *server, BridgeControl *control);

static void clear_timeout(BridgeControl *control)
{
  if (control->timeout != 0) g_source_remove(control->timeout);
  control->timeout = 0;
}

static void control_free(gpointer data)
{
  BridgeControl *control = data;
  clear_timeout(control);
  g_free(control->name);
  g_free(control->pending_target);
  g_free(control);
}

static void report_failure(PumpkinServer *server, BridgeControl *control, const char *detail)
{
  control->failed = TRUE;
  g_autofree char *message = g_strdup_printf("[SMPK] Minimap Bridge: %s. %s", detail,
    control->enabled ? "Live players may require a server restart." :
    "The bridge is disabled for the next start; restart the server if it could not be unloaded.");
  g_signal_emit_by_name(server, "console-notice", message);
}

static gboolean timed_out(gpointer data)
{
  PumpkinServer *server = data;
  BridgeControl *control = g_object_get_data(G_OBJECT(server), "minimap-bridge-control");
  control->timeout = 0;
  control->pending = BRIDGE_IDLE;
  g_clear_pointer(&control->pending_target, g_free);
  report_failure(server, control, "Pumpkin did not acknowledge the plugin change");
  return G_SOURCE_REMOVE;
}

static void bridge_feedback(PumpkinServer *server, const char *line, BridgeControl *control)
{
  if (control->pending == BRIDGE_IDLE || line == NULL) return;
  gboolean loading = control->pending == BRIDGE_LOADING;
  g_autofree char *success = g_strdup_printf("Plugin %s %s successfully", control->pending_target, loading ? "loaded" : "unloaded");
  g_autofree char *already = g_strdup_printf("Plugin %s is %sloaded", control->pending_target, loading ? "already " : "not ");
  g_autofree char *failure = g_strdup_printf("Failed to %s plugin %s:", loading ? "load" : "unload", control->pending_target);
  if (strstr(line, success) != NULL || strstr(line, already) != NULL) {
    clear_timeout(control);
    control->loaded = loading;
    control->pending = BRIDGE_IDLE;
    g_clear_pointer(&control->pending_target, g_free);
    sync_bridge(server, control);
  } else if (strstr(line, failure) != NULL) {
    clear_timeout(control);
    control->pending = BRIDGE_IDLE;
    g_clear_pointer(&control->pending_target, g_free);
    report_failure(server, control, loading ? "Could not load the bridge" : "Could not unload the bridge");
  }
}

static void sync_bridge(PumpkinServer *server, BridgeControl *control)
{
  int pid = pumpkin_server_get_pid(server);
  if (pid != control->pid) {
    clear_timeout(control);
    control->pending = BRIDGE_IDLE;
    g_clear_pointer(&control->pending_target, g_free);
    control->failed = FALSE;
    control->pid = pid;
    control->loaded = control->enabled && pumpkin_server_get_running(server);
  }
  if (pumpkin_server_get_maintenance(server) || pumpkin_server_get_stopping(server) || control->failed) return;
  g_autofree char *directory = pumpkin_server_get_data_dir(server), *name = NULL;
  gboolean changed = FALSE;
  g_autoptr(GError) error = NULL;
  if (!pumpkin_map_bridge_set_enabled(directory, control->enabled, &name, &changed, &error)) {
    report_failure(server, control, error->message); return;
  }
  if (name == NULL || control->pending != BRIDGE_IDLE) return;
  if (control->name == NULL) {
    control->name = g_strdup(name);
    control->loaded = pumpkin_server_get_running(server) && (!control->enabled || !changed);
  }
  if (g_strcmp0(control->name, name) != 0) {
    g_free(control->name); control->name = g_strdup(name);
  }
  if (!pumpkin_server_get_running(server)) return;
  if (control->loaded == control->enabled) return;
  g_autofree char *canonical = g_build_filename(directory, "plugins", PUMPKIN_MAP_BRIDGE_FILENAME, NULL);
  const char *filename = g_file_test(canonical, G_FILE_TEST_EXISTS) ? PUMPKIN_MAP_BRIDGE_FILENAME : "smashed-map-bridge.wasm";
  control->pending_target = control->enabled ? g_strdup_printf("plugins/%s", filename) : g_strdup(control->name);
  g_autofree char *command = g_strdup_printf("plugin %s %s", control->enabled ? "load" : "unload", control->pending_target);
  control->pending = control->enabled ? BRIDGE_LOADING : BRIDGE_UNLOADING;
  control->timeout = g_timeout_add_full(G_PRIORITY_DEFAULT, 10000, timed_out, g_object_ref(server), g_object_unref);
  if (!pumpkin_server_send_command(server, command, &error)) {
    clear_timeout(control);
    control->pending = BRIDGE_IDLE;
    g_clear_pointer(&control->pending_target, g_free);
    report_failure(server, control, error->message);
  }
}

static void state_changed(PumpkinServer *server, BridgeControl *control)
{
  sync_bridge(server, control);
}

void pumpkin_minimap_bridge_set_enabled(PumpkinServer *server, gboolean enabled)
{
  BridgeControl *control = g_object_get_data(G_OBJECT(server), "minimap-bridge-control");
  if (control == NULL) {
    control = g_new0(BridgeControl, 1);
    control->pid = pumpkin_server_get_pid(server);
    g_object_set_data_full(G_OBJECT(server), "minimap-bridge-control", control, control_free);
    g_signal_connect(server, "log-line", G_CALLBACK(bridge_feedback), control);
    g_signal_connect(server, "state-changed", G_CALLBACK(state_changed), control);
  }
  control->enabled = enabled;
  control->failed = FALSE;
  sync_bridge(server, control);
}
