#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PUMPKIN_TYPE_APP (pumpkin_app_get_type())
G_DECLARE_FINAL_TYPE(PumpkinApp, pumpkin_app, PUMPKIN, APP, AdwApplication)

PumpkinApp *pumpkin_app_new(void);
void pumpkin_app_set_tray_enabled(PumpkinApp *app, gboolean enabled);

G_END_DECLS
