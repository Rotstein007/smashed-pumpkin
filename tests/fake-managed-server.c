#include <glib.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
  gboolean ignore_stop = g_strcmp0(g_getenv("SMASHED_TEST_SERVER_MODE"), "ignore-stop") == 0;
  puts("INFO pumpkin::server: Ready");
  fflush(stdout);
  char command[256];
  while (fgets(command, sizeof(command), stdin) != NULL) {
    if (strcmp(command, "crash\n") == 0) return 7;
    if (strcmp(command, "buffered-exit-output\n") == 0) {
      for (guint i = 0; i < 200; ++i) printf("Buffered exit output: %03u\n", i);
      fflush(stdout);
      if (!g_file_set_contents("buffered-output-written", "written", -1, NULL)) return 2;
      return 0;
    }
    if (strcmp(command, "lifecycle-log-collision\n") == 0) {
      puts("Server process exited");
      puts("Auto-restart scheduled");
      puts("Serverprozess beendet");
      fflush(stdout);
    }
    if (g_str_has_prefix(command, "plugin ")) {
      FILE *journal = fopen("bridge-commands", "a");
      if (journal == NULL) return 2;
      gboolean written = fputs(command, journal) >= 0;
      if (fclose(journal) != 0 || !written) return 2;
    }
    if (strcmp(command, "plugin unload map-bridge\n") == 0) {
      g_usleep(100000);
      if (!g_file_set_contents("bridge-unloaded-marker", "unloaded", -1, NULL)) return 2;
      puts("INFO pumpkin::plugin: Plugin map-bridge unloaded successfully");
      fflush(stdout);
    } else if (strcmp(command, "plugin load plugins/map-bridge.wasm\n") == 0) {
      if (!g_file_set_contents("bridge-loaded-marker", "loaded", -1, NULL)) return 2;
      puts("INFO pumpkin::plugin: Plugin plugins/map-bridge.wasm loaded successfully");
      fflush(stdout);
    }
    if (strcmp(command, "stop\n") == 0 && !ignore_stop) {
      g_usleep(100000);
      if (!g_file_set_contents("saved-marker", "saved", -1, NULL)) {
        return 2;
      }
      puts("INFO pumpkin::server: Saved world");
      return 0;
    }
  }
  return 0;
}
