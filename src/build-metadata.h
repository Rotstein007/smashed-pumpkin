#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Build metadata is persisted and may be used as a fallback cache/update
 * identity. Its legacy "Build "/"Stable "/"Nightly " tokens are wire data,
 * not translated UI strings. Never pass formatted display labels back here.
 * Recognized timestamps use exact ASCII YYYY-MM-DD or YYYY-MM-DD HH:MM,
 * optionally prefixed by "Build "; only date-times may end in " UTC".
 * Other spellings remain opaque, including variable-width or signed fields. */
char *pumpkin_build_metadata_from_unix(gint64 unix_time);
gboolean pumpkin_build_metadata_equal(const char *left, const char *right);

/* Presentation only: applies the user's date/time format to known timestamps.
 * Unknown/previously formatted legacy labels are preserved, not guessed. */
char *pumpkin_build_metadata_format(const char *metadata, const char *date_time_format);

G_END_DECLS
