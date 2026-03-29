#pragma once

#include "window-internal.h"

gboolean start_server_internal(PumpkinWindow *self,
                               PumpkinServer *server,
                               gboolean from_network_action,
                               gboolean use_starting_ui_state);
gboolean stop_server_internal(PumpkinWindow *self,
                              PumpkinServer *server,
                              gboolean from_network_action);
gboolean server_is_running_ui(PumpkinWindow *self, PumpkinServer *server);
void set_server_running_hint(PumpkinWindow *self, PumpkinServer *server, gboolean running);
gboolean restart_after_delay(gpointer data);
void on_details_start(GtkButton *button, PumpkinWindow *self);
void on_details_stop(GtkButton *button, PumpkinWindow *self);
void on_details_restart(GtkButton *button, PumpkinWindow *self);
