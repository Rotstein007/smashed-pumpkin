#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PUMPKIN_TYPE_WINDOW (pumpkin_window_get_type())
G_DECLARE_FINAL_TYPE(PumpkinWindow, pumpkin_window, PUMPKIN, WINDOW, AdwApplicationWindow)

GtkWindow *pumpkin_window_new(AdwApplication *app);
void pumpkin_window_select_server(PumpkinWindow *self, const char *id_or_name);

G_END_DECLS
