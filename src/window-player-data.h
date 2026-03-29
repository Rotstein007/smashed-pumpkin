#pragma once

#include "window-internal.h"

typedef struct {
  char *name;
  char *uuid;
  char *ip;
  char *reason;
  char *created;
  char *source;
  char *expires;
  int op_level;
  gboolean bypasses_player_limit;
} PlayerEntry;

char *extract_json_string_field(const char *object_text, const char *field);
int extract_json_int_field(const char *object_text, const char *field, int default_value);
gboolean extract_json_bool_field(const char *object_text, const char *field, gboolean default_value);
void add_name_uuid_pairs(GHashTable *map, const char *contents);
char *resolve_data_file(PumpkinServer *server, const char *filename);
GPtrArray *load_player_entries_from_file(const char *path);
char *pick_latest_banned_ip(PumpkinWindow *self);
gboolean player_lookup_contains_state(GHashTable *set, const PlayerState *state);
int player_lookup_op_level_for_state(GHashTable *map, const PlayerState *state);
const char *player_lookup_reason_for_state(GHashTable *map, const PlayerState *state);
