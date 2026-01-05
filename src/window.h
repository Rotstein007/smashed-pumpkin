#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PUMPKIN_TYPE_WINDOW (pumpkin_window_get_type())
G_DECLARE_FINAL_TYPE(PumpkinWindow, pumpkin_window, PUMPKIN, WINDOW, AdwApplicationWindow)

GtkWindow *pumpkin_window_new(AdwApplication *app);

G_END_DECLS
