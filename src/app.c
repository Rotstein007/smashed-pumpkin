#include "app.h"
#include "app-config.h"
#include "config.h"
#include "window.h"

#include <stdint.h>
#include <string.h>
#if defined(G_OS_WIN32)
#include <windows.h>
#include <process.h>
#include <windowsx.h>
#else
#include <unistd.h>
#endif

struct _PumpkinApp {
  AdwApplication parent_instance;
  char *pending_server_id;
  gboolean start_minimized;
  gboolean first_activation;
  gboolean auto_start_pending;
  gboolean review_prompt_shown;
};

G_DEFINE_FINAL_TYPE(PumpkinApp, pumpkin_app, ADW_TYPE_APPLICATION)

static GPid tray_pid = 0;
static gboolean tray_spawned = FALSE;
static guint tray_watch_id = 0;
static gboolean tray_available = FALSE;

#if defined(G_OS_WIN32)
static HWND tray_control_hwnd = NULL;
static UINT tray_control_show_msg = 0;
static UINT tray_control_quit_msg = 0;
static const char *tray_control_window_class = "SmashedPumpkinControlWindow";
#endif

static GtkWindow *
find_existing_window(GtkApplication *app)
{
  GList *windows = gtk_application_get_windows(app);
  for (GList *l = windows; l != NULL; l = l->next) {
    if (GTK_IS_WINDOW(l->data)) {
      return GTK_WINDOW(l->data);
    }
  }

  return NULL;
}

#if defined(G_OS_WIN32)
static gboolean
present_existing_window_idle(gpointer data)
{
  PumpkinApp *app = PUMPKIN_APP(data);
  GtkWindow *win = find_existing_window(GTK_APPLICATION(app));

  if (win == NULL) {
    g_application_activate(G_APPLICATION(app));
  } else {
    gtk_widget_set_visible(GTK_WIDGET(win), TRUE);
    gtk_window_present(win);
  }

  g_object_unref(app);
  return G_SOURCE_REMOVE;
}

static gboolean
quit_application_idle(gpointer data)
{
  PumpkinApp *app = PUMPKIN_APP(data);
  g_action_group_activate_action(G_ACTION_GROUP(app), "quit", NULL);
  g_object_unref(app);
  return G_SOURCE_REMOVE;
}

static LRESULT CALLBACK
tray_control_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
  (void)w_param;
  (void)l_param;

  if (msg == WM_NCCREATE) {
    CREATESTRUCTA *create = (CREATESTRUCTA *)l_param;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    return TRUE;
  }

  PumpkinApp *app = (PumpkinApp *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
  if (app != NULL) {
    if (msg == tray_control_show_msg) {
      g_idle_add_full(G_PRIORITY_DEFAULT, present_existing_window_idle, g_object_ref(app), NULL);
      return 0;
    }
    if (msg == tray_control_quit_msg) {
      g_idle_add_full(G_PRIORITY_DEFAULT, quit_application_idle, g_object_ref(app), NULL);
      return 0;
    }
  }

  return DefWindowProcA(hwnd, msg, w_param, l_param);
}

static void
ensure_tray_control_window(PumpkinApp *app)
{
  HINSTANCE instance;
  WNDCLASSEXA window_class = {0};

  if (tray_control_show_msg == 0) {
    tray_control_show_msg = RegisterWindowMessageW(L"Rotstein.SmashedPumpkin.Tray.Show");
  }
  if (tray_control_quit_msg == 0) {
    tray_control_quit_msg = RegisterWindowMessageW(L"Rotstein.SmashedPumpkin.Tray.Quit");
  }
  if (tray_control_hwnd != NULL) {
    return;
  }

  instance = GetModuleHandleA(NULL);
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = tray_control_window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = tray_control_window_class;
  RegisterClassExA(&window_class);

  tray_control_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW,
                                      tray_control_window_class,
                                      "",
                                      WS_POPUP,
                                      0,
                                      0,
                                      0,
                                      0,
                                      NULL,
                                      NULL,
                                      instance,
                                      app);
}

static void
destroy_tray_control_window(void)
{
  if (tray_control_hwnd != NULL) {
    DestroyWindow(tray_control_hwnd);
    tray_control_hwnd = NULL;
  }
}
#endif

static void
present_review_prompt(PumpkinApp *self, GtkWindow *win)
{
  if (self == NULL || win == NULL || self->review_prompt_shown) {
    return;
  }

  self->review_prompt_shown = TRUE;

  gboolean is_de = FALSE;
  const char *const *langs = g_get_language_names();
  for (const char *const *lang = langs; lang != NULL && *lang != NULL; lang++) {
    if (g_str_has_prefix(*lang, "de")) {
      is_de = TRUE;
      break;
    }
  }

  AdwAlertDialog *alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(
    is_de ? "Gib uns Feedback" : "Share your feedback",
    is_de ? "Du kannst die App in GNOME Software oder KDE Discover bewerten."
          : "You can rate the app in GNOME Software or KDE Discover."
  ));
  adw_alert_dialog_add_response(alert, "close", is_de ? "Schließen" : "Close");
  adw_alert_dialog_set_default_response(alert, "close");
  adw_alert_dialog_set_close_response(alert, "close");

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(content, 6);
  gtk_widget_set_margin_bottom(content, 6);

  GtkWidget *icons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
  gtk_widget_set_halign(icons, GTK_ALIGN_CENTER);

  GtkWidget *gnome_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign(gnome_box, GTK_ALIGN_CENTER);
  GtkWidget *gnome_icon = gtk_image_new_from_resource("/dev/rotstein/SmashedPumpkin/store/org.gnome.Software.svg");
  gtk_image_set_pixel_size(GTK_IMAGE(gnome_icon), 64);
  GtkWidget *gnome_label = gtk_label_new("GNOME Software");
  gtk_box_append(GTK_BOX(gnome_box), gnome_icon);
  gtk_box_append(GTK_BOX(gnome_box), gnome_label);

  GtkWidget *discover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign(discover_box, GTK_ALIGN_CENTER);
  GtkWidget *discover_icon = gtk_image_new_from_resource("/dev/rotstein/SmashedPumpkin/store/org.kde.discover.svg");
  gtk_image_set_pixel_size(GTK_IMAGE(discover_icon), 64);
  GtkWidget *discover_label = gtk_label_new("KDE Discover");
  gtk_box_append(GTK_BOX(discover_box), discover_icon);
  gtk_box_append(GTK_BOX(discover_box), discover_label);

  gtk_box_append(GTK_BOX(icons), gnome_box);
  gtk_box_append(GTK_BOX(icons), discover_box);

  gtk_box_append(GTK_BOX(content), icons);
  adw_alert_dialog_set_extra_child(alert, content);
  adw_dialog_present(ADW_DIALOG(alert), GTK_WIDGET(win));
}

static void
maybe_present_review_prompt(PumpkinApp *self, GtkWindow *win)
{
  if (self == NULL || win == NULL || self->review_prompt_shown) {
    return;
  }
  if (!PUMPKIN_IS_WINDOW(win)) {
    return;
  }
  if (pumpkin_window_get_review_prompt_shown(PUMPKIN_WINDOW(win))) {
    self->review_prompt_shown = TRUE;
    return;
  }
  pumpkin_window_set_review_prompt_shown(PUMPKIN_WINDOW(win), TRUE);
  present_review_prompt(self, win);
}

static char *
resolve_tray_helper_path(void)
{
  g_autofree char *path = g_find_program_in_path("smashed-pumpkin-tray");
  if (path != NULL) {
    return g_steal_pointer(&path);
  }

#if defined(G_OS_WIN32)
  char exe_path[MAX_PATH] = {0};
  DWORD written = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
  if (written > 0 && written < MAX_PATH) {
    g_autofree char *exe_dir = g_path_get_dirname(exe_path);
    g_autofree char *sibling = g_build_filename(exe_dir, "smashed-pumpkin-tray.exe", NULL);
    if (g_file_test(sibling, G_FILE_TEST_EXISTS)) {
      return g_steal_pointer(&sibling);
    }
  }
#endif

#if defined(__linux__)
  g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe != NULL) {
    g_autofree char *exe_dir = g_path_get_dirname(exe);
    g_autofree char *sibling = g_build_filename(exe_dir, "smashed-pumpkin-tray", NULL);
    if (g_file_test(sibling, G_FILE_TEST_IS_EXECUTABLE)) {
      return g_steal_pointer(&sibling);
    }
  }
#endif

  g_autofree char *cwd = g_get_current_dir();
#if defined(G_OS_WIN32)
  g_autofree char *candidate = g_build_filename(cwd, "buildDir", "src", "smashed-pumpkin-tray.exe", NULL);
#else
  g_autofree char *candidate = g_build_filename(cwd, "buildDir", "src", "smashed-pumpkin-tray", NULL);
#endif
#if defined(G_OS_WIN32)
  if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
#else
  if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
#endif
    return g_steal_pointer(&candidate);
  }

  return NULL;
}

static void
on_tray_exit(GPid pid, int status, gpointer user_data)
{
  (void)status;
  (void)user_data;
  if (pid == tray_pid) {
    tray_pid = 0;
    tray_spawned = FALSE;
    tray_available = FALSE;
    tray_watch_id = 0;
  }
  g_spawn_close_pid(pid);
}

static void
spawn_tray_helper(void)
{
  if (tray_spawned) {
    return;
  }
  g_autofree char *tray_path = resolve_tray_helper_path();
  if (tray_path == NULL) {
#if defined(__linux__) || defined(G_OS_WIN32)
    g_warning("Could not find smashed-pumpkin-tray helper");
#else
    g_debug("No tray helper found for this platform");
#endif
    return;
  }
#if defined(G_OS_WIN32)
  int parent_pid = (int)_getpid();
#else
  int parent_pid = (int)getpid();
#endif
  char *arg = g_strdup_printf("--parent-pid=%d", parent_pid);
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
  tray_available = TRUE;
  tray_watch_id = g_child_watch_add(tray_pid, on_tray_exit, NULL);
  g_free(arg);
}

static gboolean
tray_should_be_enabled(PumpkinApp *app)
{
  PumpkinConfig *config = pumpkin_config_load(NULL);
  if (config == NULL) {
    return TRUE;
  }

  gboolean enabled = pumpkin_config_get_run_in_background(config) ||
                     pumpkin_config_get_start_minimized(config) ||
                     app->start_minimized;
  pumpkin_config_free(config);
  return enabled;
}

static void
stop_tray_helper(void)
{
  if (!tray_spawned || tray_pid == 0) {
    return;
  }

  if (tray_watch_id > 0) {
    g_source_remove(tray_watch_id);
    tray_watch_id = 0;
  }

#if defined(G_OS_WIN32)
  HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)(uintptr_t)tray_pid);
  if (process != NULL) {
    TerminateProcess(process, 0);
    CloseHandle(process);
  }
#else
  kill(tray_pid, SIGTERM);
#endif
  g_spawn_close_pid(tray_pid);
  tray_pid = 0;
  tray_spawned = FALSE;
  tray_available = FALSE;
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

gboolean
pumpkin_app_is_tray_available(PumpkinApp *app)
{
  (void)app;
  return tray_available;
}

static void
pumpkin_app_activate(GApplication *app)
{
  PumpkinApp *self = PUMPKIN_APP(app);
  GtkApplication *gtk_app = GTK_APPLICATION(app);
  GtkWindow *win = gtk_application_get_active_window(gtk_app);
  if (win == NULL) {
    win = find_existing_window(gtk_app);
  }
  if (win == NULL) {
    win = GTK_WINDOW(pumpkin_window_new(ADW_APPLICATION(app)));
    gtk_window_set_default_icon_name(APP_ID);
  }
#if defined(G_OS_WIN32)
  ensure_tray_control_window(self);
#endif
  if (!tray_spawned && tray_should_be_enabled(self)) {
    pumpkin_app_set_tray_enabled(self, TRUE);
  }
  if (self->start_minimized && self->first_activation) {
    self->first_activation = FALSE;
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
  } else {
    gtk_window_present(win);
  }
  if (self->auto_start_pending && PUMPKIN_IS_WINDOW(win)) {
    pumpkin_window_schedule_auto_start_servers(PUMPKIN_WINDOW(win));
    self->auto_start_pending = FALSE;
  }
  if (self->pending_server_id != NULL) {
    pumpkin_window_select_server(PUMPKIN_WINDOW(win), self->pending_server_id);
    g_clear_pointer(&self->pending_server_id, g_free);
  }
  if (gtk_widget_get_visible(GTK_WIDGET(win))) {
    maybe_present_review_prompt(self, win);
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
  gboolean is_de = FALSE;
  const char *const *langs = g_get_language_names();
  for (const char *const *lang = langs; lang != NULL && *lang != NULL; lang++) {
    if (g_str_has_prefix(*lang, "de")) {
      is_de = TRUE;
      break;
    }
  }
  adw_about_dialog_set_application_name(about, APP_NAME);
  adw_about_dialog_set_application_icon(about, APP_ID);
  adw_about_dialog_set_version(about, APP_VERSION);
  adw_about_dialog_set_release_notes_version(about, "0.5.2");
  if (is_de) {
    adw_about_dialog_set_release_notes(
      about,
      "<p>Neuerungen</p>"
      "<ul>"
      "<li>Windows-Portierung.</li>"
      "</ul>"
      "<p>Vielen Dank für euer Feedback. Es hilft der Entwicklung direkt weiter.</p>");
  } else {
    adw_about_dialog_set_release_notes(
      about,
      "<p>What’s new</p>"
      "<ul>"
      "<li>Windows port.</li>"
      "</ul>"
      "<p>Thank you for your feedback. It directly helps development.</p>");
  }
  adw_about_dialog_set_developer_name(about, "Rotstein");
  adw_about_dialog_set_comments(about,
                                is_de
                                  ? "Dies ist ein unabhängiges Tool und steht nicht in Verbindung mit PumpkinMC. "
                                    "Die folgenden Links führen zum PumpkinMC-Projekt."
                                  : "This is an independend tool and is not related to PumpkinMC. "
                                    "The following links lead to the PumpkinMC project.");
  adw_about_dialog_set_support_url(about, "https://github.com/Rotstein007/smashed-pumpkin");
  adw_about_dialog_set_issue_url(about, "https://github.com/Rotstein007/smashed-pumpkin/issues");
  adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_link(about, is_de ? "Webseite" : "Website", "https://pumpkinmc.org/");
  adw_about_dialog_add_link(about, is_de ? "Mitwirken" : "Contribute",
                            "https://docs.pumpkinmc.org/developer/contributing");
  adw_about_dialog_add_link(about, "Issues", "https://github.com/Pumpkin-MC/Pumpkin/issues");
  adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(win));
}

static void
pumpkin_app_quit_action(GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  (void)action;
  (void)parameter;
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(user_data));
  for (GList *l = windows; l != NULL; l = l->next) {
    if (PUMPKIN_IS_WINDOW(l->data)) {
      pumpkin_window_stop_all_servers(PUMPKIN_WINDOW(l->data));
    }
  }
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
  gboolean got_minimized = FALSE;
  gboolean show_requested = FALSE;
  const char *server_id = NULL;
  const char *server_name = NULL;
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--quit") == 0) {
      should_quit = TRUE;
      break;
    }
    if (g_strcmp0(argv[i], "--show") == 0) {
      show_requested = TRUE;
      continue;
    }
    if (g_strcmp0(argv[i], "--minimized") == 0) {
      got_minimized = TRUE;
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
  if (got_minimized) {
    self->start_minimized = TRUE;
  }
  if (show_requested ||
      (server_id != NULL && *server_id != '\0') ||
      (server_name != NULL && *server_name != '\0')) {
    self->start_minimized = FALSE;
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

  /* Auto-start servers on launch */
  PumpkinConfig *config = pumpkin_config_load(NULL);
  if (config != NULL) {
    if (pumpkin_config_get_autostart_on_boot(config) &&
        pumpkin_config_get_auto_start_servers_enabled(config)) {
      PUMPKIN_APP(app)->auto_start_pending = TRUE;
    }
    pumpkin_config_free(config);
  }
}

static void
pumpkin_app_dispose(GObject *object)
{
  PumpkinApp *self = PUMPKIN_APP(object);
  g_clear_pointer(&self->pending_server_id, g_free);
#if defined(G_OS_WIN32)
  destroy_tray_control_window();
#endif
  G_OBJECT_CLASS(pumpkin_app_parent_class)->dispose(object);
}

static void
pumpkin_app_shutdown(GApplication *app)
{
  stop_tray_helper();
#if defined(G_OS_WIN32)
  destroy_tray_control_window();
#endif
  G_APPLICATION_CLASS(pumpkin_app_parent_class)->shutdown(app);
}

static void
pumpkin_app_class_init(PumpkinAppClass *class)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS(class);
  GObjectClass *object_class = G_OBJECT_CLASS(class);

  app_class->activate = pumpkin_app_activate;
  app_class->command_line = pumpkin_app_command_line;
  app_class->startup = pumpkin_app_startup;
  app_class->shutdown = pumpkin_app_shutdown;
  object_class->dispose = pumpkin_app_dispose;
}

static void
pumpkin_app_init(PumpkinApp *self)
{
  self->first_activation = TRUE;
  self->auto_start_pending = FALSE;
  self->review_prompt_shown = FALSE;
}

PumpkinApp *
pumpkin_app_new(void)
{
  return g_object_new(PUMPKIN_TYPE_APP,
                      "application-id", APP_ID,
                      "flags", G_APPLICATION_DEFAULT_FLAGS | G_APPLICATION_HANDLES_COMMAND_LINE,
                      NULL);
}

void
pumpkin_app_schedule_auto_start_servers(PumpkinApp *app)
{
  if (app == NULL) {
    return;
  }
  app->auto_start_pending = TRUE;

  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (win != NULL && PUMPKIN_IS_WINDOW(win)) {
    pumpkin_window_schedule_auto_start_servers(PUMPKIN_WINDOW(win));
    app->auto_start_pending = FALSE;
  }
}
