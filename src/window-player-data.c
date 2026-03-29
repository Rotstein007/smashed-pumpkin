#include "window-player-data.h"

#include <stdlib.h>

void
add_name_uuid_pairs(GHashTable *map, const char *contents)
{
  if (contents == NULL) {
    return;
  }

  g_autoptr(GRegex) regex = g_regex_new(
    "\"uuid\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"name\"\\s*:\\s*\"([^\"]+)\"|\"name\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"uuid\"\\s*:\\s*\"([^\"]+)\"",
    G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, contents, 0, &match)) {
    return;
  }

  while (g_match_info_matches(match)) {
    g_autofree char *uuid1 = g_match_info_fetch(match, 1);
    g_autofree char *name1 = g_match_info_fetch(match, 2);
    g_autofree char *name2 = g_match_info_fetch(match, 3);
    g_autofree char *uuid2 = g_match_info_fetch(match, 4);

    if (uuid1 != NULL && name1 != NULL) {
      g_hash_table_replace(map, g_strdup(uuid1), g_strdup(name1));
    } else if (uuid2 != NULL && name2 != NULL) {
      g_hash_table_replace(map, g_strdup(uuid2), g_strdup(name2));
    }

    g_match_info_next(match, NULL);
  }
}

static void
player_entry_free(PlayerEntry *entry)
{
  if (entry == NULL) {
    return;
  }
  g_clear_pointer(&entry->name, g_free);
  g_clear_pointer(&entry->uuid, g_free);
  g_clear_pointer(&entry->ip, g_free);
  g_clear_pointer(&entry->reason, g_free);
  g_clear_pointer(&entry->created, g_free);
  g_clear_pointer(&entry->source, g_free);
  g_clear_pointer(&entry->expires, g_free);
  g_free(entry);
}

char *
extract_json_string_field(const char *object_text, const char *field)
{
  if (object_text == NULL || field == NULL || *field == '\0') {
    return NULL;
  }
  g_autofree char *pattern = g_strdup_printf("\"%s\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"", field);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  if (regex == NULL) {
    return NULL;
  }
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(regex, object_text, 0, &match) || !g_match_info_matches(match)) {
    return NULL;
  }
  return g_match_info_fetch(match, 1);
}

int
extract_json_int_field(const char *object_text, const char *field, int default_value)
{
  if (object_text == NULL || field == NULL || *field == '\0') {
    return default_value;
  }
  g_autofree char *pattern = g_strdup_printf("\"%s\"\\s*:\\s*(-?[0-9]+)", field);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  if (regex == NULL) {
    return default_value;
  }
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(regex, object_text, 0, &match) || !g_match_info_matches(match)) {
    return default_value;
  }
  g_autofree char *value_txt = g_match_info_fetch(match, 1);
  if (value_txt == NULL || *value_txt == '\0') {
    return default_value;
  }
  return (int)strtol(value_txt, NULL, 10);
}

gboolean
extract_json_bool_field(const char *object_text, const char *field, gboolean default_value)
{
  if (object_text == NULL || field == NULL || *field == '\0') {
    return default_value;
  }
  g_autofree char *pattern = g_strdup_printf("\"%s\"\\s*:\\s*(true|false)", field);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  if (regex == NULL) {
    return default_value;
  }
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(regex, object_text, 0, &match) || !g_match_info_matches(match)) {
    return default_value;
  }
  g_autofree char *value_txt = g_match_info_fetch(match, 1);
  if (value_txt == NULL) {
    return default_value;
  }
  return g_ascii_strcasecmp(value_txt, "true") == 0;
}

char *
resolve_data_file(PumpkinServer *server, const char *filename)
{
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  g_autofree char *path = g_build_filename(data_dir, "data", filename, NULL);
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    return g_strdup(path);
  }
  g_autofree char *fallback = g_build_filename(data_dir, filename, NULL);
  if (g_file_test(fallback, G_FILE_TEST_EXISTS)) {
    return g_strdup(fallback);
  }
  return g_strdup(path);
}

GPtrArray *
load_player_entries_from_file(const char *path)
{
  if (path == NULL) {
    return NULL;
  }

  g_autofree char *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL)) {
    return NULL;
  }

  g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func((GDestroyNotify)player_entry_free);
  g_autoptr(GRegex) regex = g_regex_new("\\{[^\\{\\}]*\\}", G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, contents, 0, &match)) {
    return g_steal_pointer(&entries);
  }

  while (g_match_info_matches(match)) {
    g_autofree char *object_text = g_match_info_fetch(match, 0);
    g_autofree char *name = extract_json_string_field(object_text, "name");
    g_autofree char *uuid = extract_json_string_field(object_text, "uuid");
    g_autofree char *ip = extract_json_string_field(object_text, "ip");
    g_autofree char *reason = extract_json_string_field(object_text, "reason");
    g_autofree char *created = extract_json_string_field(object_text, "created");
    g_autofree char *source = extract_json_string_field(object_text, "source");
    g_autofree char *expires = extract_json_string_field(object_text, "expires");
    int op_level = extract_json_int_field(object_text, "level", -1);
    gboolean bypasses_limit = extract_json_bool_field(object_text, "bypasses_player_limit", FALSE);
    if (!bypasses_limit) {
      bypasses_limit = extract_json_bool_field(object_text, "bypassesPlayerLimit", FALSE);
    }

    if ((name != NULL && *name != '\0') || (ip != NULL && *ip != '\0')) {
      PlayerEntry *entry = g_new0(PlayerEntry, 1);
      entry->name = (name != NULL && *name != '\0') ? g_strdup(name) : NULL;
      entry->uuid = (uuid != NULL && *uuid != '\0') ? g_strdup(uuid) : NULL;
      entry->ip = (ip != NULL && *ip != '\0') ? g_strdup(ip) : NULL;
      entry->reason = (reason != NULL && *reason != '\0') ? g_strdup(reason) : NULL;
      entry->created = (created != NULL && *created != '\0') ? g_strdup(created) : NULL;
      entry->source = (source != NULL && *source != '\0') ? g_strdup(source) : NULL;
      entry->expires = (expires != NULL && *expires != '\0') ? g_strdup(expires) : NULL;
      entry->op_level = op_level;
      entry->bypasses_player_limit = bypasses_limit;
      g_ptr_array_add(entries, entry);
    }

    g_match_info_next(match, NULL);
  }

  return g_steal_pointer(&entries);
}

char *
pick_latest_banned_ip(PumpkinWindow *self)
{
  if (self == NULL || self->current == NULL) {
    return NULL;
  }

  g_autofree char *banned_ips_path = resolve_data_file(self->current, "banned-ips.json");
  g_autoptr(GPtrArray) banned_ip_entries = load_player_entries_from_file(banned_ips_path);
  if (banned_ip_entries == NULL || banned_ip_entries->len == 0) {
    return NULL;
  }

  const char *best_ip = NULL;
  const char *best_created = NULL;
  for (guint i = 0; i < banned_ip_entries->len; i++) {
    PlayerEntry *entry = g_ptr_array_index(banned_ip_entries, i);
    if (entry == NULL || entry->ip == NULL || *entry->ip == '\0') {
      continue;
    }
    if (best_ip == NULL) {
      best_ip = entry->ip;
      best_created = entry->created;
      continue;
    }

    if (entry->created != NULL && *entry->created != '\0') {
      if (best_created == NULL || *best_created == '\0' || g_strcmp0(entry->created, best_created) > 0) {
        best_ip = entry->ip;
        best_created = entry->created;
      }
    }
  }

  return best_ip != NULL ? g_strdup(best_ip) : NULL;
}

gboolean
player_lookup_contains_state(GHashTable *set, const PlayerState *state)
{
  if (set == NULL || state == NULL) {
    return FALSE;
  }
  g_autofree char *uuid_key = normalized_key(state->uuid);
  if (uuid_key != NULL && g_hash_table_contains(set, uuid_key)) {
    return TRUE;
  }
  g_autofree char *name_key = normalized_key(state->name);
  if (name_key != NULL && g_hash_table_contains(set, name_key)) {
    return TRUE;
  }
  return FALSE;
}

int
player_lookup_op_level_for_state(GHashTable *map, const PlayerState *state)
{
  if (map == NULL || state == NULL) {
    return -1;
  }
  g_autofree char *uuid_key = normalized_key(state->uuid);
  if (uuid_key != NULL) {
    gpointer raw = g_hash_table_lookup(map, uuid_key);
    if (raw != NULL) {
      return GPOINTER_TO_INT(raw) - 1;
    }
  }
  g_autofree char *name_key = normalized_key(state->name);
  if (name_key != NULL) {
    gpointer raw = g_hash_table_lookup(map, name_key);
    if (raw != NULL) {
      return GPOINTER_TO_INT(raw) - 1;
    }
  }
  return -1;
}

const char *
player_lookup_reason_for_state(GHashTable *map, const PlayerState *state)
{
  if (map == NULL || state == NULL) {
    return NULL;
  }
  g_autofree char *uuid_key = normalized_key(state->uuid);
  if (uuid_key != NULL) {
    const char *reason = g_hash_table_lookup(map, uuid_key);
    if (reason != NULL) {
      return reason;
    }
  }
  g_autofree char *name_key = normalized_key(state->name);
  if (name_key != NULL) {
    return g_hash_table_lookup(map, name_key);
  }
  return NULL;
}
