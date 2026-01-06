#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PUMPKIN_TYPE_SERVER (pumpkin_server_get_type())
G_DECLARE_FINAL_TYPE(PumpkinServer, pumpkin_server, PUMPKIN, SERVER, GObject)

PumpkinServer *pumpkin_server_new(const char *id, const char *name);
PumpkinServer *pumpkin_server_load(const char *dir, GError **error);

gboolean pumpkin_server_save(PumpkinServer *self, GError **error);

gboolean pumpkin_server_start(PumpkinServer *self, GError **error);
void pumpkin_server_stop(PumpkinServer *self);
gboolean pumpkin_server_send_command(PumpkinServer *self, const char *command, GError **error);

const char *pumpkin_server_get_id(PumpkinServer *self);
const char *pumpkin_server_get_name(PumpkinServer *self);
const char *pumpkin_server_get_root_dir(PumpkinServer *self);
const char *pumpkin_server_get_download_url(PumpkinServer *self);
const char *pumpkin_server_get_installed_url(PumpkinServer *self);
gboolean pumpkin_server_get_auto_restart(PumpkinServer *self);
int pumpkin_server_get_auto_restart_delay(PumpkinServer *self);
const char *pumpkin_server_get_rcon_host(PumpkinServer *self);
const char *pumpkin_server_get_rcon_password(PumpkinServer *self);
int pumpkin_server_get_rcon_port(PumpkinServer *self);
int pumpkin_server_get_port(PumpkinServer *self);
int pumpkin_server_get_bedrock_port(PumpkinServer *self);
int pumpkin_server_get_max_players(PumpkinServer *self);
int pumpkin_server_get_max_cpu_cores(PumpkinServer *self);
int pumpkin_server_get_max_ram_mb(PumpkinServer *self);

gboolean pumpkin_server_get_running(PumpkinServer *self);

void pumpkin_server_set_name(PumpkinServer *self, const char *name);
void pumpkin_server_set_download_url(PumpkinServer *self, const char *url);
void pumpkin_server_set_installed_url(PumpkinServer *self, const char *url);
void pumpkin_server_set_auto_restart(PumpkinServer *self, gboolean enabled);
void pumpkin_server_set_auto_restart_delay(PumpkinServer *self, int seconds);
void pumpkin_server_set_rcon_host(PumpkinServer *self, const char *host);
void pumpkin_server_set_rcon_port(PumpkinServer *self, int port);
void pumpkin_server_set_rcon_password(PumpkinServer *self, const char *password);
void pumpkin_server_set_port(PumpkinServer *self, int port);
void pumpkin_server_set_bedrock_port(PumpkinServer *self, int port);
void pumpkin_server_set_max_players(PumpkinServer *self, int max_players);
void pumpkin_server_set_max_cpu_cores(PumpkinServer *self, int max_cpu_cores);
void pumpkin_server_set_max_ram_mb(PumpkinServer *self, int max_ram_mb);
void pumpkin_server_set_root_dir(PumpkinServer *self, const char *dir);

char *pumpkin_server_get_bin_path(PumpkinServer *self);
char *pumpkin_server_get_data_dir(PumpkinServer *self);
char *pumpkin_server_get_plugins_dir(PumpkinServer *self);
char *pumpkin_server_get_worlds_dir(PumpkinServer *self);
char *pumpkin_server_get_players_dir(PumpkinServer *self);
char *pumpkin_server_get_logs_dir(PumpkinServer *self);
int pumpkin_server_get_pid(PumpkinServer *self);

G_END_DECLS
