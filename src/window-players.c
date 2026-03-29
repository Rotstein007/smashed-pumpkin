#include "window-players.h"

#include <glib/gstdio.h>
#include <time.h>

static char *
player_tracking_file(PumpkinServer *server)
{
  if (server == NULL) {
    return NULL;
  }
  g_autofree char *data_dir = pumpkin_server_get_data_dir(server);
  if (data_dir == NULL) {
    return NULL;
  }
  return g_build_filename(data_dir, "player-tracking.ini", NULL);
}

void
player_state_free(PlayerState *state)
{
  if (state == NULL) {
    return;
  }
  g_clear_pointer(&state->key, g_free);
  g_clear_pointer(&state->name, g_free);
  g_clear_pointer(&state->uuid, g_free);
  g_clear_pointer(&state->last_ip, g_free);
  g_free(state);
}

guint64
player_state_effective_playtime(const PlayerState *state)
{
  if (state == NULL) {
    return 0;
  }
  guint64 total = state->playtime_seconds;
  if (state->online && state->session_started_mono > 0) {
    gint64 now_mono = g_get_monotonic_time();
    if (now_mono > state->session_started_mono) {
      total += (guint64)((now_mono - state->session_started_mono) / G_USEC_PER_SEC);
    }
  }
  return total;
}

void
player_states_set_dirty(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  self->player_state_dirty = TRUE;
}

static void
repoint_player_indexes(PumpkinWindow *self, PlayerState *from_state, PlayerState *to_state)
{
  if (self == NULL || from_state == NULL || to_state == NULL) {
    return;
  }

  if (self->player_states_by_uuid != NULL) {
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->player_states_by_uuid);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      if (value == from_state) {
        g_hash_table_iter_replace(&iter, to_state);
      }
    }
  }

  if (self->player_states_by_name != NULL) {
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->player_states_by_name);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      if (value == from_state) {
        g_hash_table_iter_replace(&iter, to_state);
      }
    }
  }
}

static void
merge_player_states(PumpkinWindow *self, PlayerState *dst, PlayerState *src)
{
  if (self == NULL || dst == NULL || src == NULL || dst == src) {
    return;
  }

  if ((dst->name == NULL || dst->name[0] == '\0') && src->name != NULL && src->name[0] != '\0') {
    g_free(dst->name);
    dst->name = g_strdup(src->name);
  }
  if ((dst->uuid == NULL || dst->uuid[0] == '\0') && src->uuid != NULL && src->uuid[0] != '\0') {
    g_free(dst->uuid);
    dst->uuid = g_strdup(src->uuid);
  }
  if (dst->platform == PLAYER_PLATFORM_UNKNOWN && src->platform != PLAYER_PLATFORM_UNKNOWN) {
    dst->platform = src->platform;
  }
  if ((dst->last_ip == NULL || dst->last_ip[0] == '\0') &&
      src->last_ip != NULL && src->last_ip[0] != '\0') {
    g_free(dst->last_ip);
    dst->last_ip = g_strdup(src->last_ip);
  } else if (src->last_ip != NULL && src->last_ip[0] != '\0' &&
             src->last_online_unix >= dst->last_online_unix &&
             g_strcmp0(dst->last_ip, src->last_ip) != 0) {
    g_free(dst->last_ip);
    dst->last_ip = g_strdup(src->last_ip);
  }
  dst->ip_banned_hint = dst->ip_banned_hint || src->ip_banned_hint;
  if (dst->first_joined_unix <= 0 ||
      (src->first_joined_unix > 0 && src->first_joined_unix < dst->first_joined_unix)) {
    dst->first_joined_unix = src->first_joined_unix;
  }
  if (src->last_online_unix > dst->last_online_unix) {
    dst->last_online_unix = src->last_online_unix;
  }
  dst->playtime_seconds += src->playtime_seconds;

  if (src->online) {
    if (!dst->online) {
      dst->online = TRUE;
      dst->session_started_mono = src->session_started_mono > 0
                                    ? src->session_started_mono
                                    : g_get_monotonic_time();
    } else if (src->session_started_mono > 0 &&
               (dst->session_started_mono <= 0 || src->session_started_mono < dst->session_started_mono)) {
      dst->session_started_mono = src->session_started_mono;
    }
  }

  repoint_player_indexes(self, src, dst);
  if (self->player_states != NULL && src->key != NULL) {
    g_hash_table_remove(self->player_states, src->key);
  }
  player_states_set_dirty(self);
}

void
allow_deleted_player_tracking(PumpkinWindow *self, const char *uuid, const char *name)
{
  if (self == NULL || self->deleted_player_keys == NULL) {
    return;
  }

  gboolean changed = FALSE;
  g_autofree char *uuid_key = normalized_key(uuid);
  g_autofree char *name_key = normalized_key(name);

  if (uuid_key != NULL) {
    changed = g_hash_table_remove(self->deleted_player_keys, uuid_key) || changed;
  }
  if (name_key != NULL) {
    changed = g_hash_table_remove(self->deleted_player_keys, name_key) || changed;
  }
  if (changed) {
    player_states_set_dirty(self);
  }
}

PlayerState *
ensure_player_state(PumpkinWindow *self, const char *uuid, const char *name, gboolean create)
{
  if (self == NULL || self->player_states == NULL) {
    return NULL;
  }

  g_autofree char *uuid_key = normalized_key(uuid);
  g_autofree char *name_key = normalized_key(name);
  if (self->deleted_player_keys != NULL) {
    if (uuid_key != NULL && g_hash_table_contains(self->deleted_player_keys, uuid_key)) {
      return NULL;
    }
    if (name_key != NULL && g_hash_table_contains(self->deleted_player_keys, name_key)) {
      return NULL;
    }
  }
  PlayerState *state_by_uuid = NULL;
  PlayerState *state_by_name = NULL;

  if (uuid_key != NULL && self->player_states_by_uuid != NULL) {
    state_by_uuid = g_hash_table_lookup(self->player_states_by_uuid, uuid_key);
  }
  if (name_key != NULL && self->player_states_by_name != NULL) {
    state_by_name = g_hash_table_lookup(self->player_states_by_name, name_key);
  }

  PlayerState *state = NULL;
  if (state_by_uuid != NULL) {
    state = state_by_uuid;
    if (state_by_name != NULL && state_by_name != state_by_uuid) {
      merge_player_states(self, state, state_by_name);
    }
  } else if (state_by_name != NULL) {
    state = state_by_name;
  } else if (create) {
    const char *key = uuid_key != NULL ? uuid_key : name_key;
    if (key == NULL) {
      return NULL;
    }
    state = g_new0(PlayerState, 1);
    state->key = g_strdup(key);
    g_hash_table_insert(self->player_states, g_strdup(key), state);
    player_states_set_dirty(self);
  } else {
    return NULL;
  }

  if (state != NULL && uuid != NULL && *uuid != '\0') {
    if (state->uuid == NULL || *state->uuid == '\0') {
      g_free(state->uuid);
      state->uuid = g_strdup(uuid);
      player_states_set_dirty(self);
    }
    if (uuid_key != NULL && self->player_states_by_uuid != NULL) {
      g_hash_table_replace(self->player_states_by_uuid, g_strdup(uuid_key), state);
    }
  }

  if (state != NULL && name != NULL && *name != '\0') {
    if (state->name == NULL || g_strcmp0(state->name, name) != 0) {
      g_free(state->name);
      state->name = g_strdup(name);
      player_states_set_dirty(self);
    }
    if (name_key != NULL && self->player_states_by_name != NULL) {
      g_hash_table_replace(self->player_states_by_name, g_strdup(name_key), state);
    }
  }

  return state;
}

void
player_state_mark_online(PumpkinWindow *self, PlayerState *state, PlayerPlatform platform_hint)
{
  if (self == NULL || state == NULL) {
    return;
  }
  gint64 now_unix = (gint64)time(NULL);
  if (platform_hint != PLAYER_PLATFORM_UNKNOWN) {
    state->platform = platform_hint;
  }
  if (!state->online) {
    state->online = TRUE;
    state->session_started_mono = g_get_monotonic_time();
    if (state->first_joined_unix <= 0) {
      state->first_joined_unix = now_unix;
    }
  }
  if (now_unix > state->last_online_unix) {
    state->last_online_unix = now_unix;
  }
  if (self->live_player_names != NULL) {
    const char *presence = state->name != NULL ? state->name : state->uuid;
    if (presence != NULL && *presence != '\0') {
      g_hash_table_replace(self->live_player_names, g_strdup(presence), g_strdup(presence));
    }
  }
  player_states_set_dirty(self);
}

void
player_state_mark_offline(PumpkinWindow *self, PlayerState *state)
{
  if (self == NULL || state == NULL) {
    return;
  }
  gint64 now_unix = (gint64)time(NULL);
  if (state->online) {
    if (state->session_started_mono > 0) {
      gint64 now_mono = g_get_monotonic_time();
      if (now_mono > state->session_started_mono) {
        state->playtime_seconds += (guint64)((now_mono - state->session_started_mono) / G_USEC_PER_SEC);
      }
    }
    state->session_started_mono = 0;
    state->online = FALSE;
    if (now_unix > state->last_online_unix) {
      state->last_online_unix = now_unix;
    }
    player_states_set_dirty(self);
  }

  if (self->live_player_names != NULL) {
    if (state->name != NULL) {
      g_hash_table_remove(self->live_player_names, state->name);
    }
    if (state->uuid != NULL) {
      g_hash_table_remove(self->live_player_names, state->uuid);
    }
  }
}

void
player_states_mark_all_offline(PumpkinWindow *self)
{
  if (self == NULL || self->player_states == NULL) {
    return;
  }
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->player_states);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    PlayerState *state = value;
    player_state_mark_offline(self, state);
  }
}

int
player_online_count(PumpkinWindow *self)
{
  if (self == NULL || self->player_states == NULL) {
    return 0;
  }
  int count = 0;
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->player_states);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    PlayerState *state = value;
    if (state->online) {
      count++;
    }
  }
  return count;
}

void
player_states_clear(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  if (self->live_player_names != NULL) {
    g_hash_table_remove_all(self->live_player_names);
  }
  if (self->platform_hint_by_ip != NULL) {
    g_hash_table_remove_all(self->platform_hint_by_ip);
  }
  if (self->player_states != NULL) {
    g_hash_table_remove_all(self->player_states);
  }
  if (self->player_states_by_uuid != NULL) {
    g_hash_table_remove_all(self->player_states_by_uuid);
  }
  if (self->player_states_by_name != NULL) {
    g_hash_table_remove_all(self->player_states_by_name);
  }
  if (self->deleted_player_keys != NULL) {
    g_hash_table_remove_all(self->deleted_player_keys);
  }
  self->player_state_dirty = FALSE;
  invalidate_player_list_signature(self);
}

void
player_states_load(PumpkinWindow *self, PumpkinServer *server)
{
  if (self == NULL) {
    return;
  }
  player_states_clear(self);
  if (server == NULL) {
    return;
  }

  g_autofree char *path = player_tracking_file(server);
  if (path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS)) {
    return;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
    return;
  }

  if (self->deleted_player_keys != NULL && g_key_file_has_group(key_file, "__deleted")) {
    gsize deleted_len = 0;
    g_auto(GStrv) deleted_keys = g_key_file_get_keys(key_file, "__deleted", &deleted_len, NULL);
    for (gsize i = 0; i < deleted_len; i++) {
      const char *entry_key = deleted_keys[i];
      if (entry_key == NULL || *entry_key == '\0') {
        continue;
      }
      g_autofree char *stored = g_key_file_get_string(key_file, "__deleted", entry_key, NULL);
      g_autofree char *normalized = normalized_key(stored);
      if (normalized != NULL) {
        g_hash_table_add(self->deleted_player_keys, g_strdup(normalized));
      }
    }
  }

  gsize groups_len = 0;
  g_auto(GStrv) groups = g_key_file_get_groups(key_file, &groups_len);
  for (gsize i = 0; i < groups_len; i++) {
    const char *group = groups[i];
    if (group == NULL || *group == '\0') {
      continue;
    }
    if (g_strcmp0(group, "__deleted") == 0) {
      continue;
    }

    PlayerState *state = g_new0(PlayerState, 1);
    state->key = g_strdup(group);
    state->name = g_key_file_get_string(key_file, group, "name", NULL);
    state->uuid = g_key_file_get_string(key_file, group, "uuid", NULL);
    state->last_ip = g_key_file_get_string(key_file, group, "last_ip", NULL);
    state->platform = (PlayerPlatform)g_key_file_get_integer(key_file, group, "platform", NULL);
    if (state->platform < PLAYER_PLATFORM_UNKNOWN || state->platform > PLAYER_PLATFORM_BEDROCK) {
      state->platform = PLAYER_PLATFORM_UNKNOWN;
    }
    state->first_joined_unix = g_key_file_get_int64(key_file, group, "first_joined_unix", NULL);
    state->last_online_unix = g_key_file_get_int64(key_file, group, "last_online_unix", NULL);
    gint64 saved_playtime = g_key_file_get_int64(key_file, group, "playtime_seconds", NULL);
    state->playtime_seconds = saved_playtime > 0 ? (guint64)saved_playtime : 0;
    state->online = FALSE;
    state->session_started_mono = 0;

    g_hash_table_insert(self->player_states, g_strdup(group), state);

    g_autofree char *uuid_key = normalized_key(state->uuid);
    if (uuid_key != NULL) {
      g_hash_table_replace(self->player_states_by_uuid, g_strdup(uuid_key), state);
    }
    g_autofree char *name_key = normalized_key(state->name);
    if (name_key != NULL) {
      g_hash_table_replace(self->player_states_by_name, g_strdup(name_key), state);
    }
  }
  self->player_state_dirty = FALSE;
  invalidate_player_list_signature(self);
}

void
player_states_save(PumpkinWindow *self, PumpkinServer *server)
{
  if (self == NULL || server == NULL || self->player_states == NULL || !self->player_state_dirty) {
    return;
  }

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->player_states);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *section = key;
    PlayerState *state = value;
    if (section == NULL || state == NULL) {
      continue;
    }
    if (state->name != NULL) {
      g_key_file_set_string(key_file, section, "name", state->name);
    }
    if (state->uuid != NULL) {
      g_key_file_set_string(key_file, section, "uuid", state->uuid);
    }
    if (state->last_ip != NULL && *state->last_ip != '\0') {
      g_key_file_set_string(key_file, section, "last_ip", state->last_ip);
    }
    g_key_file_set_integer(key_file, section, "platform", (int)state->platform);
    g_key_file_set_int64(key_file, section, "first_joined_unix", state->first_joined_unix);
    g_key_file_set_int64(key_file, section, "last_online_unix", state->last_online_unix);
    g_key_file_set_int64(key_file, section, "playtime_seconds",
                         (gint64)player_state_effective_playtime(state));
  }

  if (self->deleted_player_keys != NULL && g_hash_table_size(self->deleted_player_keys) > 0) {
    GHashTableIter deleted_iter;
    gpointer deleted_key = NULL;
    gpointer deleted_value = NULL;
    guint index = 0;
    g_hash_table_iter_init(&deleted_iter, self->deleted_player_keys);
    while (g_hash_table_iter_next(&deleted_iter, &deleted_key, &deleted_value)) {
      const char *deleted_token = deleted_key;
      if (deleted_token == NULL || *deleted_token == '\0') {
        continue;
      }
      g_autofree char *entry_key = g_strdup_printf("k%u", index++);
      g_key_file_set_string(key_file, "__deleted", entry_key, deleted_token);
    }
  }

  gsize out_len = 0;
  g_autofree char *serialized = g_key_file_to_data(key_file, &out_len, NULL);
  if (serialized == NULL) {
    return;
  }

  g_autofree char *path = player_tracking_file(server);
  if (path == NULL) {
    return;
  }
  if (g_file_set_contents(path, serialized, (gssize)out_len, NULL)) {
    self->player_state_dirty = FALSE;
    self->last_player_state_flush_at = g_get_monotonic_time();
  }
}

PlayerPlatform
platform_from_line(const char *line)
{
  if (line == NULL) {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  g_autofree char *lower = g_ascii_strdown(line, -1);
  if (strstr(lower, "bedrock") != NULL ||
      strstr(lower, "floodgate") != NULL ||
      strstr(lower, "geyser") != NULL ||
      strstr(lower, "raknet") != NULL) {
    return PLAYER_PLATFORM_BEDROCK;
  }
  if (strstr(lower, "java") != NULL || strstr(lower, "java edition") != NULL) {
    return PLAYER_PLATFORM_JAVA;
  }
  return PLAYER_PLATFORM_UNKNOWN;
}

PlayerPlatform
platform_guess_from_uuid(const char *uuid)
{
  if (uuid == NULL || *uuid == '\0') {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  g_autofree char *lower = g_ascii_strdown(uuid, -1);
  if (g_str_has_prefix(lower, "00000000-0000-0000-")) {
    return PLAYER_PLATFORM_BEDROCK;
  }
  return PLAYER_PLATFORM_UNKNOWN;
}

char *
extract_ip_from_socket_text(const char *text)
{
  if (text == NULL || *text == '\0') {
    return NULL;
  }

  while (*text == ' ' || *text == '\t') {
    text++;
  }
  if (*text == '\0') {
    return NULL;
  }

  g_autofree char *tmp = g_strdup(text);
  g_strstrip(tmp);
  gsize len = strlen(tmp);
  while (len > 0 && (tmp[len - 1] == ',' || tmp[len - 1] == ')' || tmp[len - 1] == ']')) {
    tmp[--len] = '\0';
  }

  if (tmp[0] == '[') {
    const char *end = strchr(tmp, ']');
    if (end != NULL && end > tmp + 1) {
      return g_strndup(tmp + 1, (gsize)(end - (tmp + 1)));
    }
  }

  const char *last_colon = strrchr(tmp, ':');
  if (last_colon != NULL) {
    gboolean has_other_colon = FALSE;
    for (const char *p = tmp; p < last_colon; p++) {
      if (*p == ':') {
        has_other_colon = TRUE;
        break;
      }
    }
    if (!has_other_colon) {
      return g_strndup(tmp, (gsize)(last_colon - tmp));
    }
  }

  return g_strdup(tmp);
}

void
remember_platform_hint_for_ip(PumpkinWindow *self, const char *ip, PlayerPlatform platform)
{
  if (self == NULL || self->platform_hint_by_ip == NULL || ip == NULL || *ip == '\0' ||
      platform == PLAYER_PLATFORM_UNKNOWN) {
    return;
  }
  g_autofree char *ip_key = normalized_key(ip);
  if (ip_key == NULL || *ip_key == '\0') {
    return;
  }
  g_hash_table_replace(self->platform_hint_by_ip, g_strdup(ip_key), GINT_TO_POINTER((int)platform));
}

PlayerPlatform
platform_hint_for_ip(PumpkinWindow *self, const char *ip)
{
  if (self == NULL || self->platform_hint_by_ip == NULL || ip == NULL || *ip == '\0') {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  g_autofree char *ip_key = normalized_key(ip);
  if (ip_key == NULL || *ip_key == '\0') {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  gpointer raw = g_hash_table_lookup(self->platform_hint_by_ip, ip_key);
  int value = GPOINTER_TO_INT(raw);
  if (value < PLAYER_PLATFORM_UNKNOWN || value > PLAYER_PLATFORM_BEDROCK) {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  return (PlayerPlatform)value;
}

PlayerPlatform
take_pending_platform_hint(PumpkinWindow *self)
{
  if (self == NULL) {
    return PLAYER_PLATFORM_UNKNOWN;
  }
  if (self->pending_bedrock_platform_hints > 0) {
    self->pending_bedrock_platform_hints--;
    return PLAYER_PLATFORM_BEDROCK;
  }
  if (self->pending_java_platform_hints > 0) {
    self->pending_java_platform_hints--;
    return PLAYER_PLATFORM_JAVA;
  }
  return PLAYER_PLATFORM_UNKNOWN;
}
