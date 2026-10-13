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
    self->base_dir = g_build_filename(g_get_user_data_dir(), "smashed-pumpkin", "servers", NULL);
  }
  if (g_mkdir_with_parents(self->base_dir, 0755) != 0 && errno != EEXIST) {
    g_warning("Cannot create server directory %s: %s", self->base_dir, g_strerror(errno));
  }

  GDir *dir = g_dir_open(self->base_dir, 0, NULL);
  if (dir != NULL) {
    const char *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
      g_autofree char *server_dir = g_build_filename(self->base_dir, entry, NULL);
      g_autoptr(GError) load_error = NULL;
      PumpkinServer *server = pumpkin_server_load(server_dir, &load_error);
      if (server != NULL) {
        g_list_store_append(self->model, server);
        g_object_unref(server);
      }
    }
    g_dir_close(dir);
  }

  if (g_list_model_get_n_items(G_LIST_MODEL(self->model)) == 0) {
    g_autoptr(GError) add_error = NULL;
    g_autoptr(PumpkinServer) server = pumpkin_server_store_add(self, "My Pumpkin Server", &add_error);
    if (add_error != NULL)
      g_warning("Cannot create the default server: %s", add_error->message);
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
remove_file_tree_typed(GFile *file, GFileType type, GError **error)
{
  /* Linked worlds and other symlinks belong to the tree as links, not as targets. */
  if (type == G_FILE_TYPE_DIRECTORY) {
    g_autoptr(GFileEnumerator) children = g_file_enumerate_children(file,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (children == NULL) {
      return FALSE;
    }
    while (TRUE) {
      g_autoptr(GError) next_error = NULL;
      g_autoptr(GFileInfo) child_info = g_file_enumerator_next_file(children, NULL, &next_error);
      if (child_info == NULL) {
        if (next_error != NULL) {
          g_propagate_error(error, g_steal_pointer(&next_error));
          return FALSE;
        }
        break;
      }
      g_autoptr(GFile) child = g_file_enumerator_get_child(children, child_info);
      if (!remove_file_tree_typed(child, g_file_info_get_file_type(child_info), error)) {
        return FALSE;
      }
    }
    if (!g_file_enumerator_close(children, NULL, error)) {
      return FALSE;
    }
  }

  return g_file_delete(file, NULL, error);
}

static gboolean
remove_file_tree(GFile *file, GError **error)
{
  g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (info == NULL) {
    return FALSE;
  }
  return remove_file_tree_typed(file, g_file_info_get_file_type(info), error);
}

gboolean
pumpkin_server_store_remove_tree(const char *path, GError **error)
{
  if (path == NULL || *path == '\0') {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid server directory");
    return FALSE;
  }
  g_autoptr(GFile) file = g_file_new_for_path(path);
  return remove_file_tree(file, error);
}

gboolean
pumpkin_server_store_remove_selected(PumpkinServerStore *self, GError **error)
{
  if (self->selected == NULL) {
    return TRUE;
  }

  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->model));
  for (guint i = 0; i < n; i++) {
    g_autoptr(PumpkinServer) server = g_list_model_get_item(G_LIST_MODEL(self->model), i);
    if (server == self->selected) {
      const char *root = pumpkin_server_get_root_dir(server);
      if (root != NULL && *root != '\0') {
        g_autoptr(GError) rm_error = NULL;
        if (!pumpkin_server_store_remove_tree(root, &rm_error)) {
          /* An already vanished directory must not block removing the list
           * entry: retrying would report NOT_FOUND forever and leave a dead
           * row that can neither run nor be removed. */
          if (!g_error_matches(rm_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
            g_propagate_error(error, g_steal_pointer(&rm_error));
            return FALSE;
          }
        }
      }
      g_list_store_remove(self->model, i);
      g_clear_object(&self->selected);
      return TRUE;
    }
  }

  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "The selected server is no longer in the server list");
  return FALSE;
}
