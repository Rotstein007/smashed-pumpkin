#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PumpkinConfig PumpkinConfig;

PumpkinConfig *pumpkin_config_load(GError **error);
void pumpkin_config_save(PumpkinConfig *config, GError **error);
void pumpkin_config_free(PumpkinConfig *config);

const char *pumpkin_config_get_base_dir(PumpkinConfig *config);
const char *pumpkin_config_get_default_download_url(PumpkinConfig *config);
gboolean pumpkin_config_get_use_cache(PumpkinConfig *config);

void pumpkin_config_set_base_dir(PumpkinConfig *config, const char *path);
void pumpkin_config_set_default_download_url(PumpkinConfig *config, const char *url);
void pumpkin_config_set_use_cache(PumpkinConfig *config, gboolean enabled);

G_END_DECLS
