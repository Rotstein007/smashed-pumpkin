#include "window-lifecycle.h"
#include "i18n.h"
#include "platform.h"

static gboolean
server_running_hint(PumpkinWindow *self, PumpkinServer *server)
{
  if (self == NULL || server == NULL || self->server_running_hints == NULL) {
    return FALSE;
  }
  return GPOINTER_TO_INT(g_hash_table_lookup(self->server_running_hints, server)) != 0;
}

void
set_server_running_hint(PumpkinWindow *self, PumpkinServer *server, gboolean running)
{
  if (self == NULL || server == NULL || self->server_running_hints == NULL) {
    return;
  }
  g_hash_table_replace(self->server_running_hints,
                       g_object_ref(server),
                       GINT_TO_POINTER(running ? 1 : 0));
}

gboolean
server_is_running_ui(PumpkinWindow *self, PumpkinServer *server)
{
  if (server == NULL) {
    return FALSE;
  }
  return pumpkin_server_get_running(server) || server_running_hint(self, server);
}

gboolean
start_server_internal(PumpkinWindow *self,
                      PumpkinServer *server,
                      gboolean from_network_action,
                      gboolean use_starting_ui_state)
{
  if (self == NULL || server == NULL) {
    return FALSE;
  }

  ensure_server_log_handler(self, server);
  if (server_is_running_ui(self, server)) {
    return FALSE;
  }
  if (!server_has_installation(self, server)) {
    if (from_network_action) {
      g_autofree char *message = g_strdup_printf("[SMPK] %s", _("Cannot start: Pumpkin is not installed."));
      append_log_for_server(self, server, message);
    } else if (self->current == server) {
      set_details_error(self, _("Install Pumpkin before starting."));
    }
    return FALSE;
  }

  DownloadProgressState *state = get_download_progress_state(self, server, FALSE);
  if (state != NULL && state->active) {
    return FALSE;
  }

  if (use_starting_ui_state && self->current == server) {
    self->ui_state = UI_STATE_STARTING;
    queue_overview_refresh(self, TRUE);
  }

  if (self->current == server) {
    int port = pumpkin_server_get_port(server);
    if (port <= 0) {
      set_details_error(self, _("Invalid server port."));
      self->ui_state = UI_STATE_ERROR;
      return FALSE;
    }

    g_autoptr(GSocketListener) listener = g_socket_listener_new();
    g_autoptr(GError) port_error = NULL;
    gboolean port_ok = g_socket_listener_add_inet_port(listener, port, NULL, &port_error);
    if (!port_ok) {
      if (port_error != NULL) {
        g_autofree char *msg = g_strdup_printf(_("Port %d may already be in use (%s). Trying to start anyway."),
                                               port, port_error->message);
        append_log(self, msg);
      } else {
        g_autofree char *msg = g_strdup_printf(_("Port %d may already be in use. Trying to start anyway."), port);
        append_log(self, msg);
      }
    }
  }

  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_start(server, &error)) {
    if (from_network_action) {
      if (error != NULL) {
        append_log_for_server(self, server, error->message);
      }
    } else if (self->current == server) {
      append_log(self, error != NULL ? error->message : _("Failed to start server."));
      set_details_error(self, error != NULL ? error->message : _("Failed to start server."));
      self->ui_state = UI_STATE_ERROR;
    }
    return FALSE;
  }

  if (from_network_action) {
    g_autofree char *message = g_strdup_printf("[SMPK] %s", _("Started by network action."));
    append_log_for_server(self, server, message);
  }
  set_server_running_hint(self, server, TRUE);
  if (self->current == server) {
    self->user_stop_requested = FALSE;
    set_console_warning(self, NULL, FALSE);
    if (from_network_action) {
      self->ui_state = UI_STATE_RUNNING;
    } else {
      if (self->start_delay_id != 0) {
        g_source_remove(self->start_delay_id);
        self->start_delay_id = 0;
      }
      self->start_delay_id = g_timeout_add_seconds(2, start_after_delay, g_object_ref(self));
    }
  }

  queue_overview_refresh(self, self->current == server);
  return TRUE;
}

gboolean
stop_server_internal(PumpkinWindow *self,
                     PumpkinServer *server,
                     gboolean from_network_action)
{
  if (self == NULL || server == NULL || !server_is_running_ui(self, server)) {
    return FALSE;
  }

  if (server == self->current) {
    self->user_stop_requested = TRUE;
    if (!from_network_action) {
      self->ui_state = UI_STATE_STOPPING;
      if (self->auto_update_server == self->current) {
        clear_auto_update_countdown(self);
      }
    }
  }

  if (pumpkin_server_get_running(server)) {
    pumpkin_server_stop(server);
  }
  /* Keep the process visible as running until the exit callback confirms it. */
  queue_overview_refresh(self, self->current == server);
  return TRUE;
}

gboolean
restart_after_delay(gpointer data)
{
  RestartContext *ctx = data;
  PumpkinWindow *self = ctx->self;
  PumpkinServer *server = ctx->server;
  self->restart_delay_id = 0;
  self->restart_requested = FALSE;
  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_start(server, &error)) {
    append_log(self, error->message);
    if (self->current == server) {
      set_details_error(self, error->message);
      self->ui_state = UI_STATE_ERROR;
    }
  } else {
    set_server_running_hint(self, server, TRUE);
    if (self->current == server) {
      self->ui_state = UI_STATE_RUNNING;
      self->user_stop_requested = FALSE;
      set_console_warning(self, NULL, FALSE);
    }
  }

  queue_overview_refresh(self, self->current == server);

  g_object_unref(ctx->server);
  g_object_unref(ctx->self);
  g_free(ctx);
  return G_SOURCE_REMOVE;
}

void
on_details_start(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (!pumpkin_platform_supports_local_hosting()) {
    return;
  }
  if (self->current == NULL) {
    return;
  }
  if (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_STOPPING ||
      self->ui_state == UI_STATE_RESTARTING || self->ui_state == UI_STATE_RUNNING) {
    return;
  }
  start_server_internal(self, self->current, FALSE, TRUE);
}

static void
on_force_stop_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(PumpkinWindow) self = PUMPKIN_WINDOW(data);
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  PumpkinServer *server = g_object_get_data(source, "server");
  if (g_strcmp0(response, "force") == 0 && pumpkin_server_get_stopping(server)) {
    pumpkin_server_force_stop(server);
    queue_overview_refresh(self, self->current == server);
  }
}

void
on_details_stop(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (!pumpkin_platform_supports_local_hosting()) {
    return;
  }
  if (self->current == NULL) {
    return;
  }
  if (pumpkin_server_get_stop_timed_out(self->current)) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Force stop server?"),
      _("The server has not finished shutting down. Force stopping may lose unsaved world or plugin data.")));
    adw_alert_dialog_add_responses(dialog, "cancel", _("Keep Waiting"), "force", _("Force Stop"), NULL);
    adw_alert_dialog_set_response_appearance(dialog, "force", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dialog, "cancel");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    g_object_set_data_full(G_OBJECT(dialog), "server", g_object_ref(self->current), g_object_unref);
    adw_alert_dialog_choose(dialog, GTK_WIDGET(self), NULL, on_force_stop_chosen, g_object_ref(self));
    return;
  }
  if (self->ui_state == UI_STATE_STOPPING || self->ui_state == UI_STATE_RESTARTING ||
      self->ui_state == UI_STATE_STARTING) {
    return;
  }
  stop_server_internal(self, self->current, FALSE);
}

void
on_details_restart(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (!pumpkin_platform_supports_local_hosting()) {
    return;
  }
  if (self->current == NULL) {
    return;
  }

  if (self->restart_delay_id != 0) {
    g_source_remove(self->restart_delay_id);
    self->restart_delay_id = 0;
  }

  self->restart_requested = TRUE;
  self->restart_pending = TRUE;
  self->ui_state = UI_STATE_RESTARTING;
  self->user_stop_requested = TRUE;
  if (self->auto_update_server == self->current) {
    clear_auto_update_countdown(self);
  }
  if (pumpkin_server_get_running(self->current)) {
    stop_server_internal(self, self->current, TRUE);
  } else {
    self->restart_pending = FALSE;
    RestartContext *ctx = g_new0(RestartContext, 1);
    ctx->self = g_object_ref(self);
    ctx->server = g_object_ref(self->current);
    self->restart_delay_id = g_timeout_add(0, restart_after_delay, ctx);
  }
  queue_overview_refresh(self, TRUE);
}
