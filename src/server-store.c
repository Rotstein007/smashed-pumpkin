#include "server-store.h"
#include "app-config.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <ctype.h>

struct _PumpkinServerStore {
  GObject parent_instance;
  GListStore *model;
  PumpkinServer *selected;
  char *base_dir;
};

G_DEFINE_FINAL_TYPE(PumpkinServerStore, pumpkin_server_store, G_TYPE_OBJECT)

static void
pumpkin_server_store_finalize(GObject *object)
{
  PumpkinServerStore *self = PUMPKIN_SERVER_STORE(object);
  g_clear_object(&self->model);
  g_clear_object(&self->selected);
  g_clear_pointer(&self->base_dir, g_free);
  G_OBJECT_CLASS(pumpkin_server_store_parent_class)->finalize(object);
}

static void
pumpkin_server_store_class_init(PumpkinServerStoreClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS(class);
  object_class->finalize = pumpkin_server_store_finalize;
}

static void
pumpkin_server_store_init(PumpkinServerStore *self)
{
  self->model = g_list_store_new(PUMPKIN_TYPE_SERVER);
  g_autoptr(GError) error = NULL;
  PumpkinConfig *config = pumpkin_config_load(&error);
  if (config != NULL) {
    self->base_dir = g_strdup(pumpkin_config_get_base_dir(config));
    pumpkin_config_free(config);
  } else {
    self->base_dir = g_build_filename(g_get_home_dir(), "PumpkinServer", NULL);
  }
  g_mkdir_with_parents(self->base_dir, 0755);

  GDir *dir = g_dir_open(self->base_dir, 0, NULL);
  if (dir != NULL) {
    const char *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
      g_autofree char *server_dir = g_build_filename(self->base_dir, entry, NULL);
      g_autoptr(GError) error = NULL;
      PumpkinServer *server = pumpkin_server_load(server_dir, &error);
      if (server != NULL) {
        g_list_store_append(self->model, server);
        g_object_unref(server);
      }
    }
    g_dir_close(dir);
  }

  if (g_list_model_get_n_items(G_LIST_MODEL(self->model)) == 0) {
    g_autoptr(GError) error = NULL;
    pumpkin_server_store_add(self, "My Pumpkin Server", &error);
  }
}

PumpkinServerStore *
pumpkin_server_store_new(void)
{
  return g_object_new(PUMPKIN_TYPE_SERVER_STORE, NULL);
}

GListModel *
pumpkin_server_store_get_model(PumpkinServerStore *self)
{
  return G_LIST_MODEL(self->model);
}

PumpkinServer *
pumpkin_server_store_get_selected(PumpkinServerStore *self)
{
  return self->selected;
}

const char *
pumpkin_server_store_get_base_dir(PumpkinServerStore *self)
{
  return self->base_dir;
}

void
pumpkin_server_store_set_selected(PumpkinServerStore *self, PumpkinServer *server)
{
  if (self->selected == server) {
    return;
  }
  g_set_object(&self->selected, server);
}

static char *
sanitize_name(const char *name)
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
    g_string_assign(out, "server");
  }

  return g_string_free(out, FALSE);
}

static char *
unique_server_dir(PumpkinServerStore *self, const char *name)
{
  g_autofree char *slug = sanitize_name(name);
  g_autofree char *candidate = g_build_filename(self->base_dir, slug, NULL);
  if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) {
    return g_strdup(candidate);
  }

  for (guint i = 2; i < 1000; i++) {
    g_autofree char *with_suffix = g_strdup_printf("%s-%u", slug, i);
    g_autofree char *path = g_build_filename(self->base_dir, with_suffix, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
      return g_strdup(path);
    }
  }

  g_autofree char *uuid = g_uuid_string_random();
  return g_build_filename(self->base_dir, uuid, NULL);
}

PumpkinServer *
pumpkin_server_store_add(PumpkinServerStore *self, const char *name, GError **error)
{
  g_autofree char *dir = unique_server_dir(self, name);
  if (g_mkdir_with_parents(dir, 0755) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Failed to create server directory");
    return NULL;
  }

  g_autofree char *id = g_path_get_basename(dir);
  PumpkinServer *server = pumpkin_server_new(id, name);
  pumpkin_server_set_root_dir(server, dir);

  if (!pumpkin_server_save(server, error)) {
    g_object_unref(server);
    return NULL;
  }

  g_list_store_append(self->model, server);
  return server;
}

PumpkinServer *
pumpkin_server_store_import(PumpkinServerStore *self, const char *dir, GError **error)
{
  if (dir == NULL || *dir == '\0') {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid server directory");
    return NULL;
  }

  g_autofree char *ini = g_build_filename(dir, "server.ini", NULL);
  PumpkinServer *server = NULL;

  if (g_file_test(ini, G_FILE_TEST_EXISTS)) {
    server = pumpkin_server_load(dir, error);
    if (server == NULL) {
      return NULL;
    }
  } else {
    g_autofree char *id = g_path_get_basename(dir);
    server = pumpkin_server_new(id, id);
    pumpkin_server_set_root_dir(server, dir);
    if (!pumpkin_server_save(server, error)) {
      g_object_unref(server);
      return NULL;
    }
  }

  g_list_store_append(self->model, server);
  return server;
}

static gboolean
remove_tree(const char *path, GError **error)
{
  GDir *dir = g_dir_open(path, 0, error);
  if (dir == NULL) {
    return FALSE;
  }

  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child = g_build_filename(path, entry, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
      if (!remove_tree(child, error)) {
        g_dir_close(dir);
        return FALSE;
      }
    } else {
      if (g_unlink(child) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to remove %s", child);
        g_dir_close(dir);
        return FALSE;
      }
    }
  }
  g_dir_close(dir);

  if (g_rmdir(path) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to remove %s", path);
    return FALSE;
  }

  return TRUE;
}

gboolean
pumpkin_server_store_remove_tree(const char *path, GError **error)
{
  return remove_tree(path, error);
}

void
pumpkin_server_store_remove_selected(PumpkinServerStore *self)
{
  if (self->selected == NULL) {
    return;
  }

  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->model));
  for (guint i = 0; i < n; i++) {
    PumpkinServer *server = g_list_model_get_item(G_LIST_MODEL(self->model), i);
    if (server == self->selected) {
      const char *root = pumpkin_server_get_root_dir(server);
      if (root != NULL) {
        g_autoptr(GError) rm_error = NULL;
        if (!remove_tree(root, &rm_error)) {
          g_clear_error(&rm_error);
        }
      }
      g_list_store_remove(self->model, i);
      g_object_unref(server);
      break;
    }
    g_object_unref(server);
  }

  g_clear_object(&self->selected);
}
