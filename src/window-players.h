#pragma once

#include "window-internal.h"

void player_state_free(PlayerState *state);
guint64 player_state_effective_playtime(const PlayerState *state);
void player_states_set_dirty(PumpkinWindow *self);
void allow_deleted_player_tracking(PumpkinWindow *self, const char *uuid, const char *name);
PlayerState *ensure_player_state(PumpkinWindow *self, const char *uuid, const char *name, gboolean create);
void player_state_mark_online(PumpkinWindow *self, PlayerState *state, PlayerPlatform platform_hint);
void player_state_mark_offline(PumpkinWindow *self, PlayerState *state);
void player_states_mark_all_offline(PumpkinWindow *self);
int player_online_count(PumpkinWindow *self);
void player_states_clear(PumpkinWindow *self);
void player_states_load(PumpkinWindow *self, PumpkinServer *server);
void player_states_save(PumpkinWindow *self, PumpkinServer *server);
PlayerPlatform platform_from_line(const char *line);
PlayerPlatform platform_guess_from_uuid(const char *uuid);
char *extract_ip_from_socket_text(const char *text);
void remember_platform_hint_for_ip(PumpkinWindow *self, const char *ip, PlayerPlatform platform);
PlayerPlatform platform_hint_for_ip(PumpkinWindow *self, const char *ip);
PlayerPlatform take_pending_platform_hint(PumpkinWindow *self);
