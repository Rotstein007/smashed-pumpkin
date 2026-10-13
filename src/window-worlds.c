#include "window-worlds.h"
#include "world-store.h"
#include "refresh-button.h"
#include "platform.h"
#include "window-lifecycle.h"
#include "i18n.h"
#include <string.h>

typedef struct {
  GtkSearchEntry *search;
  GtkStack *stack;
  AdwStatusPage *status;
  GtkLabel *summary;
  GtkButton *import_button;
  GtkButton *refresh;
  GCancellable *scan;
  guint generation;
  guint deferred_refresh;
  gboolean pending_refresh;
} WorldsPage;

typedef struct {
  PumpkinWindow *window;
  PumpkinServer *server;
  char *path;
  guint generation;
  gboolean importing;
} WorldContext;

static WorldsPage *page_for(PumpkinWindow *self)
{
  return g_object_get_data(G_OBJECT(self), "worlds-page");
}

static WorldContext *context_new(PumpkinWindow *self, PumpkinServer *server, const char *path)
{
  WorldContext *context = g_new0(WorldContext, 1);
  context->window = g_object_ref(self);
  context->server = g_object_ref(server);
  context->path = g_strdup(path);
  return context;
}

static void context_free(WorldContext *context)
{
  g_clear_object(&context->server);
  g_clear_object(&context->window);
  g_free(context->path);
  g_free(context);
}

static void page_free(gpointer data)
{
  WorldsPage *page = data;
  g_clear_object(&page->scan);
  g_free(page);
}

static gboolean can_modify(PumpkinWindow *self, PumpkinServer *server)
{
  if (self->disposing || !pumpkin_platform_supports_local_hosting() || server == NULL ||
      server_is_running_ui(self, server) || pumpkin_server_get_stopping(server) ||
      pumpkin_server_get_maintenance(server)) return FALSE;
  if (self->restart_requested || self->restart_delay_id != 0 || self->start_delay_id != 0) return FALSE;
  DownloadProgressState *download = get_download_progress_state(self, server, FALSE);
  if (download != NULL && download->active) return FALSE;
  if (self->current == server && (self->ui_state == UI_STATE_STARTING ||
      self->ui_state == UI_STATE_STOPPING || self->ui_state == UI_STATE_RESTARTING)) return FALSE;
  return TRUE;
}

static void report(WorldContext *context, const char *message)
{
  if (context->window->disposing) return;
  append_log_for_server(context->window, context->server, message);
  if (context->window->current == context->server) set_details_status(context->window, message, 6);
}

static void show_status(WorldsPage *page, const char *icon, const char *title, const char *description)
{
  adw_status_page_set_icon_name(page->status, icon);
  adw_status_page_set_title(page->status, title);
  adw_status_page_set_description(page->status, description);
  gtk_stack_set_visible_child_name(page->stack, "status");
}

static gboolean filter_world(GtkListBoxRow *row, gpointer data)
{
  WorldsPage *page = data;
  g_autofree char *query = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(page->search)), -1);
  const char *search = g_object_get_data(G_OBJECT(row), "world-search");
  return query[0] == '\0' || (search != NULL && strstr(search, query) != NULL);
}

static void search_changed(GtkSearchEntry *entry, PumpkinWindow *self)
{
  (void)entry;
  gtk_list_box_invalidate_filter(self->world_list);
  WorldsPage *page = page_for(self);
  if (page->scan != NULL) return;
  guint total = 0, matching = 0;
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->world_list)); row != NULL;
       row = gtk_widget_get_next_sibling(row)) {
    if (!GTK_IS_LIST_BOX_ROW(row)) continue;
    total++;
    if (filter_world(GTK_LIST_BOX_ROW(row), page)) matching++;
  }
  g_autofree char *summary = matching == total ?
    g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u world", "%u worlds", total), total) :
    g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u of %u worlds", "%u of %u worlds", total),
                    matching, total);
  gtk_label_set_text(page->summary, summary);
}

static void open_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  WorldContext *context = data;
  g_autoptr(GError) error = NULL;
  if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error)) report(context, error->message);
  context_free(context);
}

static void open_world(GtkButton *button, PumpkinWindow *self)
{
  PumpkinServer *server = g_object_get_data(G_OBJECT(button), "world-server");
  const char *path = g_object_get_data(G_OBJECT(button), "world-path");
  g_autofree char *data_path = NULL;
  if (server == NULL) {
    server = self->current;
    if (server != NULL) {
      data_path = pumpkin_server_get_data_dir(server);
      path = data_path;
    }
  }
  if (server == NULL || path == NULL) return;
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
  gtk_file_launcher_launch(launcher, GTK_WINDOW(self), NULL, open_finished, context_new(self, server, path));
}

static void operation_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  WorldContext *context = data;
  PumpkinWindow *self = context->window;
  g_autoptr(GError) error = NULL;
  g_autofree char *imported = NULL;
  gboolean success;
  if (context->importing) {
    imported = pumpkin_world_import_finish(result, &error);
    success = imported != NULL;
  } else success = pumpkin_world_remove_finish(result, &error);
  pumpkin_server_end_maintenance(context->server);
  if (success) report(context, context->importing ?
    _("World imported. The configured world has not been changed.") : _("World removed"));
  else report(context, error->message);
  if (!self->disposing && self->current == context->server) refresh_world_list(self);
  context_free(context);
}

static gboolean start_operation(WorldContext *context, gboolean importing)
{
  PumpkinWindow *self = context->window;
  if (!can_modify(self, context->server)) {
    report(context, _("Stop the server and finish maintenance before modifying worlds."));
    return FALSE;
  }
  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_begin_maintenance(context->server, &error)) {
    report(context, error->message);
    return FALSE;
  }
  context->importing = importing;
  g_autofree char *data_dir = pumpkin_server_get_data_dir(context->server);
  if (importing) pumpkin_world_import_async(data_dir, context->path, operation_finished, context);
  else pumpkin_world_remove_async(data_dir, context->path, operation_finished, context);
  pumpkin_window_worlds_update_state(self);
  report(context, importing ? _("Importing world...") : _("Removing world..."));
  return TRUE;
}

static void remove_confirmed(GObject *source, GAsyncResult *result, gpointer data)
{
  WorldContext *context = data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  if (g_strcmp0(response, "remove") != 0 || !start_operation(context, FALSE)) context_free(context);
}

static void remove_clicked(GtkButton *button, PumpkinWindow *self)
{
  PumpkinServer *server = g_object_get_data(G_OBJECT(button), "world-server");
  const char *path = g_object_get_data(G_OBJECT(button), "world-path");
  if (path == NULL || !can_modify(self, server)) return;
  g_autofree char *name = g_filename_display_basename(path);
  g_autofree char *title = g_strdup_printf(_("Remove \"%s\"?"), name);
  gboolean configured = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "world-configured"));
  AdwDialog *dialog = adw_alert_dialog_new(title, configured ?
    _("This is the configured world. Removing it permanently deletes its data. The server may generate a new world on its next start. Make a backup first.") :
    _("This permanently deletes the world folder. Make a backup before continuing."));
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", _("Cancel"), "remove", _("Remove World"), NULL);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  adw_alert_dialog_set_response_appearance(alert, "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(alert, GTK_WIDGET(self), NULL, remove_confirmed, context_new(self, server, path));
}

static void import_selected(GObject *source, GAsyncResult *result, gpointer data)
{
  WorldContext *context = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file == NULL) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) report(context, error->message);
    context_free(context);
    return;
  }
  context->path = g_file_get_path(file);
  if (context->path == NULL) report(context, _("Only local world folders can be imported."));
  if (context->path == NULL || !start_operation(context, TRUE)) context_free(context);
}

static void import_clicked(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (!can_modify(self, self->current)) return;
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Import World Folder"));
  gtk_file_dialog_select_folder(dialog, GTK_WINDOW(self), NULL, import_selected,
                                context_new(self, self->current, NULL));
}

static gboolean worlds_drop(GtkDropTarget *target, const GValue *value, double x, double y, PumpkinWindow *self)
{
  (void)target;
  (void)x;
  (void)y;
  gtk_widget_remove_css_class(GTK_WIDGET(self->world_page), "drop-active");
  if (!can_modify(self, self->current)) {
    set_details_status(self, _("Stop the server before importing worlds"), 5);
    return FALSE;
  }
  GdkFileList *list = g_value_get_boxed(value);
  GSList *files = list != NULL ? gdk_file_list_get_files(list) : NULL;
  if (files == NULL || files->next != NULL) {
    set_details_status(self, _("Import one world folder at a time"), 5);
    return FALSE;
  }
  g_autofree char *path = g_file_get_path(G_FILE(files->data));
  if (path == NULL) {
    set_details_status(self, _("Only local world folders can be imported"), 5);
    return FALSE;
  }
  WorldContext *context = context_new(self, self->current, path);
  if (start_operation(context, TRUE)) return TRUE;
  context_free(context);
  return FALSE;
}

static GdkDragAction drop_enter(GtkDropTarget *target, double x, double y, PumpkinWindow *self)
{
  (void)target;
  (void)x;
  (void)y;
  if (!can_modify(self, self->current)) return 0;
  gtk_widget_add_css_class(GTK_WIDGET(self->world_page), "drop-active");
  return GDK_ACTION_COPY;
}

static void drop_leave(GtkDropTarget *target, PumpkinWindow *self)
{
  (void)target;
  gtk_widget_remove_css_class(GTK_WIDGET(self->world_page), "drop-active");
}

static gboolean menus_open(GtkWidget *widget)
{
  if (GTK_IS_MENU_BUTTON(widget)) {
    GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(widget));
    if (popover != NULL && gtk_widget_get_mapped(GTK_WIDGET(popover))) return TRUE;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
       child = gtk_widget_get_next_sibling(child)) if (menus_open(child)) return TRUE;
  return FALSE;
}

static gboolean refresh_after_menu(gpointer data)
{
  PumpkinWindow *self = data;
  WorldsPage *page = page_for(self);
  if (!self->disposing && menus_open(GTK_WIDGET(self->world_list))) return G_SOURCE_CONTINUE;
  page->deferred_refresh = 0;
  if (!self->disposing && page->pending_refresh) refresh_world_list(self);
  return G_SOURCE_REMOVE;
}

static void menu_closed(GtkPopover *popover, PumpkinWindow *self)
{
  (void)popover;
  WorldsPage *page = page_for(self);
  if (!self->disposing && page->pending_refresh && page->deferred_refresh == 0)
    page->deferred_refresh = g_timeout_add_full(G_PRIORITY_DEFAULT, 30, refresh_after_menu,
                                               g_object_ref(self), g_object_unref);
}

static void set_button_world(GtkWidget *button, PumpkinServer *server, const PumpkinWorldInfo *world)
{
  g_object_set_data_full(G_OBJECT(button), "world-server", g_object_ref(server), g_object_unref);
  g_object_set_data_full(G_OBJECT(button), "world-path", g_strdup(world->path), g_free);
}

static void append_world(PumpkinWindow *self, PumpkinServer *server, const PumpkinWorldInfo *world)
{
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), world->name);
  adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 1);
  adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 2);
  g_autofree char *size = world->size_known ? g_format_size_full(world->size, G_FORMAT_SIZE_IEC_UNITS) : g_strdup(_("Size unavailable"));
  g_autoptr(GDateTime) modified = g_date_time_new_from_unix_local(world->modified);
  g_autofree char *date = modified != NULL ? g_date_time_format(modified, "%x %R") : g_strdup(_("Unknown"));
  const char *world_state = !world->managed ? _("Linked or external folder · Read-only") :
    !world->recognized ? _("No level.dat found · Read-only") :
    world->configured ? _("Configured world · Used on server start") : _("Stored world · Not configured");
  g_autofree char *subtitle = g_strdup_printf(_("%s · Modified %s\n%s"), size, date, world_state);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
  if (world->configured) {
    GtkWidget *badge = gtk_label_new(_("Configured"));
    gtk_widget_add_css_class(badge, "caption");
    gtk_widget_add_css_class(badge, "success");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), badge);
  }
  GtkWidget *menu = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "view-more-symbolic");
  gtk_widget_add_css_class(menu, "flat");
  gtk_widget_set_valign(menu, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(menu, _("World actions"));
  GtkWidget *popover = gtk_popover_new();
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *open = gtk_button_new_with_label(_("Open Folder"));
  gtk_widget_add_css_class(open, "flat");
  set_button_world(open, server, world);
  g_signal_connect(open, "clicked", G_CALLBACK(open_world), self);
  g_signal_connect_swapped(open, "clicked", G_CALLBACK(gtk_popover_popdown), popover);
  gtk_box_append(GTK_BOX(actions), open);
  if (world->managed && world->recognized) {
    GtkWidget *remove = gtk_button_new_with_label(_("Remove World..."));
    gtk_widget_add_css_class(remove, "flat");
    gtk_widget_add_css_class(remove, "destructive-action");
    set_button_world(remove, server, world);
    g_object_set_data(G_OBJECT(remove), "world-configured", GINT_TO_POINTER(world->configured));
    g_signal_connect(remove, "clicked", G_CALLBACK(remove_clicked), self);
    g_signal_connect_swapped(remove, "clicked", G_CALLBACK(gtk_popover_popdown), popover);
    gtk_box_append(GTK_BOX(actions), remove);
    g_object_set_data(G_OBJECT(row), "world-remove-button", remove);
  }
  gtk_popover_set_child(GTK_POPOVER(popover), actions);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
  g_signal_connect(popover, "closed", G_CALLBACK(menu_closed), self);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), menu);
  g_object_set_data_full(G_OBJECT(row), "world-search", g_utf8_casefold(world->name, -1), g_free);
  gtk_widget_set_tooltip_text(row, world->path);
  gtk_list_box_append(self->world_list, row);
}

static void inventory_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  WorldContext *context = data;
  PumpkinWindow *self = context->window;
  WorldsPage *page = page_for(self);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) worlds = pumpkin_world_inventory_finish(result, &error);
  if (self->disposing || self->current != context->server || context->generation != page->generation) {
    context_free(context);
    return;
  }
  g_clear_object(&page->scan);
  if (worlds == NULL) {
    show_status(page, "dialog-warning-symbolic", _("Could Not Load Worlds"), error->message);
    gtk_label_set_text(page->summary, _("Reload to try again"));
  } else {
    for (guint i = 0; i < worlds->len; i++) append_world(self, context->server, g_ptr_array_index(worlds, i));
    search_changed(page->search, self);
    if (worlds->len == 0) show_status(page, "folder-symbolic", _("No Worlds Yet"),
      _("Start the server to generate its configured world, or import a folder containing level.dat."));
    else gtk_stack_set_visible_child_name(page->stack, "list");
  }
  pumpkin_window_worlds_update_state(self);
  context_free(context);
}

void refresh_world_list(PumpkinWindow *self)
{
  WorldsPage *page = page_for(self);
  if (page == NULL || self->disposing) return;
  if (menus_open(GTK_WIDGET(self->world_list))) {
    page->pending_refresh = TRUE;
    return;
  }
  page->pending_refresh = FALSE;
  ++page->generation;
  if (page->scan != NULL) g_cancellable_cancel(page->scan);
  g_clear_object(&page->scan);
  GtkListBoxRow *row;
  while ((row = gtk_list_box_get_row_at_index(self->world_list, 0)) != NULL)
    gtk_list_box_remove(self->world_list, GTK_WIDGET(row));
  pumpkin_window_worlds_update_state(self);
  if (self->current == NULL) {
    show_status(page, "folder-symbolic", _("Select a Server"), _("Its worlds will appear here."));
    return;
  }
  show_status(page, "folder-symbolic", _("Loading Worlds"), _("Reading world folders and calculating their sizes..."));
  gtk_label_set_text(page->summary, _("Loading..."));
  page->scan = g_cancellable_new();
  WorldContext *context = context_new(self, self->current, NULL);
  context->generation = page->generation;
  g_autofree char *data_dir = pumpkin_server_get_data_dir(self->current);
  pumpkin_world_inventory_async(data_dir, page->scan, inventory_finished, context);
}

static void refresh_clicked(GtkButton *button, PumpkinWindow *self)
{
  if (pumpkin_refresh_button_begin(button)) refresh_world_list(self);
}

void pumpkin_window_worlds_update_state(PumpkinWindow *self)
{
  WorldsPage *page = page_for(self);
  if (page == NULL || self->disposing) return;
  gboolean enabled = can_modify(self, self->current);
  gtk_widget_set_sensitive(GTK_WIDGET(page->import_button), enabled);
  gtk_widget_set_tooltip_text(GTK_WIDGET(page->import_button), enabled ?
    _("Copy a world into this server's storage without replacing its configured world") :
    _("Stop the server and finish maintenance before importing worlds"));
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_open_worlds), self->current != NULL);
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->world_list)); row != NULL;
       row = gtk_widget_get_next_sibling(row)) {
    GtkWidget *remove = g_object_get_data(G_OBJECT(row), "world-remove-button");
    if (remove != NULL) gtk_widget_set_sensitive(remove,
      can_modify(self, g_object_get_data(G_OBJECT(remove), "world-server")));
  }
}

void pumpkin_window_worlds_shutdown(PumpkinWindow *self)
{
  WorldsPage *page = page_for(self);
  if (page == NULL) return;
  ++page->generation;
  if (page->scan != NULL) g_cancellable_cancel(page->scan);
  g_clear_object(&page->scan);
  if (page->deferred_refresh != 0) {
    g_source_remove(page->deferred_refresh);
    page->deferred_refresh = 0;
  }
}

void pumpkin_window_worlds_setup(PumpkinWindow *self)
{
  WorldsPage *page = g_new0(WorldsPage, 1);
  g_object_set_data_full(G_OBJECT(self), "worlds-page", page, page_free);
  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  page->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(page->search), TRUE);
  g_object_set(page->search, "placeholder-text", _("Search worlds"), NULL);
  gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(page->search));
  page->import_button = GTK_BUTTON(gtk_button_new_with_label(_("Import World...")));
  gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(page->import_button));
  page->refresh = GTK_BUTTON(gtk_button_new_from_icon_name("view-refresh-symbolic"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(page->refresh), _("Reload worlds"));
  pumpkin_refresh_button_setup(page->refresh);
  gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(page->refresh));
  self->btn_open_worlds = GTK_BUTTON(gtk_button_new_from_icon_name("folder-open-symbolic"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_open_worlds), _("Open server data folder"));
  gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(self->btn_open_worlds));
  gtk_box_append(self->world_page, toolbar);

  page->summary = GTK_LABEL(gtk_label_new(_("Worlds")));
  gtk_label_set_xalign(page->summary, 0);
  gtk_widget_add_css_class(GTK_WIDGET(page->summary), "dim-label");
  gtk_box_append(self->world_page, GTK_WIDGET(page->summary));
  page->stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_vhomogeneous(page->stack, FALSE);
  gtk_widget_set_vexpand(GTK_WIDGET(page->stack), TRUE);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  self->world_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->world_list, GTK_SELECTION_NONE);
  gtk_list_box_set_filter_func(self->world_list, filter_world, page, NULL);
  gtk_widget_add_css_class(GTK_WIDGET(self->world_list), "boxed-list");
  GtkWidget *no_results = gtk_label_new(_("No matching worlds"));
  gtk_widget_set_margin_top(no_results, 24);
  gtk_widget_set_margin_bottom(no_results, 24);
  gtk_list_box_set_placeholder(self->world_list, no_results);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->world_list));
  GtkWidget *hint = gtk_label_new(_("Imported worlds are stored separately. Importing does not change the world's server configuration."));
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_xalign(GTK_LABEL(hint), 0);
  gtk_widget_add_css_class(hint, "dim-label");
  gtk_box_append(GTK_BOX(content), hint);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
  gtk_stack_add_named(page->stack, scroll, "list");
  page->status = ADW_STATUS_PAGE(adw_status_page_new());
  gtk_stack_add_named(page->stack, GTK_WIDGET(page->status), "status");
  gtk_box_append(self->world_page, GTK_WIDGET(page->stack));
  g_signal_connect(page->search, "search-changed", G_CALLBACK(search_changed), self);
  g_signal_connect(page->import_button, "clicked", G_CALLBACK(import_clicked), self);
  g_signal_connect(page->refresh, "clicked", G_CALLBACK(refresh_clicked), self);
  g_signal_connect(self->btn_open_worlds, "clicked", G_CALLBACK(open_world), self);
  GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect(drop, "drop", G_CALLBACK(worlds_drop), self);
  g_signal_connect(drop, "enter", G_CALLBACK(drop_enter), self);
  g_signal_connect(drop, "leave", G_CALLBACK(drop_leave), self);
  gtk_widget_add_controller(GTK_WIDGET(self->world_page), GTK_EVENT_CONTROLLER(drop));
  pumpkin_window_worlds_update_state(self);
}
