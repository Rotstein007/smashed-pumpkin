#include "app.h"
#include "config.h"
#include "window.h"

struct _PumpkinApp {
  AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE(PumpkinApp, pumpkin_app, ADW_TYPE_APPLICATION)

static void
pumpkin_app_activate(GApplication *app)
{
  GtkWindow *win = GTK_WINDOW(pumpkin_window_new(ADW_APPLICATION(app)));
  gtk_window_set_default_icon_name(APP_ID);
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
}

static void
pumpkin_app_class_init(PumpkinAppClass *class)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS(class);

  app_class->activate = pumpkin_app_activate;
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
                      "flags", G_APPLICATION_DEFAULT_FLAGS,
                      NULL);
}
