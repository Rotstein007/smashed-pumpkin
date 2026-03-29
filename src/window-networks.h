#pragma once

#include "window-internal.h"

char *sanitize_network_id(const char *name);
ServerNetwork *server_network_new(const char *id, const char *name);
void server_network_free(ServerNetwork *network);
char *network_config_path(PumpkinWindow *self);
ServerNetwork *find_network_by_id(PumpkinWindow *self, const char *id);
gboolean network_has_member(ServerNetwork *network, const char *server_id);
gboolean network_includes_server(ServerNetwork *network, PumpkinServer *server);
gboolean server_in_any_network(PumpkinWindow *self, const char *server_id);
void network_add_member(ServerNetwork *network, const char *server_id);
void network_remove_member(ServerNetwork *network, const char *server_id);
PumpkinServer *find_server_by_id(PumpkinWindow *self, const char *server_id);
void get_next_standalone_server_ports(PumpkinWindow *self, int *out_java_port, int *out_bedrock_port);
gboolean apply_network_auto_ports(PumpkinWindow *self, ServerNetwork *network);
void networks_load(PumpkinWindow *self);
gboolean networks_save(PumpkinWindow *self);
gboolean networks_prune_server(PumpkinWindow *self, const char *server_id);
