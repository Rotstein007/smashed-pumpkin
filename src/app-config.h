#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PumpkinConfig PumpkinConfig;

typedef enum {
  PUMPKIN_DATE_FORMAT_YMD = 0,
  PUMPKIN_DATE_FORMAT_DMY,
  PUMPKIN_DATE_FORMAT_MDY
} PumpkinDateFormat;

typedef enum {
  PUMPKIN_TIME_FORMAT_24H = 0,
  PUMPKIN_TIME_FORMAT_12H
} PumpkinTimeFormat;

PumpkinConfig *pumpkin_config_load(GError **error);
void pumpkin_config_save(PumpkinConfig *config, GError **error);
void pumpkin_config_free(PumpkinConfig *config);

const char *pumpkin_config_get_base_dir(PumpkinConfig *config);
const char *pumpkin_config_get_default_download_url(PumpkinConfig *config);
gboolean pumpkin_config_get_use_cache(PumpkinConfig *config);
gboolean pumpkin_config_get_run_in_background(PumpkinConfig *config);
gboolean pumpkin_config_get_detailed_overview_cards(PumpkinConfig *config);
gboolean pumpkin_config_get_autostart_on_boot(PumpkinConfig *config);
gboolean pumpkin_config_get_start_minimized(PumpkinConfig *config);
gboolean pumpkin_config_get_auto_start_servers_enabled(PumpkinConfig *config);
gboolean pumpkin_config_get_review_prompt_shown(PumpkinConfig *config);
PumpkinDateFormat pumpkin_config_get_date_format(PumpkinConfig *config);
PumpkinTimeFormat pumpkin_config_get_time_format(PumpkinConfig *config);

void pumpkin_config_set_base_dir(PumpkinConfig *config, const char *path);
void pumpkin_config_set_default_download_url(PumpkinConfig *config, const char *url);
void pumpkin_config_set_use_cache(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_run_in_background(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_detailed_overview_cards(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_autostart_on_boot(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_start_minimized(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_auto_start_servers_enabled(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_review_prompt_shown(PumpkinConfig *config, gboolean shown);
void pumpkin_config_set_date_format(PumpkinConfig *config, PumpkinDateFormat format);
void pumpkin_config_set_time_format(PumpkinConfig *config, PumpkinTimeFormat format);

void pumpkin_config_manage_autostart_desktop(gboolean enabled);

G_END_DECLS
