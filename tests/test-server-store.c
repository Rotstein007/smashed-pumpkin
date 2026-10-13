#include "server-store.h"

#include <glib/gstdio.h>

static char *profile_dir;

typedef struct {
  char *directory;
  char *root;
  char *external;
  char *external_file;
} Fixture;

static void
fixture_setup(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autoptr(GError) error = NULL;
  fixture->directory = g_dir_make_tmp("smashed-server-store-XXXXXX", &error);
  g_assert_no_error(error);
  fixture->root = g_build_filename(fixture->directory, "server", NULL);
  fixture->external = g_build_filename(fixture->directory, "external-world", NULL);
  fixture->external_file = g_build_filename(fixture->external, "level.dat", NULL);
  g_assert_cmpint(g_mkdir(fixture->root, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(fixture->external, 0700), ==, 0);
  g_assert_true(g_file_set_contents(fixture->external_file, "external world data", -1, &error));
  g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_store_remove_tree(fixture->directory, &error));
  g_assert_no_error(error);
  g_free(fixture->directory);
  g_free(fixture->root);
  g_free(fixture->external);
  g_free(fixture->external_file);
}

static void
assert_external_preserved(Fixture *fixture)
{
  g_autofree char *contents = NULL;
  g_assert_true(g_file_get_contents(fixture->external_file, &contents, NULL, NULL));
  g_assert_cmpstr(contents, ==, "external world data");
}

static void
make_link(const char *path, const char *target)
{
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GError) error = NULL;
  g_assert_true(g_file_make_symbolic_link(file, target, NULL, &error));
  g_assert_no_error(error);
}

static void
test_normal_tree(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autofree char *nested = g_build_filename(fixture->root, "world", "region", NULL);
  g_autofree char *data = g_build_filename(nested, "r.0.0.mca", NULL);
  g_assert_cmpint(g_mkdir_with_parents(nested, 0700), ==, 0);
  g_assert_true(g_file_set_contents(data, "region data", -1, NULL));
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_store_remove_tree(fixture->root, &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(fixture->root, G_FILE_TEST_EXISTS));
  assert_external_preserved(fixture);
}

static void
test_child_links(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autofree char *nested = g_build_filename(fixture->root, "nested", NULL);
  g_assert_cmpint(g_mkdir(nested, 0700), ==, 0);
  g_autofree char *world_link = g_build_filename(nested, "world", NULL);
  g_autofree char *file_link = g_build_filename(fixture->root, "external-file", NULL);
  g_autofree char *missing_link = g_build_filename(fixture->root, "missing", NULL);
  g_autofree char *cycle_link = g_build_filename(fixture->root, "cycle", NULL);
  make_link(world_link, fixture->external);
  make_link(file_link, fixture->external_file);
  make_link(missing_link, "nonexistent-target");
  make_link(cycle_link, fixture->root);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_store_remove_tree(fixture->root, &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(fixture->root, G_FILE_TEST_EXISTS));
  assert_external_preserved(fixture);
}

static void
test_root_link(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autofree char *link = g_build_filename(fixture->directory, "server-link", NULL);
  make_link(link, fixture->external);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_store_remove_tree(link, &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
  assert_external_preserved(fixture);
}

static void
test_invalid_path(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autoptr(GError) error = NULL;
  g_assert_false(pumpkin_server_store_remove_tree(NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_assert_false(pumpkin_server_store_remove_tree("", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_autofree char *missing = g_build_filename(fixture->directory, "missing", NULL);
  g_assert_false(pumpkin_server_store_remove_tree(missing, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  assert_external_preserved(fixture);
}

static void
test_remove_selected_already_gone(gconstpointer user_data)
{
  g_autoptr(PumpkinServerStore) store = pumpkin_server_store_new();
  GListModel *model = pumpkin_server_store_get_model(store);
  guint count = g_list_model_get_n_items(model);
  g_assert_cmpuint(count, >, 0);
  g_autoptr(PumpkinServer) server = g_list_model_get_item(model, 0);
  pumpkin_server_store_set_selected(store, server);
  g_autofree char *root = g_strdup(pumpkin_server_get_root_dir(server));
  g_autofree char *missing = g_build_filename(profile_dir, "missing-server", NULL);
  pumpkin_server_set_root_dir(server, missing);
  g_autoptr(GError) error = NULL;
  /* Setting the root creates the server directories; remove this private fixture first. */
  g_assert_true(pumpkin_server_store_remove_tree(missing, &error));
  g_assert_no_error(error);
  /* An already vanished directory must not leave a dead row that could never
   * be removed again: the entry is dropped without an error. */
  g_assert_true(pumpkin_server_store_remove_selected(store,
    GPOINTER_TO_INT(user_data) ? &error : NULL));
  g_assert_no_error(error);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, count - 1);
  g_assert_null(pumpkin_server_store_get_selected(store));
  pumpkin_server_set_root_dir(server, root);
}

static void
test_remove_selected_success(void)
{
  g_autoptr(PumpkinServerStore) store = pumpkin_server_store_new();
  GListModel *model = pumpkin_server_store_get_model(store);
  guint count = g_list_model_get_n_items(model);
  g_assert_cmpuint(count, >, 0);
  g_autoptr(PumpkinServer) server = g_list_model_get_item(model, 0);
  g_autofree char *root = g_strdup(pumpkin_server_get_root_dir(server));
  pumpkin_server_store_set_selected(store, server);
  g_autoptr(GError) error = NULL;
  g_assert_true(pumpkin_server_store_remove_selected(store, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, count - 1);
  g_assert_null(pumpkin_server_store_get_selected(store));
  g_assert_false(g_file_test(root, G_FILE_TEST_EXISTS));
  g_assert_true(pumpkin_server_store_remove_selected(store, &error));
  g_assert_no_error(error);
}

static void
test_unknown_selection(void)
{
  g_autoptr(PumpkinServerStore) store = pumpkin_server_store_new();
  g_autoptr(PumpkinServer) server = pumpkin_server_new("unknown", "Unknown server");
  pumpkin_server_store_set_selected(store, server);
  guint count = g_list_model_get_n_items(pumpkin_server_store_get_model(store));
  g_autoptr(GError) error = NULL;
  g_assert_false(pumpkin_server_store_remove_selected(store, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpuint(g_list_model_get_n_items(pumpkin_server_store_get_model(store)), ==, count);
  g_assert_true(pumpkin_server_store_get_selected(store) == server);
}

static void
server_finalized(gpointer data, GObject *object)
{
  (void)object;
  gboolean *finalized = data;
  *finalized = TRUE;
}

static void
test_model_ownership(void)
{
  PumpkinServerStore *store = pumpkin_server_store_new();
  GListModel *model = pumpkin_server_store_get_model(store);
  g_assert_cmpuint(g_list_model_get_n_items(model), >, 0);
  PumpkinServer *server = g_list_model_get_item(model, 0);
  gboolean finalized = FALSE;
  g_object_weak_ref(G_OBJECT(server), server_finalized, &finalized);

  g_clear_object(&server);
  g_assert_false(finalized);
  g_clear_object(&store);
  g_assert_true(finalized);
}

int
main(int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  profile_dir = g_dir_make_tmp("smashed-server-store-profile-XXXXXX", &error);
  g_assert_no_error(error);
  g_setenv("HOME", profile_dir, TRUE);
  g_setenv("XDG_CONFIG_HOME", profile_dir, TRUE);
  g_setenv("XDG_DATA_HOME", profile_dir, TRUE);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/server-store/model-ownership", test_model_ownership);
  g_test_add("/server-store/normal-tree", Fixture, NULL, fixture_setup, test_normal_tree, fixture_teardown);
  g_test_add("/server-store/child-links", Fixture, NULL, fixture_setup, test_child_links, fixture_teardown);
  g_test_add("/server-store/root-link", Fixture, NULL, fixture_setup, test_root_link, fixture_teardown);
  g_test_add("/server-store/invalid-path", Fixture, NULL, fixture_setup, test_invalid_path, fixture_teardown);
  g_test_add_data_func("/server-store/selected-already-gone", GINT_TO_POINTER(TRUE),
    test_remove_selected_already_gone);
  g_test_add_data_func("/server-store/selected-already-gone-no-error-output", GINT_TO_POINTER(FALSE),
    test_remove_selected_already_gone);
  g_test_add_func("/server-store/selected-success", test_remove_selected_success);
  g_test_add_func("/server-store/unknown-selection", test_unknown_selection);
  int result = g_test_run();
  g_assert_true(pumpkin_server_store_remove_tree(profile_dir, &error));
  g_assert_no_error(error);
  g_free(profile_dir);
  return result;
}
