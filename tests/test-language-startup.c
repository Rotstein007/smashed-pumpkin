#include "i18n.h"
#include <glib/gstdio.h>
#include <locale.h>
#include <string.h>

#ifdef LC_MESSAGES
#define MESSAGE_CATEGORY LC_MESSAGES
#else
#define MESSAGE_CATEGORY LC_ALL
#endif

int
main(int argc, char **argv)
{
  g_assert_cmpint(argc, ==, 5);
  g_autofree char *profile = g_dir_make_tmp("pumpkin-language-startup-XXXXXX", NULL);
  g_assert_nonnull(profile);
  g_setenv("HOME", profile, TRUE);
  g_setenv("XDG_CONFIG_HOME", profile, TRUE);
  g_autofree char *dir = g_build_filename(profile, "smashed-pumpkin", NULL);
  g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_autofree char *path = g_build_filename(dir, "config.ini", NULL);
  if (!g_str_equal(argv[1], "absent")) {
    g_autofree char *ini = g_strdup_printf("[behavior]\nlanguage=%s\n", argv[1]);
    g_assert_true(g_file_set_contents(path, ini, -1, NULL));
  }
  /* Match production startup ordering; all changes precede threads/GTK. */
  pumpkin_i18n_startup();
  g_assert_cmpstr(pumpkin_i18n_get_language(), ==, argv[2]);
  if (!g_str_equal(argv[3], "-")) {
    g_assert_cmpstr(_("Overview"), ==, argv[3]);
    g_assert_cmpstr(dgettext(GETTEXT_PACKAGE, "Overview"), ==, argv[3]);
  }
  g_assert_cmpstr(setlocale(LC_NUMERIC, NULL), ==, "C");
  g_assert_cmpstr(localeconv()->decimal_point, ==, ".");
  if (g_str_equal(argv[4], "private")) {
    g_assert_cmpstr(setlocale(MESSAGE_CATEGORY, NULL), ==, "smashed.UTF-8");
    g_assert_nonnull(g_getenv("LOCPATH"));
    g_assert_true(g_str_has_prefix(g_getenv("LOCPATH"), pumpkin_i18n_message_locale_root()));
  }
  g_autoptr(GKeyFile) key = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL));
  g_autofree char *saved = g_key_file_get_string(key, "behavior", "language", NULL);
  g_assert_cmpstr(saved, ==, g_str_equal(argv[1], "absent") ? argv[2] : argv[1]);
  /* The config directory stays private (0700) however it was created, and a
   * first-run config.ini written by the application is owner-only (0600). */
  GStatBuf dir_stat, file_stat;
  g_assert_cmpint(g_stat(dir, &dir_stat), ==, 0);
  g_assert_cmpint(dir_stat.st_mode & 0777, ==, 0700);
  if (g_str_equal(argv[1], "absent")) {
    g_assert_cmpint(g_stat(path, &file_stat), ==, 0);
    g_assert_cmpint(file_stat.st_mode & 0777, ==, 0600);
  }
  g_assert_cmpint(g_remove(path), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  g_assert_cmpint(g_rmdir(profile), ==, 0);
  g_print("PASS: startup selection %s; saved preference %s; messages %s\n",
           pumpkin_i18n_get_language(), saved, setlocale(MESSAGE_CATEGORY, NULL));
  return 0;
}
