#pragma once

#include "server.h"

G_BEGIN_DECLS

#define PUMPKIN_TYPE_SERVER_STORE (pumpkin_server_store_get_type())
G_DECLARE_FINAL_TYPE(PumpkinServerStore, pumpkin_server_store, PUMPKIN, SERVER_STORE, GObject)

PumpkinServerStore *pumpkin_server_store_new(void);

GListModel *pumpkin_server_store_get_model(PumpkinServerStore *self);
PumpkinServer *pumpkin_server_store_get_selected(PumpkinServerStore *self);
void pumpkin_server_store_set_selected(PumpkinServerStore *self, PumpkinServer *server);
const char *pumpkin_server_store_get_base_dir(PumpkinServerStore *self);

PumpkinServer *pumpkin_server_store_add(PumpkinServerStore *self, const char *name, GError **error);
PumpkinServer *pumpkin_server_store_import(PumpkinServerStore *self, const char *dir, GError **error);
void pumpkin_server_store_remove_selected(PumpkinServerStore *self);
gboolean pumpkin_server_store_remove_tree(const char *path, GError **error);

G_END_DECLS
