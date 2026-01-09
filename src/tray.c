#include "config.h"

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include "smashed-pumpkin-resources.h"
#include "app-config.h"

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

static void
on_server_activate(GtkMenuItem *item, gpointer user_data)
{
  (void)user_data;
  const char *server_name = g_object_get_data(G_OBJECT(item), "server-name");
  if (server_name == NULL || *server_name == '\0') {
    spawn_command(NULL);
    return;
  }
  g_autofree char *arg = g_strdup_printf("--server-name=%s", server_name);
  spawn_command(arg);
}

typedef struct {
  char *id;
  char *name;
} TrayServer;

static void
tray_server_free(TrayServer *server)
{
  if (server == NULL) {
    return;
  }
  g_free(server->id);
  g_free(server->name);
  g_free(server);
}

static gint
tray_server_compare(gconstpointer a, gconstpointer b)
{
  const TrayServer *sa = *(const TrayServer *const *)a;
  const TrayServer *sb = *(const TrayServer *const *)b;
  if (sa == NULL || sa->name == NULL) {
    return -1;
  }
  if (sb == NULL || sb->name == NULL) {
    return 1;
  }
  return g_ascii_strcasecmp(sa->name, sb->name);
}

static char *
get_base_dir(void)
{
  PumpkinConfig *config = pumpkin_config_load(NULL);
  if (config != NULL) {
    char *base = g_strdup(pumpkin_config_get_base_dir(config));
    pumpkin_config_free(config);
    return base;
  }
  return g_build_filename(g_get_user_data_dir(), "smashed-pumpkin", "servers", NULL);
}

static GPtrArray *
load_servers(void)
{
  g_autofree char *base_dir = get_base_dir();
  GPtrArray *servers = g_ptr_array_new_with_free_func((GDestroyNotify)tray_server_free);
  if (base_dir == NULL) {
    return servers;
  }

  GDir *dir = g_dir_open(base_dir, 0, NULL);
  if (dir == NULL) {
    return servers;
  }

  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *server_dir = g_build_filename(base_dir, entry, NULL);
    g_autofree char *ini = g_build_filename(server_dir, "server.ini", NULL);
    if (!g_file_test(ini, G_FILE_TEST_EXISTS)) {
      continue;
    }
    g_autoptr(GKeyFile) key = g_key_file_new();
    if (!g_key_file_load_from_file(key, ini, G_KEY_FILE_NONE, NULL)) {
      continue;
    }
    g_autofree char *id = g_key_file_get_string(key, "server", "id", NULL);
    g_autofree char *name = g_key_file_get_string(key, "server", "name", NULL);
    TrayServer *server = g_new0(TrayServer, 1);
    server->id = g_strdup(id != NULL ? id : entry);
    server->name = g_strdup(name != NULL ? name : entry);
    g_ptr_array_add(servers, server);
  }
  g_dir_close(dir);

  g_ptr_array_sort(servers, (GCompareFunc)tray_server_compare);
  return servers;
}

static void
clear_menu(GtkWidget *menu)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
  for (GList *l = children; l != NULL; l = l->next) {
    gtk_widget_destroy(GTK_WIDGET(l->data));
  }
  g_list_free(children);
}

static void
populate_menu(GtkWidget *menu)
{
  clear_menu(menu);

  GtkWidget *servers_header = gtk_menu_item_new_with_label("Servers");
  gtk_widget_set_sensitive(servers_header, FALSE);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), servers_header);

  g_autoptr(GPtrArray) servers = load_servers();
  if (servers->len == 0) {
    GtkWidget *empty = gtk_menu_item_new_with_label("No servers");
    gtk_widget_set_sensitive(empty, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), empty);
  } else {
    for (guint i = 0; i < servers->len; i++) {
      TrayServer *server = g_ptr_array_index(servers, i);
      GtkWidget *item = gtk_menu_item_new_with_label(server->name);
      g_object_set_data_full(G_OBJECT(item), "server-id", g_strdup(server->id), g_free);
      g_object_set_data_full(G_OBJECT(item), "server-name", g_strdup(server->name), g_free);
      g_signal_connect(item, "activate", G_CALLBACK(on_server_activate), NULL);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
  }

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  GtkWidget *show_item = gtk_menu_item_new_with_label("Show");
  GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
  g_signal_connect(show_item, "activate", G_CALLBACK(on_show_activate), NULL);
  g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_activate), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

  gtk_widget_show_all(menu);
}

static void
on_menu_show(GtkWidget *menu, gpointer user_data)
{
  (void)user_data;
  populate_menu(menu);
}

static void
on_menu_popped_up(GtkWidget *menu, gpointer user_data)
{
  (void)user_data;
  populate_menu(menu);
}

static gboolean
refresh_menu_cb(gpointer user_data)
{
  GtkWidget *menu = GTK_WIDGET(user_data);
  if (menu != NULL) {
    populate_menu(menu);
  }
  return G_SOURCE_CONTINUE;
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
  g_signal_connect(menu, "show", G_CALLBACK(on_menu_show), NULL);
  g_signal_connect(menu, "popped-up", G_CALLBACK(on_menu_popped_up), NULL);
  populate_menu(menu);
  app_indicator_set_menu(indicator, GTK_MENU(menu));
  g_timeout_add_seconds(5, refresh_menu_cb, menu);

  if (parent_pid > 0) {
    g_timeout_add_seconds(5, check_parent_alive, GINT_TO_POINTER(parent_pid));
  }

  gtk_main();
  return 0;
}
