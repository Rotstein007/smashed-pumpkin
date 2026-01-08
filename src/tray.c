#include "config.h"

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include "smashed-pumpkin-resources.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
spawn_command(const char *arg)
{
  const char *argv[3] = {"smashed-pumpkin", arg, NULL};
  if (arg == NULL) {
    argv[1] = NULL;
  }
  g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void
on_show_activate(GtkMenuItem *item, gpointer user_data)
{
  (void)item;
  (void)user_data;
  spawn_command(NULL);
}

static void
on_quit_activate(GtkMenuItem *item, gpointer user_data)
{
  (void)item;
  (void)user_data;
  spawn_command("--quit");
  gtk_main_quit();
}

static gboolean
check_parent_alive(gpointer user_data)
{
  pid_t pid = GPOINTER_TO_INT(user_data);
  if (pid <= 0) {
    return G_SOURCE_CONTINUE;
  }
  if (kill(pid, 0) != 0 && errno == ESRCH) {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

int
main(int argc, char *argv[])
{
  gtk_init(&argc, &argv);
  smashed_pumpkin_register_resource();

  GtkIconTheme *theme = gtk_icon_theme_get_default();
  gtk_icon_theme_add_resource_path(theme, "/dev/rotstein/SmashedPumpkin/icons");

  pid_t parent_pid = 0;
  for (int i = 1; i < argc; i++) {
    if (g_str_has_prefix(argv[i], "--parent-pid=")) {
      parent_pid = (pid_t)atoi(argv[i] + strlen("--parent-pid="));
    }
  }

  AppIndicator *indicator = app_indicator_new(APP_ID, APP_ID, APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
  app_indicator_set_icon_full(indicator, APP_ID, "Smashed Pumpkin");

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *show_item = gtk_menu_item_new_with_label("Show");
  GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");

  g_signal_connect(show_item, "activate", G_CALLBACK(on_show_activate), NULL);
  g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_activate), NULL);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
  gtk_widget_show_all(menu);
  app_indicator_set_menu(indicator, GTK_MENU(menu));

  if (parent_pid > 0) {
    g_timeout_add_seconds(5, check_parent_alive, GINT_TO_POINTER(parent_pid));
  }

  gtk_main();
  return 0;
}
