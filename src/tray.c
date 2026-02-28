#include "config.h"

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include "smashed-pumpkin-resources.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
spawn_command(const char *arg)
{
  g_autofree char *binary = g_find_program_in_path("smashed-pumpkin");
  if (binary == NULL) {
#if defined(__linux__)
    g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe != NULL) {
      g_autofree char *exe_dir = g_path_get_dirname(exe);
      g_autofree char *sibling = g_build_filename(exe_dir, "smashed-pumpkin", NULL);
      if (g_file_test(sibling, G_FILE_TEST_IS_EXECUTABLE)) {
        binary = g_steal_pointer(&sibling);
      }
    }
#endif
  }
  if (binary == NULL) {
    g_autofree char *cwd = g_get_current_dir();
    g_autofree char *candidate = g_build_filename(cwd, "buildDir", "src", "smashed-pumpkin", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
      binary = g_steal_pointer(&candidate);
    }
  }
  if (binary == NULL) {
    g_warning("Could not find smashed-pumpkin binary for tray action");
    return;
  }

  const char *argv[3] = {binary, arg, NULL};
  if (arg == NULL) {
    argv[1] = NULL;
  }
  g_autoptr(GError) error = NULL;
  if (!g_spawn_async(NULL, (char **)argv, NULL, 0, NULL, NULL, NULL, &error)) {
    g_warning("Failed to launch smashed-pumpkin from tray: %s",
              error != NULL ? error->message : "unknown error");
  }
}

static void
on_appindicator_log(const gchar *log_domain,
                    GLogLevelFlags log_level,
                    const gchar *message,
                    gpointer user_data)
{
  (void)user_data;
  if (message != NULL &&
      strstr(message, "is deprecated") != NULL &&
      strstr(message, "libayatana-appindicator-glib") != NULL) {
    return;
  }
  g_log_default_handler(log_domain, log_level, message, NULL);
}

static void
on_show_activate(GtkMenuItem *item, gpointer user_data)
{
  (void)item;
  (void)user_data;
  spawn_command("--show");
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

  GtkWidget *open_item = gtk_menu_item_new_with_label("Open");
  GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
  g_signal_connect(open_item, "activate", G_CALLBACK(on_show_activate), NULL);
  g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_activate), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);
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

static gboolean
icon_exists_in_dir(const char *dir)
{
  if (dir == NULL || *dir == '\0') {
    return FALSE;
  }
  g_autofree char *svg = g_build_filename(dir, APP_ID ".svg", NULL);
  g_autofree char *png = g_build_filename(dir, APP_ID ".png", NULL);
  return g_file_test(svg, G_FILE_TEST_EXISTS) || g_file_test(png, G_FILE_TEST_EXISTS);
}

static char *
resolve_flatpak_icon_theme_path(void)
{
  /* Inside Flatpak the installed icon lives under /app/share/icons/ which is
     only visible inside the sandbox.  The StatusNotifierWatcher (e.g. the
     GNOME Shell AppIndicator extension) runs on the host and cannot read that
     path.  Copy the icon to XDG_DATA_HOME/icons/… which is bind-mounted and
     therefore accessible from both the sandbox and the host. */
  const char *data_home = g_get_user_data_dir();
  g_autofree char *dest_dir =
    g_build_filename(data_home, "icons", "hicolor", "scalable", "apps", NULL);
  g_autofree char *src =
    g_build_filename("/app", "share", "icons", "hicolor", "scalable", "apps",
                     APP_ID ".svg", NULL);

  if (!g_file_test(src, G_FILE_TEST_EXISTS)) {
    return NULL;
  }

  g_mkdir_with_parents(dest_dir, 0755);

  g_autofree char *dest = g_build_filename(dest_dir, APP_ID ".svg", NULL);
  g_autofree char *contents = NULL;
  gsize len = 0;
  if (g_file_get_contents(src, &contents, &len, NULL)) {
    g_file_set_contents(dest, contents, (gssize)len, NULL);
  }

  if (icon_exists_in_dir(dest_dir)) {
    return g_steal_pointer(&dest_dir);
  }
  return NULL;
}

static char *
resolve_icon_theme_path(void)
{
  if (g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS)) {
    return resolve_flatpak_icon_theme_path();
  }

#if defined(__linux__)
  g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe != NULL) {
    g_autofree char *exe_dir = g_path_get_dirname(exe);
    g_autofree char *from_install = g_build_filename(exe_dir, "..", "share", "icons", "hicolor", "scalable", "apps", NULL);
    if (icon_exists_in_dir(from_install)) {
      return g_canonicalize_filename(from_install, NULL);
    }
    g_autofree char *from_build = g_build_filename(exe_dir, "..", "..", "resources", "icons", "hicolor", "scalable", "apps", NULL);
    if (icon_exists_in_dir(from_build)) {
      return g_canonicalize_filename(from_build, NULL);
    }
  }
#endif

  g_autofree char *cwd = g_get_current_dir();
  g_autofree char *local = g_build_filename(cwd, "resources", "icons", "hicolor", "scalable", "apps", NULL);
  if (icon_exists_in_dir(local)) {
    return g_steal_pointer(&local);
  }
  g_autofree char *parent = g_build_filename(cwd, "..", "resources", "icons", "hicolor", "scalable", "apps", NULL);
  if (icon_exists_in_dir(parent)) {
    return g_steal_pointer(&parent);
  }

  return NULL;
}

int
main(int argc, char *argv[])
{
  g_log_set_handler("libayatana-appindicator", G_LOG_LEVEL_WARNING, on_appindicator_log, NULL);
  g_log_set_handler("libappindicator", G_LOG_LEVEL_WARNING, on_appindicator_log, NULL);
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

  g_autofree char *icon_theme_path = resolve_icon_theme_path();
  if (icon_theme_path != NULL) {
    app_indicator_set_icon_theme_path(indicator, icon_theme_path);
  }
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
