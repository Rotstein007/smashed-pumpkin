#pragma once

#include <gio/gio.h>
#include "release-channel.h"

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

typedef enum {
  PUMPKIN_START_MAXIMIZED = 0,
  PUMPKIN_START_FULLSCREEN
} PumpkinStartMode;

PumpkinConfig *pumpkin_config_load(GError **error);
/* Stored preference, not the language currently active in this process. */
const char *pumpkin_config_get_language(PumpkinConfig *config);
void pumpkin_config_set_language(PumpkinConfig *config, const char *language);
/* Save all current fields, rolling back only the language preference on failure. */
gboolean pumpkin_config_save_language(PumpkinConfig *config, const char *language, GError **error);
void pumpkin_config_save(PumpkinConfig *config, GError **error);
void pumpkin_config_free(PumpkinConfig *config);

const char *pumpkin_config_get_base_dir(PumpkinConfig *config);
const char *pumpkin_config_get_default_download_url(PumpkinConfig *config);
PumpkinReleaseChannel pumpkin_config_get_release_channel(PumpkinConfig *config);
gboolean pumpkin_config_get_use_cache(PumpkinConfig *config);
gboolean pumpkin_config_get_run_in_background(PumpkinConfig *config);
gboolean pumpkin_config_get_detailed_overview_cards(PumpkinConfig *config);
gboolean pumpkin_config_get_minimap_enabled(PumpkinConfig *config);
gboolean pumpkin_config_get_minimap_consent(PumpkinConfig *config);
guint pumpkin_config_get_minimap_refresh_hz(PumpkinConfig *config);
gboolean pumpkin_config_get_autostart_on_boot(PumpkinConfig *config);
gboolean pumpkin_config_get_start_minimized(PumpkinConfig *config);
gboolean pumpkin_config_get_start_expanded(PumpkinConfig *config);
gboolean pumpkin_config_should_expand_on_startup(PumpkinConfig *config);
PumpkinStartMode pumpkin_config_get_start_mode(PumpkinConfig *config);
const char *pumpkin_config_get_start_monitor(PumpkinConfig *config);
gboolean pumpkin_config_get_auto_start_servers_enabled(PumpkinConfig *config);
gboolean pumpkin_config_get_review_prompt_shown(PumpkinConfig *config);
PumpkinDateFormat pumpkin_config_get_date_format(PumpkinConfig *config);
PumpkinTimeFormat pumpkin_config_get_time_format(PumpkinConfig *config);

void pumpkin_config_set_base_dir(PumpkinConfig *config, const char *path);
void pumpkin_config_set_default_download_url(PumpkinConfig *config, const char *url);
void pumpkin_config_set_release_channel(PumpkinConfig *config, PumpkinReleaseChannel channel);
void pumpkin_config_set_use_cache(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_run_in_background(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_detailed_overview_cards(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_minimap_enabled(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_minimap_consent(PumpkinConfig *config, gboolean consent);
void pumpkin_config_set_minimap_refresh_hz(PumpkinConfig *config, guint hz);
void pumpkin_config_set_autostart_on_boot(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_start_minimized(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_start_expanded(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_start_mode(PumpkinConfig *config, PumpkinStartMode mode);
void pumpkin_config_set_start_monitor(PumpkinConfig *config, const char *monitor);
void pumpkin_config_set_auto_start_servers_enabled(PumpkinConfig *config, gboolean enabled);
void pumpkin_config_set_review_prompt_shown(PumpkinConfig *config, gboolean shown);
void pumpkin_config_set_date_format(PumpkinConfig *config, PumpkinDateFormat format);
void pumpkin_config_set_time_format(PumpkinConfig *config, PumpkinTimeFormat format);

void pumpkin_config_manage_autostart_desktop(gboolean enabled);

G_END_DECLS
