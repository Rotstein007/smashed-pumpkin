#include "app.h"
#include "app-config.h"
#include "config.h"
#include "window.h"

#include <string.h>
#include <unistd.h>

struct _PumpkinApp {
  AdwApplication parent_instance;
  char *pending_server_id;
};

G_DEFINE_FINAL_TYPE(PumpkinApp, pumpkin_app, ADW_TYPE_APPLICATION)

static GPid tray_pid = 0;
static gboolean tray_spawned = FALSE;

static void
on_tray_exit(GPid pid, int status, gpointer user_data)
{
  (void)status;
  (void)user_data;
  if (pid == tray_pid) {
    tray_pid = 0;
    tray_spawned = FALSE;
  }
  g_spawn_close_pid(pid);
}

static void
spawn_tray_helper(void)
{
  if (tray_spawned) {
    return;
  }
  g_autofree char *tray_path = g_find_program_in_path("smashed-pumpkin-tray");
  if (tray_path == NULL) {
    g_autofree char *cwd = g_get_current_dir();
    g_autofree char *candidate = g_build_filename(cwd, "buildDir", "src", "smashed-pumpkin-tray", NULL);
    if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
      tray_path = g_strdup(candidate);
    }
  }
  if (tray_path == NULL) {
    g_warning("Could not find smashed-pumpkin-tray helper");
    return;
  }
  char *arg = g_strdup_printf("--parent-pid=%d", (int)getpid());
  gchar *argv[] = { tray_path, arg, NULL };
  GError *error = NULL;
  if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                     NULL, NULL, &tray_pid, &error)) {
    g_warning("Failed to start tray helper: %s", error->message);
    g_clear_error(&error);
    g_free(arg);
    return;
  }
  tray_spawned = TRUE;
  g_child_watch_add(tray_pid, on_tray_exit, NULL);
  g_free(arg);
}

static void
stop_tray_helper(void)
{
  if (!tray_spawned || tray_pid <= 0) {
    return;
  }
  kill(tray_pid, SIGTERM);
  g_spawn_close_pid(tray_pid);
  tray_pid = 0;
  tray_spawned = FALSE;
}

void
pumpkin_app_set_tray_enabled(PumpkinApp *app, gboolean enabled)
{
  (void)app;
  if (enabled) {
    spawn_tray_helper();
  } else {
    stop_tray_helper();
  }
}

static void
pumpkin_app_activate(GApplication *app)
{
  PumpkinApp *self = PUMPKIN_APP(app);
  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (win == NULL) {
    win = GTK_WINDOW(pumpkin_window_new(ADW_APPLICATION(app)));
    gtk_window_set_default_icon_name(APP_ID);
  }
  gtk_window_present(win);
  if (self->pending_server_id != NULL) {
    pumpkin_window_select_server(PUMPKIN_WINDOW(win), self->pending_server_id);
    g_clear_pointer(&self->pending_server_id, g_free);
  }
}

static void
pumpkin_app_about_action(GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  (void)action;
  (void)parameter;
  PumpkinApp *app = PUMPKIN_APP(user_data);
  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));

  AdwAboutDialog *about = ADW_ABOUT_DIALOG(adw_about_dialog_new());
  adw_about_dialog_set_application_name(about, APP_NAME);
  adw_about_dialog_set_application_icon(about, APP_ID);
  adw_about_dialog_set_version(about, APP_VERSION);
  adw_about_dialog_set_developer_name(about, "Rotstein");
  adw_about_dialog_set_comments(about,
                                "Independent community tool, not affiliated with PumpkinMC.");
  adw_about_dialog_set_website(about, "https://github.com/Rotstein007/smashed-pumpkin");
  adw_about_dialog_set_issue_url(about, "https://github.com/Rotstein007/smashed-pumpkin/issues");
  adw_about_dialog_add_link(about, "PumpkinMC · Website", "https://pumpkinmc.org/");
  adw_about_dialog_add_link(about, "PumpkinMC · Contribute", "https://docs.pumpkinmc.org/developer/contributing");
  adw_about_dialog_add_link(about, "PumpkinMC · Issues", "https://github.com/Pumpkin-MC/Pumpkin/issues");
  adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(win));
}

static void
pumpkin_app_quit_action(GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  (void)action;
  (void)parameter;
  stop_tray_helper();
  g_application_quit(G_APPLICATION(user_data));
}

static const GActionEntry app_actions[] = {
  { .name = "about", .activate = pumpkin_app_about_action, .parameter_type = NULL, .state = NULL, .change_state = NULL, .padding = { 0 } },
  { .name = "quit", .activate = pumpkin_app_quit_action, .parameter_type = NULL, .state = NULL, .change_state = NULL, .padding = { 0 } },
};

static int
pumpkin_app_command_line(GApplication *app, GApplicationCommandLine *command_line)
{
  PumpkinApp *self = PUMPKIN_APP(app);
  int argc = 0;
  char **argv = g_application_command_line_get_arguments(command_line, &argc);
  gboolean should_quit = FALSE;
  const char *server_id = NULL;
  const char *server_name = NULL;
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--quit") == 0) {
      should_quit = TRUE;
      break;
    }
    if (g_str_has_prefix(argv[i], "--server-id=")) {
      server_id = argv[i] + strlen("--server-id=");
    }
    if (g_str_has_prefix(argv[i], "--server-name=")) {
      server_name = argv[i] + strlen("--server-name=");
    }
  }
  g_strfreev(argv);

  if (should_quit) {
    g_action_group_activate_action(G_ACTION_GROUP(app), "quit", NULL);
    return 0;
  }

  if (server_id != NULL && *server_id != '\0') {
    g_free(self->pending_server_id);
    self->pending_server_id = g_strdup(server_id);
  } else if (server_name != NULL && *server_name != '\0') {
    g_free(self->pending_server_id);
    self->pending_server_id = g_strdup(server_name);
  }
  g_application_activate(app);
  return 0;
}

static void
pumpkin_app_startup(GApplication *app)
{
  G_APPLICATION_CLASS(pumpkin_app_parent_class)->startup(app);

  GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
  gtk_icon_theme_add_resource_path(theme, "/dev/rotstein/SmashedPumpkin/icons");
  gtk_icon_theme_add_resource_path(theme, "/dev/rotstein/SmashedPumpkin/icons/hicolor");
  gtk_icon_theme_add_resource_path(theme, "/dev/rotstein/SmashedPumpkin");

  g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), app);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", (const char*[]) { "<primary>q", NULL });

  pumpkin_app_set_tray_enabled(PUMPKIN_APP(app), TRUE);
}

static void
pumpkin_app_dispose(GObject *object)
{
  PumpkinApp *self = PUMPKIN_APP(object);
  g_clear_pointer(&self->pending_server_id, g_free);
  G_OBJECT_CLASS(pumpkin_app_parent_class)->dispose(object);
}

static void
pumpkin_app_class_init(PumpkinAppClass *class)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS(class);
  GObjectClass *object_class = G_OBJECT_CLASS(class);

  app_class->activate = pumpkin_app_activate;
  app_class->command_line = pumpkin_app_command_line;
  app_class->startup = pumpkin_app_startup;
  object_class->dispose = pumpkin_app_dispose;
}

static void
pumpkin_app_init(PumpkinApp *self)
{
  (void)self;
}

PumpkinApp *
pumpkin_app_new(void)
{
  return g_object_new(PUMPKIN_TYPE_APP,
                      "application-id", APP_ID,
                      "flags", G_APPLICATION_DEFAULT_FLAGS | G_APPLICATION_HANDLES_COMMAND_LINE,
                      NULL);
}
