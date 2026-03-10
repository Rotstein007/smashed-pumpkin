#include "app.h"
#include "smashed-pumpkin-resources.h"

#if defined(G_OS_WIN32)
#include <windows.h>

#include <glib/gstdio.h>

static gchar *
find_first_child_directory(const gchar *root_path)
{
  if (!g_file_test(root_path, G_FILE_TEST_IS_DIR)) {
    return NULL;
  }

  GDir *dir = g_dir_open(root_path, 0, NULL);
  if (dir == NULL) {
    return NULL;
  }

  const gchar *name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL) {
    g_autofree gchar *candidate = g_build_filename(root_path, name, NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
      g_dir_close(dir);
      return g_steal_pointer(&candidate);
    }
  }

  g_dir_close(dir);
  return NULL;
}

static void
prepend_search_path_env(const gchar *name, const gchar *value)
{
  const gchar *existing = g_getenv(name);
  if (existing != NULL && *existing != '\0') {
    g_autofree gchar *combined =
      g_strconcat(value, G_SEARCHPATH_SEPARATOR_S, existing, NULL);
    g_setenv(name, combined, TRUE);
    return;
  }

  g_setenv(name, value, TRUE);
}

static void
prepare_gdk_pixbuf_cache(const gchar *prefix, const gchar *bin_dir)
{
  g_autofree gchar *query_tool =
    g_build_filename(bin_dir, "gdk-pixbuf-query-loaders.exe", NULL);
  if (!g_file_test(query_tool, G_FILE_TEST_EXISTS)) {
    return;
  }

  g_autofree gchar *pixbuf_root =
    g_build_filename(prefix, "lib", "gdk-pixbuf-2.0", NULL);
  g_autofree gchar *pixbuf_version_dir = find_first_child_directory(pixbuf_root);
  if (pixbuf_version_dir == NULL) {
    return;
  }

  g_autofree gchar *loaders_dir = g_build_filename(pixbuf_version_dir, "loaders", NULL);
  if (!g_file_test(loaders_dir, G_FILE_TEST_IS_DIR)) {
    return;
  }

  g_setenv("GDK_PIXBUF_MODULEDIR", loaders_dir, TRUE);

  GDir *dir = g_dir_open(loaders_dir, 0, NULL);
  if (dir == NULL) {
    return;
  }

  g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(argv, g_strdup(query_tool));

  const gchar *name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (!g_str_has_suffix(name, ".dll")) {
      continue;
    }

    g_autofree gchar *loader_path = g_build_filename(loaders_dir, name, NULL);
    if (g_file_test(loader_path, G_FILE_TEST_EXISTS)) {
      g_ptr_array_add(argv, g_steal_pointer(&loader_path));
    }
  }
  g_dir_close(dir);

  if (argv->len <= 1) {
    return;
  }

  g_ptr_array_add(argv, NULL);

  g_autofree gchar *cache_dir =
    g_build_filename(g_get_user_cache_dir(), "smashed-pumpkin", NULL);
  if (g_mkdir_with_parents(cache_dir, 0700) != 0) {
    return;
  }

  g_autofree gchar *cache_file =
    g_build_filename(cache_dir, "gdk-pixbuf-loaders.cache", NULL);
  g_autofree gchar *stdout_data = NULL;
  g_autofree gchar *stderr_data = NULL;
  gint status = 0;
  gboolean ok =
    g_spawn_sync(NULL,
                 (gchar **)argv->pdata,
                 NULL,
                 G_SPAWN_DEFAULT,
                 NULL,
                 NULL,
                 &stdout_data,
                 &stderr_data,
                 &status,
                 NULL);

  if (!ok || status != 0 || stdout_data == NULL || *stdout_data == '\0') {
    return;
  }

  if (g_file_set_contents(cache_file, stdout_data, -1, NULL)) {
    g_setenv("GDK_PIXBUF_MODULE_FILE", cache_file, TRUE);
  }
}

static void
init_windows_runtime(void)
{
  wchar_t exe_path_w[MAX_PATH] = {0};
  DWORD written = GetModuleFileNameW(NULL, exe_path_w, G_N_ELEMENTS(exe_path_w));
  if (written == 0 || written >= G_N_ELEMENTS(exe_path_w)) {
    return;
  }

  g_autofree gchar *exe_path =
    g_utf16_to_utf8((const gunichar2 *)exe_path_w, -1, NULL, NULL, NULL);
  if (exe_path == NULL) {
    return;
  }

  g_autofree gchar *bin_dir = g_path_get_dirname(exe_path);
  g_autofree gchar *parent_dir = g_path_get_dirname(bin_dir);
  g_autofree gchar *parent_share = g_build_filename(parent_dir, "share", NULL);
  g_autofree gchar *prefix =
    g_file_test(parent_share, G_FILE_TEST_IS_DIR) ? g_strdup(parent_dir) : g_strdup(bin_dir);

  g_autofree gchar *share_dir = g_build_filename(prefix, "share", NULL);
  if (g_file_test(share_dir, G_FILE_TEST_IS_DIR)) {
    prepend_search_path_env("XDG_DATA_DIRS", share_dir);
  }

  g_autofree gchar *etc_dir = g_build_filename(prefix, "etc", NULL);
  if (g_file_test(etc_dir, G_FILE_TEST_IS_DIR)) {
    g_setenv("XDG_CONFIG_DIRS", etc_dir, TRUE);
  }

  g_setenv("GTK_DATA_PREFIX", prefix, TRUE);
  g_setenv("GTK_EXE_PREFIX", prefix, TRUE);

  g_autofree gchar *schema_dir =
    g_build_filename(share_dir, "glib-2.0", "schemas", NULL);
  if (g_file_test(schema_dir, G_FILE_TEST_IS_DIR)) {
    g_setenv("GSETTINGS_SCHEMA_DIR", schema_dir, TRUE);
  }

  g_autofree gchar *gio_module_dir =
    g_build_filename(prefix, "lib", "gio", "modules", NULL);
  if (g_file_test(gio_module_dir, G_FILE_TEST_IS_DIR)) {
    g_setenv("GIO_MODULE_DIR", gio_module_dir, TRUE);
    g_setenv("GIO_EXTRA_MODULES", gio_module_dir, TRUE);
  }

  g_autofree gchar *fontconfig_dir = g_build_filename(etc_dir, "fonts", NULL);
  g_autofree gchar *fontconfig_file =
    g_build_filename(fontconfig_dir, "fonts.conf", NULL);
  if (g_file_test(fontconfig_dir, G_FILE_TEST_IS_DIR)) {
    g_setenv("FONTCONFIG_PATH", fontconfig_dir, TRUE);
  }
  if (g_file_test(fontconfig_file, G_FILE_TEST_EXISTS)) {
    g_setenv("FONTCONFIG_FILE", fontconfig_file, TRUE);
  }

  prepare_gdk_pixbuf_cache(prefix, bin_dir);
}
#endif

int
main(int argc, char *argv[])
{
#if defined(G_OS_WIN32)
  init_windows_runtime();
#endif
  smashed_pumpkin_register_resource();
  return g_application_run(G_APPLICATION(pumpkin_app_new()), argc, argv);
}
