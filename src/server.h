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
const char *pumpkin_server_get_installed_build_id(PumpkinServer *self);
const char *pumpkin_server_get_installed_build_label(PumpkinServer *self);
gboolean pumpkin_server_get_auto_restart(PumpkinServer *self);
int pumpkin_server_get_auto_restart_delay(PumpkinServer *self);
gboolean pumpkin_server_get_auto_update_enabled(PumpkinServer *self);
gboolean pumpkin_server_get_auto_update_use_schedule(PumpkinServer *self);
int pumpkin_server_get_auto_update_hour(PumpkinServer *self);
int pumpkin_server_get_auto_update_minute(PumpkinServer *self);
const char *pumpkin_server_get_rcon_host(PumpkinServer *self);
const char *pumpkin_server_get_rcon_password(PumpkinServer *self);
int pumpkin_server_get_rcon_port(PumpkinServer *self);
const char *pumpkin_server_get_domain(PumpkinServer *self);
gboolean pumpkin_server_get_ddns_enabled(PumpkinServer *self);
const char *pumpkin_server_get_ddns_provider(PumpkinServer *self);
const char *pumpkin_server_get_ddns_cf_api_token(PumpkinServer *self);
const char *pumpkin_server_get_ddns_cf_zone_id(PumpkinServer *self);
const char *pumpkin_server_get_ddns_cf_record_id(PumpkinServer *self);
gboolean pumpkin_server_get_ddns_cf_proxied(PumpkinServer *self);
gboolean pumpkin_server_get_ddns_update_ipv6(PumpkinServer *self);
int pumpkin_server_get_ddns_interval_seconds(PumpkinServer *self);
int pumpkin_server_get_port(PumpkinServer *self);
int pumpkin_server_get_bedrock_port(PumpkinServer *self);
int pumpkin_server_get_max_players(PumpkinServer *self);
int pumpkin_server_get_max_cpu_cores(PumpkinServer *self);
int pumpkin_server_get_max_ram_mb(PumpkinServer *self);
int pumpkin_server_get_stats_sample_msec(PumpkinServer *self);
gboolean pumpkin_server_get_auto_start_on_launch(PumpkinServer *self);
int pumpkin_server_get_auto_start_delay(PumpkinServer *self);

gboolean pumpkin_server_get_running(PumpkinServer *self);

void pumpkin_server_set_name(PumpkinServer *self, const char *name);
void pumpkin_server_set_download_url(PumpkinServer *self, const char *url);
void pumpkin_server_set_installed_url(PumpkinServer *self, const char *url);
void pumpkin_server_set_installed_build_id(PumpkinServer *self, const char *build_id);
void pumpkin_server_set_installed_build_label(PumpkinServer *self, const char *build_label);
void pumpkin_server_set_auto_restart(PumpkinServer *self, gboolean enabled);
void pumpkin_server_set_auto_restart_delay(PumpkinServer *self, int seconds);
void pumpkin_server_set_auto_update_enabled(PumpkinServer *self, gboolean enabled);
void pumpkin_server_set_auto_update_use_schedule(PumpkinServer *self, gboolean enabled);
void pumpkin_server_set_auto_update_hour(PumpkinServer *self, int hour);
void pumpkin_server_set_auto_update_minute(PumpkinServer *self, int minute);
void pumpkin_server_set_rcon_host(PumpkinServer *self, const char *host);
void pumpkin_server_set_rcon_port(PumpkinServer *self, int port);
void pumpkin_server_set_rcon_password(PumpkinServer *self, const char *password);
void pumpkin_server_set_domain(PumpkinServer *self, const char *domain);
void pumpkin_server_set_ddns_enabled(PumpkinServer *self, gboolean enabled);
void pumpkin_server_set_ddns_provider(PumpkinServer *self, const char *provider);
void pumpkin_server_set_ddns_cf_api_token(PumpkinServer *self, const char *token);
void pumpkin_server_set_ddns_cf_zone_id(PumpkinServer *self, const char *zone_id);
void pumpkin_server_set_ddns_cf_record_id(PumpkinServer *self, const char *record_id);
void pumpkin_server_set_ddns_cf_proxied(PumpkinServer *self, gboolean proxied);
void pumpkin_server_set_ddns_update_ipv6(PumpkinServer *self, gboolean enabled);
void pumpkin_server_set_ddns_interval_seconds(PumpkinServer *self, int seconds);
void pumpkin_server_set_port(PumpkinServer *self, int port);
void pumpkin_server_set_bedrock_port(PumpkinServer *self, int port);
void pumpkin_server_set_max_players(PumpkinServer *self, int max_players);
void pumpkin_server_set_max_cpu_cores(PumpkinServer *self, int max_cpu_cores);
void pumpkin_server_set_max_ram_mb(PumpkinServer *self, int max_ram_mb);
void pumpkin_server_set_stats_sample_msec(PumpkinServer *self, int msec);
void pumpkin_server_set_auto_start_on_launch(PumpkinServer *self, gboolean enabled);
void pumpkin_server_set_auto_start_delay(PumpkinServer *self, int seconds);
void pumpkin_server_set_root_dir(PumpkinServer *self, const char *dir);

char *pumpkin_server_get_bin_path(PumpkinServer *self);
char *pumpkin_server_get_data_dir(PumpkinServer *self);
char *pumpkin_server_get_plugins_dir(PumpkinServer *self);
char *pumpkin_server_get_worlds_dir(PumpkinServer *self);
char *pumpkin_server_get_players_dir(PumpkinServer *self);
char *pumpkin_server_get_logs_dir(PumpkinServer *self);
int pumpkin_server_get_pid(PumpkinServer *self);

G_END_DECLS
