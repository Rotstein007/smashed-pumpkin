#include "app.h"
#include "app-config.h"
#include "config.h"
#include "window.h"

#include <string.h>
#include <unistd.h>

struct _PumpkinApp {
  AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE(PumpkinApp, pumpkin_app, ADW_TYPE_APPLICATION)

static void
spawn_tray_helper(void)
{
  static gboolean spawned = FALSE;
  if (spawned) {
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
  spawned = TRUE;
  char *arg = g_strdup_printf("--parent-pid=%d", (int)getpid());
  gchar *argv[] = { tray_path, arg, NULL };
  GError *error = NULL;
  if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
    g_warning("Failed to start tray helper: %s", error->message);
    g_clear_error(&error);
    g_free(arg);
    return;
  }
  g_free(arg);
}

static void
pumpkin_app_activate(GApplication *app)
{
  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (win == NULL) {
    win = GTK_WINDOW(pumpkin_window_new(ADW_APPLICATION(app)));
    gtk_window_set_default_icon_name(APP_ID);
  }
  gtk_window_present(win);
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
                                "Smashed Pumpkin is an independent community tool. "
                                "It is not affiliated with, endorsed by, or sponsored by the PumpkinMC project.");
  adw_about_dialog_set_website(about, "https://github.com/Pumpkin-MC/Pumpkin");
  adw_about_dialog_set_issue_url(about, "https://github.com/Pumpkin-MC/Pumpkin/issues");
  adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(win));
}

static void
pumpkin_app_quit_action(GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  (void)action;
  (void)parameter;
  g_application_quit(G_APPLICATION(user_data));
}

static const GActionEntry app_actions[] = {
  { .name = "about", .activate = pumpkin_app_about_action, .parameter_type = NULL, .state = NULL, .change_state = NULL, .padding = { 0 } },
  { .name = "quit", .activate = pumpkin_app_quit_action, .parameter_type = NULL, .state = NULL, .change_state = NULL, .padding = { 0 } },
};

static int
pumpkin_app_command_line(GApplication *app, GApplicationCommandLine *command_line)
{
  int argc = 0;
  char **argv = g_application_command_line_get_arguments(command_line, &argc);
  gboolean should_quit = FALSE;
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--quit") == 0) {
      should_quit = TRUE;
      break;
    }
  }
  g_strfreev(argv);

  if (should_quit) {
    g_action_group_activate_action(G_ACTION_GROUP(app), "quit", NULL);
    return 0;
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

  PumpkinConfig *config = pumpkin_config_load(NULL);
  if (config != NULL && pumpkin_config_get_run_in_background(config)) {
    spawn_tray_helper();
  }
  if (config != NULL) {
    pumpkin_config_free(config);
  }
}

static void
pumpkin_app_class_init(PumpkinAppClass *class)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS(class);

  app_class->activate = pumpkin_app_activate;
  app_class->command_line = pumpkin_app_command_line;
  app_class->startup = pumpkin_app_startup;
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
