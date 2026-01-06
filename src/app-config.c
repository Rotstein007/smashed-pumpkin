#include "app-config.h"

#include <glib/gstdio.h>

struct _PumpkinConfig {
  char *path;
  char *base_dir;
  char *default_download_url;
  gboolean use_cache;
  gboolean run_in_background;
};

static char *
default_base_dir(void)
{
  return g_build_filename(g_get_user_data_dir(), "smashed-pumpkin", "servers", NULL);
}

static char *
legacy_base_dir(void)
{
  return g_build_filename(g_get_home_dir(), "PumpkinServer", NULL);
}

static gboolean
dir_is_empty(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir == NULL) {
    return TRUE;
  }
  const char *entry = g_dir_read_name(dir);
  g_dir_close(dir);
  return entry == NULL;
}

static gboolean
migrate_legacy_servers(const char *legacy_dir, const char *new_dir, GError **error)
{
  if (legacy_dir == NULL || new_dir == NULL) {
    return FALSE;
  }
  if (!g_file_test(legacy_dir, G_FILE_TEST_IS_DIR)) {
    return FALSE;
  }
  if (g_file_test(new_dir, G_FILE_TEST_EXISTS)) {
    if (!dir_is_empty(new_dir)) {
      return FALSE;
    }
    g_rmdir(new_dir);
  }

  g_autoptr(GFile) src = g_file_new_for_path(legacy_dir);
  g_autoptr(GFile) dst = g_file_new_for_path(new_dir);
  return g_file_move(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error);
}

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
  config->base_dir = default_base_dir();
  config->default_download_url = g_strdup("");
  config->use_cache = TRUE;
  config->run_in_background = TRUE;

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

    if (g_key_file_has_key(key, "behavior", "run_in_background", NULL)) {
      config->run_in_background = g_key_file_get_boolean(key, "behavior", "run_in_background", NULL);
    }
  }

  gboolean migrated = FALSE;
  gboolean should_save = FALSE;
  g_autofree char *legacy = legacy_base_dir();
  g_autofree char *default_dir = default_base_dir();
  if (g_strcmp0(config->base_dir, legacy) == 0 &&
      !g_file_test(legacy, G_FILE_TEST_IS_DIR)) {
    g_clear_pointer(&config->base_dir, g_free);
    config->base_dir = g_strdup(default_dir);
    should_save = TRUE;
  }
  if (g_strcmp0(config->base_dir, legacy) == 0 ||
      (g_strcmp0(config->base_dir, default_dir) == 0 && dir_is_empty(default_dir))) {
    g_autoptr(GError) migrate_error = NULL;
    if (migrate_legacy_servers(legacy, default_dir, &migrate_error)) {
      g_clear_pointer(&config->base_dir, g_free);
      config->base_dir = g_strdup(default_dir);
      migrated = TRUE;
      should_save = TRUE;
    }
  }

  if (g_mkdir_with_parents(config->base_dir, 0755) != 0) {
    g_clear_pointer(&config->base_dir, g_free);
    config->base_dir = default_base_dir();
    g_mkdir_with_parents(config->base_dir, 0755);
  }

  if (migrated || should_save) {
    pumpkin_config_save(config, NULL);
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
  g_key_file_set_boolean(key, "behavior", "run_in_background", config->run_in_background);

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

gboolean
pumpkin_config_get_run_in_background(PumpkinConfig *config)
{
  return config->run_in_background;
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

void
pumpkin_config_set_run_in_background(PumpkinConfig *config, gboolean enabled)
{
  config->run_in_background = enabled;
}
