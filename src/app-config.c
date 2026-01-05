#include "app-config.h"

#include <glib/gstdio.h>

struct _PumpkinConfig {
  char *path;
  char *base_dir;
  char *default_download_url;
  gboolean use_cache;
};

static char *
config_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "smashed-pumpkin", "config.ini", NULL);
}

PumpkinConfig *
pumpkin_config_load(GError **error)
{
  (void)error;
  PumpkinConfig *config = g_new0(PumpkinConfig, 1);
  config->path = config_path();
  config->base_dir = g_build_filename(g_get_home_dir(), "PumpkinServer", NULL);
  config->default_download_url = g_strdup("");
  config->use_cache = TRUE;

  g_autoptr(GKeyFile) key = g_key_file_new();
  if (g_key_file_load_from_file(key, config->path, G_KEY_FILE_NONE, NULL)) {
    g_autofree char *base = g_key_file_get_string(key, "storage", "base_dir", NULL);
    if (base != NULL && *base != '\0') {
      g_clear_pointer(&config->base_dir, g_free);
      config->base_dir = g_strdup(base);
    }

    g_autofree char *url = g_key_file_get_string(key, "updates", "default_download_url", NULL);
    if (url != NULL && *url != '\0') {
      g_clear_pointer(&config->default_download_url, g_free);
      config->default_download_url = g_strdup(url);
    }

    if (g_key_file_has_key(key, "storage", "use_cache", NULL)) {
      config->use_cache = g_key_file_get_boolean(key, "storage", "use_cache", NULL);
    }
  }

  return config;
}

void
pumpkin_config_save(PumpkinConfig *config, GError **error)
{
  g_autoptr(GKeyFile) key = g_key_file_new();
  g_key_file_set_string(key, "storage", "base_dir", config->base_dir);
  g_key_file_set_boolean(key, "storage", "use_cache", config->use_cache);
  g_key_file_set_string(key, "updates", "default_download_url", config->default_download_url);

  g_autofree char *data = g_key_file_to_data(key, NULL, NULL);
  g_autofree char *dir = g_path_get_dirname(config->path);
  g_mkdir_with_parents(dir, 0755);

  g_file_set_contents(config->path, data, -1, error);
}

void
pumpkin_config_free(PumpkinConfig *config)
{
  if (config == NULL) {
    return;
  }
  g_clear_pointer(&config->path, g_free);
  g_clear_pointer(&config->base_dir, g_free);
  g_clear_pointer(&config->default_download_url, g_free);
  g_free(config);
}

const char *
pumpkin_config_get_base_dir(PumpkinConfig *config)
{
  return config->base_dir;
}

const char *
pumpkin_config_get_default_download_url(PumpkinConfig *config)
{
  return config->default_download_url;
}

gboolean
pumpkin_config_get_use_cache(PumpkinConfig *config)
{
  return config->use_cache;
}

void
pumpkin_config_set_base_dir(PumpkinConfig *config, const char *path)
{
  g_free(config->base_dir);
  config->base_dir = g_strdup(path);
}

void
pumpkin_config_set_default_download_url(PumpkinConfig *config, const char *url)
{
  g_free(config->default_download_url);
  config->default_download_url = g_strdup(url);
}

void
pumpkin_config_set_use_cache(PumpkinConfig *config, gboolean enabled)
{
  config->use_cache = enabled;
}
