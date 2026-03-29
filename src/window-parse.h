#pragma once

#include <gtk/gtk.h>

const char *pumpkin_get_entry_text(GtkEntry *entry);
int pumpkin_get_entry_int_value(GtkEntry *entry);
gboolean pumpkin_strings_equal(const char *a, const char *b);
gboolean pumpkin_entry_matches_string(GtkEntry *entry, const char *value);
gboolean pumpkin_entry_matches_int(GtkEntry *entry, int value);
gboolean pumpkin_parse_optional_positive_int(GtkEntry *entry, int *out_value, gboolean *has_value);
gboolean pumpkin_parse_clock_time_text(const char *text, int *out_hour, int *out_minute);
gboolean pumpkin_parse_clock_time_entry(GtkEntry *entry, int *out_hour, int *out_minute);
int pumpkin_parse_limit_entry(GtkEntry *entry, int max_value);
