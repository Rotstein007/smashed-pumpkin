#include "window-parse.h"

#include <limits.h>
#include <stdlib.h>

const char *
pumpkin_get_entry_text(GtkEntry *entry)
{
  if (entry == NULL) {
    return NULL;
  }
  return gtk_editable_get_text(GTK_EDITABLE(entry));
}

gboolean
pumpkin_parse_optional_positive_int(GtkEntry *entry, int *out_value, gboolean *has_value)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (has_value != NULL) {
    *has_value = FALSE;
  }
  if (text == NULL) {
    return TRUE;
  }
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  if (*text == '\0') {
    return TRUE;
  }
  if (has_value != NULL) {
    *has_value = TRUE;
  }
  char *endptr = NULL;
  long value = strtol(text, &endptr, 10);
  if (endptr == text) {
    return FALSE;
  }
  while (*endptr == ' ' || *endptr == '\t') {
    endptr++;
  }
  if (*endptr != '\0') {
    return FALSE;
  }
  if (value < 0 || value > INT_MAX) {
    return FALSE;
  }
  if (out_value != NULL) {
    *out_value = (int)value;
  }
  return TRUE;
}

int
pumpkin_get_entry_int_value(GtkEntry *entry)
{
  int value = 0;
  gboolean has_value = FALSE;
  if (entry == NULL || !pumpkin_parse_optional_positive_int(entry, &value, &has_value) || !has_value) {
    return 0;
  }
  return value;
}

gboolean
pumpkin_strings_equal(const char *a, const char *b)
{
  const char *left = a != NULL ? a : "";
  const char *right = b != NULL ? b : "";
  return g_strcmp0(left, right) == 0;
}

gboolean
pumpkin_entry_matches_string(GtkEntry *entry, const char *value)
{
  if (entry == NULL) {
    return TRUE;
  }
  return pumpkin_strings_equal(pumpkin_get_entry_text(entry), value);
}

gboolean
pumpkin_entry_matches_int(GtkEntry *entry, int value)
{
  if (entry == NULL) {
    return TRUE;
  }
  return pumpkin_get_entry_int_value(entry) == value;
}

gboolean
pumpkin_parse_clock_time_text(const char *text, int *out_hour, int *out_minute)
{
  if (text == NULL) {
    return FALSE;
  }
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  if (*text == '\0') {
    return FALSE;
  }

  char *end = NULL;
  long hour = strtol(text, &end, 10);
  if (end == text || *end != ':') {
    return FALSE;
  }
  const char *minutes_text = end + 1;
  if (!g_ascii_isdigit(minutes_text[0]) || !g_ascii_isdigit(minutes_text[1])) {
    return FALSE;
  }
  char *minutes_end = NULL;
  long minute = strtol(minutes_text, &minutes_end, 10);
  while (*minutes_end == ' ' || *minutes_end == '\t') {
    minutes_end++;
  }
  if (*minutes_end != '\0') {
    return FALSE;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return FALSE;
  }
  if (out_hour != NULL) {
    *out_hour = (int)hour;
  }
  if (out_minute != NULL) {
    *out_minute = (int)minute;
  }
  return TRUE;
}

gboolean
pumpkin_parse_clock_time_entry(GtkEntry *entry, int *out_hour, int *out_minute)
{
  if (entry == NULL) {
    return FALSE;
  }
  return pumpkin_parse_clock_time_text(gtk_editable_get_text(GTK_EDITABLE(entry)), out_hour, out_minute);
}

int
pumpkin_parse_limit_entry(GtkEntry *entry, int max_value)
{
  int value = 0;
  gboolean has_value = FALSE;
  if (entry == NULL || !pumpkin_parse_optional_positive_int(entry, &value, &has_value) || !has_value) {
    return 0;
  }
  if (max_value > 0 && value > max_value) {
    return max_value;
  }
  return value;
}
