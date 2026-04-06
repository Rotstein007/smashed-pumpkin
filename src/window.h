#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PUMPKIN_TYPE_WINDOW (pumpkin_window_get_type())
#if defined(G_OS_WIN32)
G_DECLARE_FINAL_TYPE(PumpkinWindow, pumpkin_window, PUMPKIN, WINDOW, GtkApplicationWindow)
#else
G_DECLARE_FINAL_TYPE(PumpkinWindow, pumpkin_window, PUMPKIN, WINDOW, AdwApplicationWindow)
#endif

GtkWindow *pumpkin_window_new(AdwApplication *app);
void pumpkin_window_select_server(PumpkinWindow *self, const char *id_or_name);
void pumpkin_window_stop_all_servers(PumpkinWindow *self);
void pumpkin_window_schedule_auto_start_servers(PumpkinWindow *self);
gboolean pumpkin_window_get_review_prompt_shown(PumpkinWindow *self);
void pumpkin_window_set_review_prompt_shown(PumpkinWindow *self, gboolean shown);

G_END_DECLS
