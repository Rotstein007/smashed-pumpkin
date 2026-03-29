#include "window-networks.h"

ServerNetwork *
server_network_new(const char *id, const char *name)
{
  ServerNetwork *network = g_new0(ServerNetwork, 1);
  network->id = g_strdup(id != NULL ? id : "");
  network->name = g_strdup((name != NULL && *name != '\0') ? name : network->id);
  network->member_server_ids = g_ptr_array_new_with_free_func(g_free);
  return network;
}

void
server_network_free(ServerNetwork *network)
{
  if (network == NULL) {
    return;
  }
  g_clear_pointer(&network->id, g_free);
  g_clear_pointer(&network->name, g_free);
  g_clear_pointer(&network->icon_name, g_free);
  g_clear_pointer(&network->proxy_server_id, g_free);
  if (network->member_server_ids != NULL) {
    g_ptr_array_unref(network->member_server_ids);
    network->member_server_ids = NULL;
  }
  g_free(network);
}

char *
sanitize_network_id(const char *name)
{
  GString *out = g_string_new(NULL);
  gboolean last_dash = FALSE;

  for (const char *p = name; p != NULL && *p != '\0'; p++) {
    gunichar ch = g_utf8_get_char(p);
    if (g_unichar_isalnum(ch)) {
      g_string_append_unichar(out, g_unichar_tolower(ch));
      last_dash = FALSE;
    } else if (!last_dash) {
      g_string_append_c(out, '-');
      last_dash = TRUE;
    }
    if ((guchar)*p >= 0x80) {
      p = g_utf8_next_char(p) - 1;
    }
  }

  while (out->len > 0 && out->str[out->len - 1] == '-') {
    g_string_truncate(out, out->len - 1);
  }
  if (out->len == 0) {
    g_string_assign(out, "network");
  }
  return g_string_free(out, FALSE);
}

char *
network_config_path(PumpkinWindow *self)
{
  if (self == NULL || self->store == NULL) {
    return NULL;
  }
  const char *base = pumpkin_server_store_get_base_dir(self->store);
  if (base == NULL || *base == '\0') {
    return NULL;
  }
  return g_build_filename(base, "networks.ini", NULL);
}

ServerNetwork *
find_network_by_id(PumpkinWindow *self, const char *id)
{
  if (self == NULL || self->networks == NULL || id == NULL || *id == '\0') {
    return NULL;
  }
  for (guint i = 0; i < self->networks->len; i++) {
    ServerNetwork *network = g_ptr_array_index(self->networks, i);
    if (network != NULL && g_strcmp0(network->id, id) == 0) {
      return network;
    }
  }
  return NULL;
}

gboolean
network_has_member(ServerNetwork *network, const char *server_id)
{
  if (network == NULL || network->member_server_ids == NULL || server_id == NULL || *server_id == '\0') {
    return FALSE;
  }
  for (guint i = 0; i < network->member_server_ids->len; i++) {
    const char *member = g_ptr_array_index(network->member_server_ids, i);
    if (g_strcmp0(member, server_id) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
network_member_token_matches_server(const char *token, PumpkinServer *server)
{
  if (token == NULL || *token == '\0' || server == NULL) {
    return FALSE;
  }
  const char *server_id = pumpkin_server_get_id(server);
  const char *server_name = pumpkin_server_get_name(server);
  if ((server_id != NULL && g_strcmp0(token, server_id) == 0) ||
      (server_name != NULL && g_strcmp0(token, server_name) == 0)) {
    return TRUE;
  }

  g_autofree char *token_key = normalized_key(token);
  if (token_key == NULL || *token_key == '\0') {
    return FALSE;
  }
  g_autofree char *id_key = normalized_key(server_id);
  g_autofree char *name_key = normalized_key(server_name);
  return (id_key != NULL && g_strcmp0(token_key, id_key) == 0) ||
         (name_key != NULL && g_strcmp0(token_key, name_key) == 0);
}

gboolean
network_includes_server(ServerNetwork *network, PumpkinServer *server)
{
  if (network == NULL || network->member_server_ids == NULL || server == NULL) {
    return FALSE;
  }
  for (guint i = 0; i < network->member_server_ids->len; i++) {
    const char *member = g_ptr_array_index(network->member_server_ids, i);
    if (network_member_token_matches_server(member, server)) {
      return TRUE;
    }
  }
  return FALSE;
}

gboolean
server_in_any_network(PumpkinWindow *self, const char *server_id)
{
  if (self == NULL || self->networks == NULL || server_id == NULL || *server_id == '\0') {
    return FALSE;
  }
  for (guint i = 0; i < self->networks->len; i++) {
    ServerNetwork *network = g_ptr_array_index(self->networks, i);
    if (network != NULL && network_has_member(network, server_id)) {
      return TRUE;
    }
  }
  return FALSE;
}

void
network_add_member(ServerNetwork *network, const char *server_id)
{
  if (network == NULL || network->member_server_ids == NULL || server_id == NULL || *server_id == '\0') {
    return;
  }
  if (!network_has_member(network, server_id)) {
    g_ptr_array_add(network->member_server_ids, g_strdup(server_id));
  }
}

void
network_remove_member(ServerNetwork *network, const char *server_id)
{
  if (network == NULL || network->member_server_ids == NULL || server_id == NULL || *server_id == '\0') {
    return;
  }
  for (guint i = 0; i < network->member_server_ids->len; i++) {
    const char *member = g_ptr_array_index(network->member_server_ids, i);
    if (g_strcmp0(member, server_id) == 0) {
      g_ptr_array_remove_index(network->member_server_ids, i);
      return;
    }
  }
}

PumpkinServer *
find_server_by_id(PumpkinWindow *self, const char *server_id)
{
  if (self == NULL || self->store == NULL || server_id == NULL || *server_id == '\0') {
    return NULL;
  }
  g_autofree char *target_key = normalized_key(server_id);
  GListModel *model = get_server_model(self);
  if (model == NULL) {
    return NULL;
  }
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(model, i);
    if (server == NULL) {
      continue;
    }
    const char *candidate_id = pumpkin_server_get_id(server);
    const char *candidate_name = pumpkin_server_get_name(server);
    gboolean match =
      (candidate_id != NULL && g_strcmp0(candidate_id, server_id) == 0) ||
      (candidate_name != NULL && g_strcmp0(candidate_name, server_id) == 0);
    if (!match && target_key != NULL) {
      g_autofree char *id_key = normalized_key(candidate_id);
      g_autofree char *name_key = normalized_key(candidate_name);
      if ((id_key != NULL && g_strcmp0(id_key, target_key) == 0) ||
          (name_key != NULL && g_strcmp0(name_key, target_key) == 0)) {
        match = TRUE;
      }
    }
    if (match) {
      return server;
    }
    g_object_unref(server);
  }
  return NULL;
}

static void
reserve_port_if_valid(GHashTable *set, int port)
{
  if (set == NULL || port <= 0 || port > 65535) {
    return;
  }
  g_hash_table_add(set, GINT_TO_POINTER(port));
}

static int
reserve_next_port(GHashTable *used, int start)
{
  int port = start < 1 ? 1 : start;
  for (; port <= 65535; port++) {
    gpointer key = GINT_TO_POINTER(port);
    if (!g_hash_table_contains(used, key)) {
      g_hash_table_add(used, key);
      return port;
    }
  }
  return CLAMP(start, 1, 65535);
}

void
get_next_standalone_server_ports(PumpkinWindow *self, int *out_java_port, int *out_bedrock_port)
{
  int java_port = NETWORK_PROXY_JAVA_PORT;
  int bedrock_port = NETWORK_PROXY_BEDROCK_PORT;

  GListModel *model = get_server_model(self);
  if (model != NULL) {
    g_autoptr(GHashTable) used_java = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_autoptr(GHashTable) used_bedrock = g_hash_table_new(g_direct_hash, g_direct_equal);

    guint n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; i++) {
      g_autoptr(PumpkinServer) server = g_list_model_get_item(model, i);
      if (server == NULL) {
        continue;
      }
      reserve_port_if_valid(used_java, pumpkin_server_get_port(server));
      reserve_port_if_valid(used_bedrock, pumpkin_server_get_bedrock_port(server));
    }

    java_port = reserve_next_port(used_java, NETWORK_PROXY_JAVA_PORT);
    bedrock_port = reserve_next_port(used_bedrock, NETWORK_PROXY_BEDROCK_PORT);
  }

  if (out_java_port != NULL) {
    *out_java_port = java_port;
  }
  if (out_bedrock_port != NULL) {
    *out_bedrock_port = bedrock_port;
  }
}

gboolean
apply_network_auto_ports(PumpkinWindow *self, ServerNetwork *network)
{
  if (self == NULL || network == NULL || network->member_server_ids == NULL || network->member_server_ids->len == 0) {
    return FALSE;
  }

  GListModel *model = get_server_model(self);
  if (model == NULL) {
    return FALSE;
  }

  g_autoptr(GHashTable) used_java = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_autoptr(GHashTable) used_bedrock = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_autoptr(GHashTable) used_rcon = g_hash_table_new(g_direct_hash, g_direct_equal);

  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    g_autoptr(PumpkinServer) server = g_list_model_get_item(model, i);
    if (server == NULL) {
      continue;
    }
    const char *server_id = pumpkin_server_get_id(server);
    if (server_id == NULL || *server_id == '\0' || network_has_member(network, server_id)) {
      continue;
    }
    reserve_port_if_valid(used_java, pumpkin_server_get_port(server));
    reserve_port_if_valid(used_bedrock, pumpkin_server_get_bedrock_port(server));
    reserve_port_if_valid(used_rcon, pumpkin_server_get_rcon_port(server));
  }

  gboolean changed_any = FALSE;
  gboolean current_changed = FALSE;
  gboolean has_explicit_proxy = network->proxy_server_id != NULL && *network->proxy_server_id != '\0';
  gboolean implicit_primary_assigned = FALSE;
  int next_java = NETWORK_PROXY_JAVA_PORT + 1;
  int next_bedrock = NETWORK_PROXY_BEDROCK_PORT + 1;
  int next_rcon = NETWORK_PROXY_RCON_PORT + 1;

  for (guint i = 0; i < network->member_server_ids->len; i++) {
    const char *member_id = g_ptr_array_index(network->member_server_ids, i);
    if (member_id == NULL || *member_id == '\0') {
      continue;
    }

    g_autoptr(PumpkinServer) server = find_server_by_id(self, member_id);
    if (server == NULL) {
      continue;
    }

    gboolean is_primary = FALSE;
    if (has_explicit_proxy) {
      is_primary = g_strcmp0(member_id, network->proxy_server_id) == 0;
    } else if (!implicit_primary_assigned) {
      is_primary = TRUE;
      implicit_primary_assigned = TRUE;
    }

    int desired_java;
    int desired_bedrock;
    int desired_rcon;
    if (is_primary) {
      desired_java = NETWORK_PROXY_JAVA_PORT;
      desired_bedrock = NETWORK_PROXY_BEDROCK_PORT;
      desired_rcon = NETWORK_PROXY_RCON_PORT;
      reserve_port_if_valid(used_java, desired_java);
      reserve_port_if_valid(used_bedrock, desired_bedrock);
      reserve_port_if_valid(used_rcon, desired_rcon);
    } else {
      desired_java = reserve_next_port(used_java, next_java);
      desired_bedrock = reserve_next_port(used_bedrock, next_bedrock);
      desired_rcon = reserve_next_port(used_rcon, next_rcon);
    }
    next_java = desired_java + 1;
    next_bedrock = desired_bedrock + 1;
    next_rcon = desired_rcon + 1;

    gboolean server_changed = FALSE;
    if (pumpkin_server_get_port(server) != desired_java) {
      pumpkin_server_set_port(server, desired_java);
      server_changed = TRUE;
    }
    if (pumpkin_server_get_bedrock_port(server) != desired_bedrock) {
      pumpkin_server_set_bedrock_port(server, desired_bedrock);
      server_changed = TRUE;
    }
    if (pumpkin_server_get_rcon_port(server) != desired_rcon) {
      pumpkin_server_set_rcon_port(server, desired_rcon);
      server_changed = TRUE;
    }

    if (server_changed) {
      pumpkin_server_save(server, NULL);
      changed_any = TRUE;
      if (self->current == server) {
        current_changed = TRUE;
      }
    }
  }

  if (current_changed) {
    update_settings_form(self);
  }

  return changed_any;
}

void
networks_load(PumpkinWindow *self)
{
  if (self == NULL) {
    return;
  }
  if (self->networks == NULL) {
    self->networks = g_ptr_array_new_with_free_func((GDestroyNotify)server_network_free);
  } else {
    g_ptr_array_set_size(self->networks, 0);
  }

  g_autofree char *path = network_config_path(self);
  if (path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS)) {
    return;
  }

  g_autoptr(GKeyFile) key = g_key_file_new();
  if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL)) {
    return;
  }

  gsize groups_len = 0;
  g_auto(GStrv) groups = g_key_file_get_groups(key, &groups_len);
  for (gsize i = 0; i < groups_len; i++) {
    const char *group = groups[i];
    if (group == NULL || !g_str_has_prefix(group, "network:")) {
      continue;
    }

    const char *id = group + strlen("network:");
    if (id == NULL || *id == '\0') {
      continue;
    }
    g_autofree char *name = g_key_file_get_string(key, group, "name", NULL);
    ServerNetwork *network = server_network_new(id, name);

    g_autofree char *proxy = g_key_file_get_string(key, group, "proxy_server_id", NULL);
    if (proxy != NULL && *proxy != '\0') {
      network->proxy_server_id = g_strdup(proxy);
    }

    g_autofree char *members = g_key_file_get_string(key, group, "members", NULL);
    if (members != NULL && *members != '\0') {
      g_auto(GStrv) split = g_strsplit(members, ",", -1);
      for (guint j = 0; split[j] != NULL; j++) {
        g_autofree char *id_txt = g_strdup(split[j]);
        g_strstrip(id_txt);
        if (id_txt[0] != '\0') {
          network_add_member(network, id_txt);
        }
      }
    }
    if (network->proxy_server_id != NULL && *network->proxy_server_id != '\0') {
      network_add_member(network, network->proxy_server_id);
    }
    g_ptr_array_add(self->networks, network);
  }
}

gboolean
networks_save(PumpkinWindow *self)
{
  if (self == NULL) {
    return FALSE;
  }

  g_autofree char *path = network_config_path(self);
  if (path == NULL) {
    return FALSE;
  }

  g_autoptr(GKeyFile) key = g_key_file_new();
  if (self->networks != NULL) {
    for (guint i = 0; i < self->networks->len; i++) {
      ServerNetwork *network = g_ptr_array_index(self->networks, i);
      if (network == NULL || network->id == NULL || *network->id == '\0') {
        continue;
      }
      g_autofree char *group = g_strdup_printf("network:%s", network->id);
      g_key_file_set_string(key, group, "name", network->name != NULL ? network->name : network->id);
      g_key_file_set_string(key, group, "proxy_server_id",
                            network->proxy_server_id != NULL ? network->proxy_server_id : "");

      g_autoptr(GString) members = g_string_new(NULL);
      for (guint j = 0; j < network->member_server_ids->len; j++) {
        const char *member = g_ptr_array_index(network->member_server_ids, j);
        if (member == NULL || *member == '\0') {
          continue;
        }
        if (members->len > 0) {
          g_string_append_c(members, ',');
        }
        g_string_append(members, member);
      }
      g_key_file_set_string(key, group, "members", members->str);
    }
  }

  gsize len = 0;
  g_autofree char *data = g_key_file_to_data(key, &len, NULL);
  return g_file_set_contents(path, data, (gssize)len, NULL);
}

gboolean
networks_prune_server(PumpkinWindow *self, const char *server_id)
{
  if (self == NULL || self->networks == NULL || server_id == NULL || *server_id == '\0') {
    return FALSE;
  }
  gboolean changed = FALSE;
  for (gint i = (gint)self->networks->len - 1; i >= 0; i--) {
    ServerNetwork *network = g_ptr_array_index(self->networks, (guint)i);
    if (network == NULL) {
      continue;
    }
    guint before = network->member_server_ids->len;
    network_remove_member(network, server_id);
    if (before != network->member_server_ids->len) {
      changed = TRUE;
    }
    if (g_strcmp0(network->proxy_server_id, server_id) == 0) {
      g_clear_pointer(&network->proxy_server_id, g_free);
      changed = TRUE;
    }
    if (network->member_server_ids->len == 0) {
      g_ptr_array_remove_index(self->networks, (guint)i);
      changed = TRUE;
    }
  }
  if (changed) {
    networks_save(self);
  }
  return changed;
}
