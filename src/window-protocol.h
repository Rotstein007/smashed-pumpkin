#pragma once

#include <glib.h>

char *pumpkin_strip_ansi(const char *line);
gboolean pumpkin_query_minecraft_players(const char *host, int port, int *out_players, int *out_max_players);
gboolean pumpkin_is_player_list_snapshot_line(const char *line);
gboolean pumpkin_parse_player_list_snapshot_line(const char *line, int *out_count, char **out_names_csv);
gboolean pumpkin_parse_tps_from_line(const char *line, double *out);
