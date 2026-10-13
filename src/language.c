#include "language.h"
#include "config.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

typedef struct {
  char *id;
  char *name;
  char *tag;
  char **aliases;
} Language;

static GPtrArray *languages;
static gsize languages_initialized;

/* Only the source-language fallback and the preference sentinel are built in.
 * All other languages and optional locale aliases come from installed catalogs. */
static char *
normalize_tag(const char *id)
{
  char *tag = g_ascii_strdown(id, -1);
  char *encoding = strchr(tag, '.');
  if (encoding != NULL) {
    char *modifier = strchr(encoding, '@');
    if (modifier != NULL) memmove(encoding, modifier, strlen(modifier) + 1);
    else *encoding = '\0';
  }
  for (char *p = tag; *p != '\0'; p++) if (*p == '-') *p = '_';
  return tag;
}

static gboolean
catalog_id_is_safe(const char *id)
{
  if (id == NULL || !g_ascii_isalpha(*id)) return FALSE;
  for (const char *p = id; *p != '\0'; p++) {
    if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '@') return FALSE;
  }
  return TRUE;
}

static guint32
mo_word(const guint8 *data, gsize offset, gboolean little_endian)
{
  guint32 value;
  memcpy(&value, data + offset, sizeof value);
  return little_endian ? GUINT32_FROM_LE(value) : GUINT32_FROM_BE(value);
}

/* Read only the metadata entry, without changing gettext's current language.
 * Check every offset before touching data from a possibly damaged catalog. */
static char *
catalog_header(const char *path)
{
  g_autoptr(GMappedFile) file = g_mapped_file_new(path, FALSE, NULL);
  if (file == NULL) return NULL;
  gsize size = g_mapped_file_get_length(file);
  const guint8 *data = (const guint8 *)g_mapped_file_get_contents(file);
  if (size < 28) return NULL;
  guint32 magic = mo_word(data, 0, TRUE);
  gboolean little_endian = magic == 0x950412de;
  if (!little_endian && magic != 0xde120495) return NULL;
  if ((mo_word(data, 4, little_endian) >> 16) > 1) return NULL;
  guint32 count = mo_word(data, 8, little_endian);
  guint32 originals = mo_word(data, 12, little_endian);
  guint32 translations = mo_word(data, 16, little_endian);
  if (originals > size || translations > size ||
      count > (size - originals) / 8 || count > (size - translations) / 8) return NULL;
  for (guint32 i = 0; i < count; i++) {
    if (mo_word(data, originals + (gsize)i * 8, little_endian) != 0) continue;
    guint32 original_offset = mo_word(data, originals + (gsize)i * 8 + 4, little_endian);
    if (original_offset >= size || data[original_offset] != '\0') return NULL;
    guint32 length = mo_word(data, translations + (gsize)i * 8, little_endian);
    guint32 offset = mo_word(data, translations + (gsize)i * 8 + 4, little_endian);
    if (offset >= size || length >= size - offset || data[offset + length] != '\0' ||
        !g_utf8_validate((const char *)data + offset, length, NULL)) return NULL;
    return g_strndup((const char *)data + offset, length);
  }
  return NULL;
}

static char *
header_value(const char *header, const char *field)
{
  g_auto(GStrv) lines = g_strsplit(header, "\n", -1);
  gsize length = strlen(field);
  for (guint i = 0; lines[i] != NULL; i++) {
    if (g_ascii_strncasecmp(lines[i], field, length) == 0 && lines[i][length] == ':') {
      return g_strdup(g_strstrip(lines[i] + length + 1));
    }
  }
  return NULL;
}

static void
add_language(const char *id, const char *name, const char *aliases)
{
  Language *language = g_new0(Language, 1);
  language->id = g_strdup(id);
  language->name = g_strdup(name != NULL && *name != '\0' ? name : id);
  language->tag = normalize_tag(id);
  language->aliases = g_strsplit(aliases != NULL ? aliases : "", ";", -1);
  for (guint i = 0; language->aliases[i] != NULL; i++) {
    char *tag = normalize_tag(g_strstrip(language->aliases[i]));
    g_free(language->aliases[i]);
    language->aliases[i] = tag;
  }
  g_ptr_array_add(languages, language);
}

static gint
compare_languages(gconstpointer left, gconstpointer right)
{
  const Language *a = *(Language *const *)left;
  const Language *b = *(Language *const *)right;
  return g_strcmp0(a->id, b->id);
}

/* Does the root contain a catalog for our domain? Only well-formed directory
 * IDs are probed; discovery re-validates the matching files. */
gboolean
pumpkin_language_catalog_available(const char *root)
{
  g_autoptr(GDir) dir = g_dir_open(root, 0, NULL);
  if (dir == NULL) return FALSE;
  const char *id;
  while ((id = g_dir_read_name(dir)) != NULL) {
    if (!catalog_id_is_safe(id) || g_str_equal(id, "system") || g_str_equal(id, "en")) continue;
    g_autofree char *tag = normalize_tag(id);
    if (g_str_equal(tag, "c") || g_str_equal(tag, "posix")) continue;
    g_autofree char *path = g_build_filename(root, id, "LC_MESSAGES", GETTEXT_PACKAGE ".mo", NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) return TRUE;
  }
  return FALSE;
}

/* Resolve the gettext root shared by catalog discovery and binding: an
 * absolute SMASHED_PUMPKIN_LOCALEDIR always wins, then the configured
 * LOCALEDIR when it holds a catalog for this domain, then the build tree
 * for uninstalled development runs, then LOCALEDIR itself. */
const char *
pumpkin_language_catalog_root(void)
{
  const char *override = g_getenv("SMASHED_PUMPKIN_LOCALEDIR");
  if (override != NULL && g_path_is_absolute(override)) return override;
  if (pumpkin_language_catalog_available(LOCALEDIR)) return LOCALEDIR;
#ifdef BUILD_LOCALEDIR
  if (pumpkin_language_catalog_available(BUILD_LOCALEDIR)) return BUILD_LOCALEDIR;
#endif
  return LOCALEDIR;
}

static void
ensure_languages(void)
{
  if (g_once_init_enter(&languages_initialized)) {
    languages = g_ptr_array_new();
    const char *root = pumpkin_language_catalog_root();
    g_autoptr(GDir) dir = g_dir_open(root, 0, NULL);
    const char *id;
    while (dir != NULL && (id = g_dir_read_name(dir)) != NULL) {
      if (!catalog_id_is_safe(id) || g_str_equal(id, "system") || g_str_equal(id, "en")) continue;
      g_autofree char *tag = normalize_tag(id);
      if (g_str_equal(tag, "c") || g_str_equal(tag, "posix")) continue;
      g_autofree char *path = g_build_filename(root, id, "LC_MESSAGES", GETTEXT_PACKAGE ".mo", NULL);
      g_autofree char *header = catalog_header(path);
      if (header == NULL) continue;
      g_autofree char *name = header_value(header, "X-Language-Name");
      g_autofree char *aliases = header_value(header, "X-Language-Aliases");
      add_language(id, name, aliases);
    }
    g_ptr_array_sort(languages, compare_languages);
    add_language("en", "English", NULL);
    g_ptr_array_insert(languages, 0, g_ptr_array_steal_index(languages, languages->len - 1));
    add_language("system", "System language", NULL);
    g_ptr_array_insert(languages, 0, g_ptr_array_steal_index(languages, languages->len - 1));
    g_once_init_leave(&languages_initialized, 1);
  }
}

guint
pumpkin_language_count(void)
{
  ensure_languages();
  return languages->len;
}

gboolean
pumpkin_language_is_valid(const char *id)
{
  ensure_languages();
  for (guint i = 0; i < languages->len; i++) {
    Language *language = g_ptr_array_index(languages, i);
    if (g_strcmp0(id, language->id) == 0) return TRUE;
  }
  return FALSE;
}

guint
pumpkin_language_index(const char *id)
{
  ensure_languages();
  for (guint i = 0; i < languages->len; i++) {
    Language *language = g_ptr_array_index(languages, i);
    if (g_strcmp0(id, language->id) == 0) return i;
  }
  return 1; /* Unknown persisted values display the safe English fallback. */
}

const char *
pumpkin_language_from_index(guint index)
{
  ensure_languages();
  return ((Language *)g_ptr_array_index(languages, index < languages->len ? index : 1))->id;
}

const char *
pumpkin_language_name(guint index)
{
  ensure_languages();
  return ((Language *)g_ptr_array_index(languages, index < languages->len ? index : 1))->name;
}

static const char *
match_tag(const char *tag)
{
  /* A real catalog ID wins over an alias declared by another catalog. */
  for (guint i = 1; i < languages->len; i++) {
    Language *language = g_ptr_array_index(languages, i);
    if (g_str_equal(tag, language->tag)) return language->id;
  }
  for (guint i = 2; i < languages->len; i++) {
    Language *language = g_ptr_array_index(languages, i);
    for (guint a = 0; language->aliases[a] != NULL; a++) {
      if (*language->aliases[a] != '\0' && g_str_equal(tag, language->aliases[a])) return language->id;
    }
  }
  return NULL;
}

/* Try every preference and return the first matching catalog ID, or NULL when
 * nothing matches. C/POSIX and empty entries are skipped like gettext skips
 * them inside LANGUAGE lists; the caller decides the final fallback. */
static const char *
try_match_preferences(const char *const *preferences)
{
  ensure_languages();
  for (const char *const *p = preferences; p != NULL && *p != NULL; p++) {
    if (**p == '\0') continue;
    g_autofree char *tag = normalize_tag(*p);
    if (g_str_equal(tag, "c") || g_str_equal(tag, "posix")) continue;
    const char *match = match_tag(tag);
    if (match != NULL) return match;
    /* Fall back to a less specific installed catalog, never a different region
     * or script. Explicit cross-region equivalents belong in catalog metadata. */
    char *modifier = strchr(tag, '@');
    if (modifier != NULL) *modifier = '\0';
    while (TRUE) {
      match = match_tag(tag);
      if (match != NULL) return match;
      char *separator = strrchr(tag, '_');
      if (separator == NULL) break;
      *separator = '\0';
    }
  }
  return NULL;
}

const char *
pumpkin_language_detect(const char *const *preferences)
{
  ensure_languages();
  const char *match = try_match_preferences(preferences);
  return match != NULL ? match : "en";
}

static const char *
detect_environment(void)
{
  const char *messages = g_getenv("LC_ALL");
  if (messages == NULL || *messages == '\0') messages = g_getenv("LC_MESSAGES");
  if (messages == NULL || *messages == '\0') messages = g_getenv("LANG");
  /* An explicit POSIX/C locale requests source English, even with LANGUAGE set.
   * C.UTF-8 is a usable character locale, not an explicit language selection. */
  if (g_strcmp0(messages, "C") == 0 || g_strcmp0(messages, "POSIX") == 0) return "en";
  const char *language = g_getenv("LANGUAGE");
  if (language != NULL && *language != '\0') {
    g_auto(GStrv) preferences = g_strsplit(language, ":", -1);
    for (guint i = 0; preferences[i] != NULL; i++) g_strstrip(preferences[i]);
    /* Like gettext, fall back to the locale language when no LANGUAGE entry
     * matches, instead of persisting an English detection. */
    const char *match = try_match_preferences((const char *const *)preferences);
    if (match != NULL) return match;
  }
  if (messages != NULL && *messages != '\0') {
    const char *preferences[] = { messages, NULL };
    const char *match = try_match_preferences(preferences);
    if (match != NULL) return match;
  }
#ifdef G_OS_WIN32
  WCHAR name[LOCALE_NAME_MAX_LENGTH];
  if (GetUserDefaultLocaleName(name, G_N_ELEMENTS(name)) > 0) {
    g_autofree char *tag = g_utf16_to_utf8((const gunichar2 *)name, -1, NULL, NULL, NULL);
    const char *preferences[] = { tag, NULL };
    const char *match = try_match_preferences(preferences);
    if (match != NULL) return match;
  }
#endif
  return "en";
}

char *
pumpkin_language_prepare(GError **error)
{
  g_autofree char *path = g_build_filename(g_get_user_config_dir(), "smashed-pumpkin", "config.ini", NULL);
  g_autoptr(GKeyFile) key = g_key_file_new();
  g_autoptr(GError) load_error = NULL;
  if (!g_key_file_load_from_file(key, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                 &load_error) &&
      !g_error_matches(load_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    g_propagate_error(error, g_steal_pointer(&load_error));
    return g_strdup("en");
  }
  g_autoptr(GError) value_error = NULL;
  g_autofree char *saved = g_key_file_get_string(key, "behavior", "language", &value_error);
  if (value_error != NULL &&
      !g_error_matches(value_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND) &&
      !g_error_matches(value_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND)) {
    g_propagate_error(error, g_steal_pointer(&value_error));
    return g_strdup("en");
  }
  if (saved != NULL && *saved != '\0') {
    if (g_str_equal(saved, "system")) return g_strdup(detect_environment());
    return g_strdup(pumpkin_language_is_valid(saved) ? saved : "en");
  }

  const char *detected = detect_environment();
  g_key_file_set_string(key, "behavior", "language", detected);
  g_autofree char *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot create language configuration directory: %s", g_strerror(errno));
  } else {
    g_autofree char *data = g_key_file_to_data(key, NULL, NULL);
    g_file_set_contents_full(path, data, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                             0600, error);
  }
  return g_strdup(detected);
}
