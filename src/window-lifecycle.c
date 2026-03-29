#include "window-lifecycle.h"

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
      append_log_for_server(self, server, "[SMPK] Cannot start: Pumpkin is not installed.");
    } else if (self->current == server) {
      set_details_error(self, "Install Pumpkin before starting.");
    }
    return FALSE;
  }

  DownloadProgressState *state = get_download_progress_state(self, server, FALSE);
  if (state != NULL && state->active) {
    state->active = FALSE;
  }

  if (use_starting_ui_state && self->current == server) {
    self->ui_state = UI_STATE_STARTING;
    queue_overview_refresh(self, TRUE);
  }

  if (self->current == server) {
    int port = pumpkin_server_get_port(server);
    if (port <= 0) {
      set_details_error(self, "Invalid server port.");
      self->ui_state = UI_STATE_ERROR;
      return FALSE;
    }

    g_autoptr(GSocketListener) listener = g_socket_listener_new();
    g_autoptr(GError) port_error = NULL;
    gboolean port_ok = g_socket_listener_add_inet_port(listener, port, NULL, &port_error);
    if (!port_ok) {
      if (port_error != NULL) {
        g_autofree char *msg = g_strdup_printf("Port %d may already be in use (%s). Trying to start anyway.",
                                               port, port_error->message);
        append_log(self, msg);
      } else {
        g_autofree char *msg = g_strdup_printf("Port %d may already be in use. Trying to start anyway.", port);
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
      append_log(self, error != NULL ? error->message : "Failed to start server.");
      set_details_error(self, error != NULL ? error->message : "Failed to start server.");
      self->ui_state = UI_STATE_ERROR;
    }
    return FALSE;
  }

  if (from_network_action) {
    append_log_for_server(self, server, "[SMPK] Started by network action.");
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
  set_server_running_hint(self, server, FALSE);
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
  if (self->current == NULL) {
    return;
  }
  if (self->ui_state == UI_STATE_STARTING || self->ui_state == UI_STATE_STOPPING ||
      self->ui_state == UI_STATE_RESTARTING || self->ui_state == UI_STATE_RUNNING) {
    return;
  }
  start_server_internal(self, self->current, FALSE, TRUE);
}

void
on_details_stop(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->current == NULL) {
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
