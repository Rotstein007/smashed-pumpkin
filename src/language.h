#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Catalog discovery is cached for the process lifetime. IDs and optional display
 * names/locale aliases come from the active gettext directory, not a fixed list.
 * Returned strings are borrowed; system and source English are always available. */
guint pumpkin_language_count(void);
const char *pumpkin_language_name(guint index);
gboolean pumpkin_language_is_valid(const char *id);
guint pumpkin_language_index(const char *id);
const char *pumpkin_language_from_index(guint index);
const char *pumpkin_language_detect(const char *const *preferences);

/* The gettext root used by both catalog discovery and binding. An absolute
 * SMASHED_PUMPKIN_LOCALEDIR wins; otherwise installed catalogs take priority
 * and uninstalled development builds fall back to the build tree. */
const char *pumpkin_language_catalog_root(void);

/* Startup only, before GTK or threads. Preserve manual selections and unrelated
 * INI keys. Return an owned effective ID even when reading/saving reports an error.
 * Missing preferences are detected and saved once; system is resolved each time. */
char *pumpkin_language_prepare(GError **error);

G_END_DECLS
