#include "build-metadata.h"
#include "i18n.h"

#include <string.h>

static int
decimal_pair(const char *text)
{
  return (text[0] - '0') * 10 + text[1] - '0';
}

/* Only parse the old, locale-independent YMD metadata grammar. DMY/MDY labels
 * saved by older versions are opaque: inferring their timezone or date order
 * from today's UI preferences would corrupt the build identity. The returned
 * date holds the original components; unqualified legacy dates are wall time,
 * not UTC instants. */
static GDateTime *
parse_build_time(const char *metadata, gboolean *is_utc, gboolean *has_time)
{
  if (metadata == NULL || *metadata == '\0') return NULL;
  const char *text = g_str_has_prefix(metadata, "Build ") ? metadata + 6 : metadata;
  gsize length = strlen(text);
  if (length != 10 && length != 16 && length != 20) return NULL;

  /* Numeric scanf conversions also accept signs, short fields and whitespace.
   * Only the exact ASCII grammar is comparable; everything else stays opaque. */
  for (gsize i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (text[i] != '-') return NULL;
    } else if (!g_ascii_isdigit(text[i])) return NULL;
  }
  *has_time = length != 10;
  *is_utc = length == 20;
  if (*has_time) {
    for (gsize i = 10; i < 16; ++i) {
      if (i == 10) {
        if (text[i] != ' ') return NULL;
      } else if (i == 13) {
        if (text[i] != ':') return NULL;
      } else if (!g_ascii_isdigit(text[i])) return NULL;
    }
  }
  if (*is_utc && memcmp(text + 16, " UTC", 4) != 0) return NULL;

  int year = decimal_pair(text) * 100 + decimal_pair(text + 2);
  int month = decimal_pair(text + 5), day = decimal_pair(text + 8);
  int hour = *has_time ? decimal_pair(text + 11) : 0;
  int minute = *has_time ? decimal_pair(text + 14) : 0;
  if (!g_date_valid_dmy(day, month, year) || hour > 23 || minute > 59) {
    return NULL;
  }
  return g_date_time_new_utc(year, month, day, hour, minute, 0.0);
}

char *
pumpkin_build_metadata_from_unix(gint64 unix_time)
{
  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(unix_time);
  if (utc == NULL) return NULL;
  g_autofree char *timestamp = g_date_time_format(utc, "%Y-%m-%d %H:%M UTC");
  /* Stable metadata grammar, deliberately not a translation entry. */
  return g_strdup_printf("Build %s", timestamp);
}

gboolean
pumpkin_build_metadata_equal(const char *left, const char *right)
{
  if (g_strcmp0(left, right) == 0) return TRUE;
  gboolean left_utc = FALSE, right_utc = FALSE, left_time = FALSE, right_time = FALSE;
  g_autoptr(GDateTime) left_date = parse_build_time(left, &left_utc, &left_time);
  g_autoptr(GDateTime) right_date = parse_build_time(right, &right_utc, &right_time);
  return left_date != NULL && right_date != NULL &&
    left_utc == right_utc && left_time == right_time &&
    g_date_time_equal(left_date, right_date);
}

char *
pumpkin_build_metadata_format(const char *metadata, const char *date_time_format)
{
  if (metadata == NULL || *metadata == '\0') return NULL;
  gboolean is_utc = FALSE, has_time = FALSE;
  g_autoptr(GDateTime) date = parse_build_time(metadata, &is_utc, &has_time);
  if (date != NULL) {
    g_autoptr(GDateTime) display_date = is_utc ? g_date_time_to_local(date) : g_date_time_ref(date);
    g_autofree char *formatted = g_date_time_format(display_date,
      date_time_format != NULL ? date_time_format : "%Y-%m-%d %H:%M");
    /* Translators: %s is a display date/time, not persisted build metadata. */
    return g_strdup_printf(_("Build %s"), formatted);
  }
  if (g_str_has_prefix(metadata, "Stable ")) {
    /* Translators: %s is an unmodified version identifier. */
    return g_strdup_printf(_("Stable %s"), metadata + 7);
  }
  if (g_str_has_prefix(metadata, "Nightly ")) {
    /* Translators: %s is an unmodified nightly build identifier. */
    return g_strdup_printf(_("Nightly %s"), metadata + 8);
  }
  return g_strdup(metadata);
}
