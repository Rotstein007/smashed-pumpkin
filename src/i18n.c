#include "i18n.h"

#include <locale.h>
#include "language.h"

#ifdef LC_MESSAGES
#define MESSAGE_CATEGORY LC_MESSAGES
#else
/* The Windows CRT does not expose LC_MESSAGES. */
#define MESSAGE_CATEGORY LC_ALL
#endif

static const char *session_language;

const char *
pumpkin_i18n_get_language(void)
{
  return session_language;
}

/* C, POSIX and C.* locales suppress gettext LANGUAGE lookups; any other
 * available locale enables them. */
static gboolean
is_c_family_locale(const char *name)
{
  return name == NULL || g_str_equal(name, "C") || g_str_equal(name, "POSIX") ||
    g_str_has_prefix(name, "C.");
}

void
pumpkin_i18n_init_language(const char *language)
{
  /* This is startup-only: do not mutate the environment once workers exist. */
  session_language = pumpkin_language_from_index(pumpkin_language_index(language));
  if (g_str_equal(session_language, "system")) session_language = "en";
  g_setenv("LANGUAGE", session_language, TRUE);
  pumpkin_i18n_init();

  if (!g_str_equal(session_language, "en") &&
      is_c_family_locale(setlocale(MESSAGE_CATEGORY, NULL))) {
#ifdef HAVE_PRIVATE_MESSAGE_LOCALE
    const char *root = pumpkin_i18n_message_locale_root();
    g_autofree char *old_path = g_strdup(g_getenv("LOCPATH"));
    g_autofree char *path = old_path != NULL && *old_path != '\0'
      ? g_strconcat(root, G_SEARCHPATH_SEPARATOR_S, old_path, NULL) : g_strdup(root);
    g_setenv("LOCPATH", path, TRUE);
    if (setlocale(MESSAGE_CATEGORY, "smashed.UTF-8") == NULL) {
      if (old_path != NULL) g_setenv("LOCPATH", old_path, TRUE);
      else g_unsetenv("LOCPATH");
      if (setlocale(MESSAGE_CATEGORY, "en_US.UTF-8") == NULL)
        g_warning("No usable messages locale; translations for '%s' are unavailable",
                  session_language);
    }
#elif !defined(G_OS_WIN32)
    /* On libintl platforms, an available non-C locale enables LANGUAGE. The
     * Windows CRT rejects POSIX-style locale names and libintl follows
     * LANGUAGE directly there, so skip the attempt on Windows. */
    if (setlocale(MESSAGE_CATEGORY, "en_US.UTF-8") == NULL)
      g_warning("No usable messages locale; translations for '%s' are unavailable",
                session_language);
#endif
  }
  setlocale(LC_NUMERIC, "C");
}

void
pumpkin_i18n_startup(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *language = pumpkin_language_prepare(&error);
  if (error != NULL) g_printerr("Language configuration: %s\n", error->message);
  pumpkin_i18n_init_language(language);
}

/* Resolve the private messages-only locale root: an absolute environment
 * override wins, then the installed category when present, then the build-tree
 * mirror used by uninstalled development runs. */
const char *
pumpkin_i18n_message_locale_root(void)
{
#ifdef HAVE_PRIVATE_MESSAGE_LOCALE
  const char *override = g_getenv("SMASHED_PUMPKIN_MESSAGE_LOCALEDIR");
  if (override != NULL && g_path_is_absolute(override)) return override;
  if (g_file_test(MESSAGE_LOCALE_ROOT "/smashed.UTF-8/LC_MESSAGES/SYS_LC_MESSAGES", G_FILE_TEST_IS_REGULAR))
    return MESSAGE_LOCALE_ROOT;
#ifdef BUILD_MESSAGE_LOCALE_ROOT
  if (g_file_test(BUILD_MESSAGE_LOCALE_ROOT "/smashed.UTF-8/LC_MESSAGES/SYS_LC_MESSAGES", G_FILE_TEST_IS_REGULAR))
    return BUILD_MESSAGE_LOCALE_ROOT;
#endif
  return MESSAGE_LOCALE_ROOT;
#else
  return NULL;
#endif
}

void
pumpkin_i18n_init(void)
{
  /* Respect the process environment for messages, character handling and time.
   * An invalid system locale leaves the existing C locale as the safe fallback.
   * Wire formats, JSON, commands and machine-readable numbers must use dots. */
  setlocale(LC_ALL, "");
  setlocale(LC_NUMERIC, "C");

  bindtextdomain(GETTEXT_PACKAGE, pumpkin_language_catalog_root());
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);
}
