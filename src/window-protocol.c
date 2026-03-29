#include "window-protocol.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

char *
pumpkin_strip_ansi(const char *line)
{
  if (line == NULL) {
    return NULL;
  }

  GString *out = g_string_sized_new(strlen(line));
  for (const char *p = line; *p != '\0'; ) {
    if ((guchar)*p == 0x1B && p[1] == '[') {
      p += 2;
      while (*p != '\0' && !((*p >= '@' && *p <= '~'))) {
        p++;
      }
      if (*p != '\0') {
        p++;
      }
      continue;
    }
    g_string_append_c(out, *p);
    p++;
  }
  return g_string_free(out, FALSE);
}

static void
slp_varint_append(GByteArray *array, guint32 value)
{
  do {
    guint8 temp = (guint8)(value & 0x7F);
    value >>= 7;
    if (value != 0) {
      temp |= 0x80;
    }
    g_byte_array_append(array, &temp, 1);
  } while (value != 0);
}

static gboolean
slp_read_exact(GInputStream *stream, guint8 *buffer, gsize length)
{
  if (stream == NULL || buffer == NULL || length == 0) {
    return FALSE;
  }
  gsize bytes_read = 0;
  return g_input_stream_read_all(stream, buffer, length, &bytes_read, NULL, NULL) && bytes_read == length;
}

static gboolean
slp_read_varint_stream(GInputStream *stream, guint32 *out_value)
{
  if (stream == NULL || out_value == NULL) {
    return FALSE;
  }
  guint32 value = 0;
  int position = 0;
  while (position < 35) {
    guint8 byte = 0;
    if (!slp_read_exact(stream, &byte, 1)) {
      return FALSE;
    }
    value |= (guint32)(byte & 0x7F) << position;
    if ((byte & 0x80) == 0) {
      *out_value = value;
      return TRUE;
    }
    position += 7;
  }
  return FALSE;
}

static gboolean
slp_read_varint_buffer(const guint8 *data, gsize length, gsize *offset, guint32 *out_value)
{
  if (data == NULL || offset == NULL || out_value == NULL) {
    return FALSE;
  }
  guint32 value = 0;
  int position = 0;
  while (position < 35 && *offset < length) {
    guint8 byte = data[*offset];
    (*offset)++;
    value |= (guint32)(byte & 0x7F) << position;
    if ((byte & 0x80) == 0) {
      *out_value = value;
      return TRUE;
    }
    position += 7;
  }
  return FALSE;
}

static gboolean
parse_slp_players_json(const char *json, int *out_players, int *out_max_players)
{
  if (json == NULL || out_players == NULL || out_max_players == NULL) {
    return FALSE;
  }

  g_autoptr(GRegex) players_re = g_regex_new("\"players\"\\s*:\\s*\\{([^}]*)\\}",
                                              G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autoptr(GMatchInfo) players_match = NULL;
  if (!g_regex_match(players_re, json, 0, &players_match)) {
    return FALSE;
  }

  g_autofree char *players_blob = g_match_info_fetch(players_match, 1);
  if (players_blob == NULL) {
    return FALSE;
  }

  g_autoptr(GRegex) online_re = g_regex_new("\"online\"\\s*:\\s*([0-9]+)", G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GRegex) max_re = g_regex_new("\"max\"\\s*:\\s*([0-9]+)", G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) online_match = NULL;
  g_autoptr(GMatchInfo) max_match = NULL;
  if (!g_regex_match(online_re, players_blob, 0, &online_match)) {
    return FALSE;
  }
  if (!g_regex_match(max_re, players_blob, 0, &max_match)) {
    return FALSE;
  }

  g_autofree char *online_txt = g_match_info_fetch(online_match, 1);
  g_autofree char *max_txt = g_match_info_fetch(max_match, 1);
  if (online_txt == NULL || max_txt == NULL) {
    return FALSE;
  }

  *out_players = (int)strtol(online_txt, NULL, 10);
  *out_max_players = (int)strtol(max_txt, NULL, 10);
  if (*out_players < 0) {
    *out_players = 0;
  }
  if (*out_max_players < 0) {
    *out_max_players = 0;
  }
  return TRUE;
}

gboolean
pumpkin_query_minecraft_players(const char *host, int port, int *out_players, int *out_max_players)
{
  if (host == NULL || port <= 0 || out_players == NULL || out_max_players == NULL) {
    return FALSE;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_socket_client_set_timeout(client, 1);
  g_autofree char *target = g_strdup(host);
  if (target == NULL || *target == '\0') {
    target = g_strdup("127.0.0.1");
  }

  g_autoptr(GSocketConnection) connection = g_socket_client_connect_to_host(client, target, port, NULL, &error);
  if (connection == NULL) {
    return FALSE;
  }
  GSocket *socket = g_socket_connection_get_socket(connection);
  if (socket != NULL) {
    g_socket_set_timeout(socket, 1);
  }

  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  if (output == NULL || input == NULL) {
    return FALSE;
  }

  g_autoptr(GByteArray) handshake_payload = g_byte_array_new();
  slp_varint_append(handshake_payload, 0x00);
  slp_varint_append(handshake_payload, 0);
  slp_varint_append(handshake_payload, (guint32)strlen(target));
  g_byte_array_append(handshake_payload, (const guint8 *)target, strlen(target));
  guint8 port_bytes[2] = {
    (guint8)((port >> 8) & 0xFF),
    (guint8)(port & 0xFF)
  };
  g_byte_array_append(handshake_payload, port_bytes, 2);
  slp_varint_append(handshake_payload, 1);

  g_autoptr(GByteArray) handshake_packet = g_byte_array_new();
  slp_varint_append(handshake_packet, handshake_payload->len);
  g_byte_array_append(handshake_packet, handshake_payload->data, handshake_payload->len);

  gsize written = 0;
  if (!g_output_stream_write_all(output, handshake_packet->data, handshake_packet->len, &written, NULL, NULL) ||
      written != handshake_packet->len) {
    return FALSE;
  }

  static const guint8 status_request[] = {0x01, 0x00};
  written = 0;
  if (!g_output_stream_write_all(output, status_request, sizeof(status_request), &written, NULL, NULL) ||
      written != sizeof(status_request)) {
    return FALSE;
  }
  if (!g_output_stream_flush(output, NULL, NULL)) {
    return FALSE;
  }

  guint32 packet_len = 0;
  if (!slp_read_varint_stream(input, &packet_len) || packet_len == 0 || packet_len > 32768) {
    return FALSE;
  }

  g_autofree guint8 *packet_data = g_malloc0(packet_len);
  if (!slp_read_exact(input, packet_data, packet_len)) {
    return FALSE;
  }

  gsize offset = 0;
  guint32 packet_id = 0;
  if (!slp_read_varint_buffer(packet_data, packet_len, &offset, &packet_id) || packet_id != 0x00) {
    return FALSE;
  }

  guint32 json_len = 0;
  if (!slp_read_varint_buffer(packet_data, packet_len, &offset, &json_len) || json_len == 0) {
    return FALSE;
  }

  if (offset + json_len > packet_len) {
    return FALSE;
  }

  g_autofree char *json = g_strndup((const char *)(packet_data + offset), (gsize)json_len);
  return parse_slp_players_json(json, out_players, out_max_players);
}

gboolean
pumpkin_is_player_list_snapshot_line(const char *line)
{
  if (line == NULL) {
    return FALSE;
  }
  g_autofree char *lower = g_ascii_strdown(line, -1);
  return strstr(lower, "players online") != NULL || strstr(lower, "of a max of") != NULL;
}

gboolean
pumpkin_parse_player_list_snapshot_line(const char *line, int *out_count, char **out_names_csv)
{
  if (line == NULL) {
    return FALSE;
  }
  if (out_count != NULL) {
    *out_count = -1;
  }
  if (out_names_csv != NULL) {
    *out_names_csv = NULL;
  }

  g_autoptr(GRegex) list_re =
    g_regex_new("there\\s+are\\s+([0-9]+)\\s+of\\s+a\\s+max\\s+of\\s+[0-9]+\\s+players\\s+online\\s*:?\\s*(.*)$",
                G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match(list_re, line, 0, &match)) {
    return FALSE;
  }

  g_autofree char *count_txt = g_match_info_fetch(match, 1);
  g_autofree char *names_txt = g_match_info_fetch(match, 2);
  if (count_txt != NULL && out_count != NULL) {
    *out_count = (int)strtol(count_txt, NULL, 10);
    if (*out_count < 0) {
      *out_count = 0;
    }
  }
  if (out_names_csv != NULL && names_txt != NULL) {
    g_strstrip(names_txt);
    if (names_txt[0] != '\0') {
      *out_names_csv = g_strdup(names_txt);
    }
  }
  return TRUE;
}

gboolean
pumpkin_parse_tps_from_line(const char *line, double *out)
{
  if (line == NULL || out == NULL) {
    return FALSE;
  }
  g_autofree char *clean = pumpkin_strip_ansi(line);
  const char *check = clean != NULL ? clean : line;

  static GRegex *primary = NULL;
  static GRegex *fallback = NULL;
  if (primary == NULL) {
    primary = g_regex_new("TPS\\s*:\\s*([0-9]+(\\.[0-9]+)?)", G_REGEX_CASELESS, 0, NULL);
  }
  if (fallback == NULL) {
    fallback = g_regex_new("tps[^0-9]*([0-9]+(\\.[0-9]+)?)", G_REGEX_CASELESS, 0, NULL);
  }
  if (primary == NULL || fallback == NULL) {
    return FALSE;
  }

  g_autoptr(GMatchInfo) match_info = NULL;
  if (!g_regex_match(primary, check, 0, &match_info) || !g_match_info_matches(match_info)) {
    g_clear_pointer(&match_info, g_match_info_free);
    if (!g_regex_match(fallback, check, 0, &match_info) || !g_match_info_matches(match_info)) {
      return FALSE;
    }
  }

  g_autofree char *num = g_match_info_fetch(match_info, 1);
  if (num == NULL || *num == '\0') {
    return FALSE;
  }

  char *endptr = NULL;
  double value = g_ascii_strtod(num, &endptr);
  if (endptr == num) {
    return FALSE;
  }
  *out = value;
  return TRUE;
}
