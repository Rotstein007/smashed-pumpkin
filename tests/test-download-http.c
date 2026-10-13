#include "download.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>

static const char payload[] = "\0asm\1\0\0\0";

typedef struct {
  SoupServer *server;
  char *base_url;
  char *directory;
  char *destination;
} Fixture;

typedef struct {
  GMainLoop *loop;
  gboolean ok;
  GError *error;
  guint callbacks;
  guint progress_calls;
  goffset current;
  goffset total;
  GCancellable *cancel;
  gboolean cancel_after_progress;
} Result;

static void
serve_request(SoupServer *server, SoupServerMessage *message, const char *path,
              GHashTable *query, gpointer user_data)
{
  (void)server;
  (void)query;
  (void)user_data;
  guint status = g_str_equal(path, "/401") ? SOUP_STATUS_UNAUTHORIZED :
    g_str_equal(path, "/403") ? SOUP_STATUS_FORBIDDEN :
    g_str_equal(path, "/503") ? SOUP_STATUS_SERVICE_UNAVAILABLE : SOUP_STATUS_OK;
  soup_server_message_set_status(message, status, NULL);
  if (g_str_equal(path, "/large")) {
    g_autofree char *large_payload = g_malloc0(1024 * 1024);
    soup_server_message_set_response(message, "application/octet-stream", SOUP_MEMORY_COPY,
      large_payload, 1024 * 1024);
  } else {
    soup_server_message_set_response(message,
      g_str_equal(path, "/text") ? "text/html" : "application/octet-stream", SOUP_MEMORY_COPY,
      payload, sizeof(payload) - 1);
  }
}

static void
fixture_setup(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  fixture->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(fixture->server, NULL, serve_request, NULL, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(fixture->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(fixture->server);
  fixture->base_url = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  fixture->directory = g_dir_make_tmp("smashed-download-http-XXXXXX", &error);
  g_assert_no_error(error);
  fixture->destination = g_build_filename(fixture->directory, "download.wasm", NULL);
}

static void
fixture_teardown(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  soup_server_disconnect(fixture->server);
  g_object_unref(fixture->server);
  g_free(fixture->base_url);
  if (g_file_test(fixture->destination, G_FILE_TEST_IS_DIR)) {
    g_assert_cmpint(g_rmdir(fixture->destination), ==, 0);
  } else if (g_file_test(fixture->destination, G_FILE_TEST_EXISTS)) {
    g_assert_cmpint(g_remove(fixture->destination), ==, 0);
  }
  /* Failure paths must not leave staging files behind. */
  g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
  g_free(fixture->destination);
  g_free(fixture->directory);
  while (g_main_context_iteration(NULL, FALSE)) {}
}

static void
download_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  Result *state = user_data;
  ++state->callbacks;
  state->ok = pumpkin_download_file_finish(result, &state->error);
  g_main_loop_quit(state->loop);
}

static gboolean
test_guard(gpointer user_data)
{
  (void)user_data;
  g_error("Download failed to complete within the test deadline");
  return G_SOURCE_REMOVE;
}

static void
report_progress(goffset current, goffset total, gpointer user_data)
{
  Result *result = user_data;
  g_assert_cmpint(current, >=, result->current);
  result->current = current;
  result->total = total;
  ++result->progress_calls;
  if (result->cancel_after_progress && current > 0) {
    g_cancellable_cancel(result->cancel);
  }
}

static void
run_download_url(Fixture *fixture, const char *url, GCancellable *cancel, Result *result)
{
  result->loop = g_main_loop_new(NULL, FALSE);
  result->cancel = cancel;
  guint guard = g_timeout_add_seconds(5, test_guard, NULL);
  pumpkin_download_file_async(url, fixture->destination, cancel, report_progress, result,
                             download_finished, result);
  g_assert_cmpuint(result->callbacks, ==, 0);
  g_main_loop_run(result->loop);
  g_source_remove(guard);
  g_main_loop_unref(result->loop);
  result->loop = NULL;
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(result->callbacks, ==, 1);
}

static void
run_download(Fixture *fixture, const char *path, GCancellable *cancel, Result *result)
{
  g_autofree char *url = g_strconcat(fixture->base_url, path, NULL);
  run_download_url(fixture, url, cancel, result);
}

static void
test_http_status(Fixture *fixture, gconstpointer user_data)
{
  guint status = GPOINTER_TO_UINT(user_data);
  g_autofree char *path = g_strdup_printf("%u", status);
  Result result = {0};
  g_assert_true(g_file_set_contents(fixture->destination, "existing plugin", -1, NULL));
  run_download(fixture, path, NULL, &result);
  g_assert_false(result.ok);
  g_assert_error(result.error, PUMPKIN_DOWNLOAD_HTTP_ERROR, (gint)status);
  /* The UI can still classify the response when its message is translated. */
  g_free(result.error->message);
  result.error->message = g_strdup("Download fehlgeschlagen: Der Server hat die Anfrage zurückgewiesen");
  g_assert_true(g_error_matches(result.error, PUMPKIN_DOWNLOAD_HTTP_ERROR, (gint)status));
  g_assert_false(g_error_matches(result.error, PUMPKIN_DOWNLOAD_HTTP_ERROR,
    status == SOUP_STATUS_UNAUTHORIZED ? SOUP_STATUS_FORBIDDEN : SOUP_STATUS_UNAUTHORIZED));
  g_autofree char *contents = NULL;
  g_assert_true(g_file_get_contents(fixture->destination, &contents, NULL, NULL));
  g_assert_cmpstr(contents, ==, "existing plugin");
  g_clear_error(&result.error);
}

static void
test_success(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  Result result = {0};
  run_download(fixture, "ok", NULL, &result);
  g_assert_true(result.ok);
  g_assert_no_error(result.error);
  g_autofree char *contents = NULL;
  gsize length = 0;
  g_assert_true(g_file_get_contents(fixture->destination, &contents, &length, NULL));
  g_assert_cmpmem(contents, length, payload, sizeof(payload) - 1);
}

static void
test_staging_cleanup(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autofree char *old_path = g_build_filename(fixture->directory,
    ".smashed-pumpkin-download-00000000-0000-4000-8000-000000000001", NULL);
  g_autofree char *recent_path = g_build_filename(fixture->directory,
    ".smashed-pumpkin-download-00000000-0000-4000-8000-000000000002", NULL);
  g_autofree char *unrelated_path = g_build_filename(fixture->directory, "unrelated.wasm", NULL);
  g_assert_true(g_file_set_contents(old_path, "abandoned download", -1, NULL));
  g_assert_true(g_file_set_contents(recent_path, "active download", -1, NULL));
  g_assert_true(g_file_set_contents(unrelated_path, "unrelated file", -1, NULL));

  const char *aged_paths[] = {old_path, unrelated_path};
  guint64 old_time = (guint64)(g_get_real_time() / G_USEC_PER_SEC) - 2 * 24 * 60 * 60;
  for (guint i = 0; i < G_N_ELEMENTS(aged_paths); ++i) {
    g_autoptr(GFile) file = g_file_new_for_path(aged_paths[i]);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_attribute_uint64(file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
      old_time, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error));
    g_assert_no_error(error);
  }

  /* Cover both deletion and the recent-file skip, including another sweep of
   * the retained file. Sanitizer runs must release every modification date. */
  for (guint i = 0; i < 2; ++i) {
    Result result = {0};
    run_download(fixture, "ok", NULL, &result);
    g_assert_true(result.ok);
    g_assert_no_error(result.error);
    g_assert_false(g_file_test(old_path, G_FILE_TEST_EXISTS));
    g_autofree char *recent_contents = NULL;
    g_assert_true(g_file_get_contents(recent_path, &recent_contents, NULL, NULL));
    g_assert_cmpstr(recent_contents, ==, "active download");
    g_autofree char *unrelated_contents = NULL;
    g_assert_true(g_file_get_contents(unrelated_path, &unrelated_contents, NULL, NULL));
    g_assert_cmpstr(unrelated_contents, ==, "unrelated file");
  }
  g_assert_cmpint(g_remove(recent_path), ==, 0);
  g_assert_cmpint(g_remove(unrelated_path), ==, 0);
}

static void
test_cancel(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  Result result = {0};
  run_download(fixture, "ok", cancel, &result);
  g_assert_false(result.ok);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&result.error);
}

static void
test_cancel_midstream(Fixture *fixture, gconstpointer user_data)
{
  gboolean existing = GPOINTER_TO_INT(user_data);
  if (existing) {
    g_assert_true(g_file_set_contents(fixture->destination, "existing plugin", -1, NULL));
  }
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  Result result = {.cancel_after_progress = TRUE};
  run_download(fixture, "large", cancel, &result);
  g_assert_cmpuint(result.progress_calls, >=, 2);
  g_assert_cmpint(result.current, >, 0);
  g_assert_cmpint(result.current, <, 1024 * 1024);
  g_assert_false(result.ok);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&result.error);
  if (existing) {
    g_autofree char *contents = NULL;
    g_assert_true(g_file_get_contents(fixture->destination, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "existing plugin");
  } else {
    g_assert_false(g_file_test(fixture->destination, G_FILE_TEST_EXISTS));
  }
}

static void
test_replace_success(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_assert_true(g_file_set_contents(fixture->destination, "existing plugin", -1, NULL));
#ifndef G_OS_WIN32
  g_assert_cmpint(g_chmod(fixture->destination, 0640), ==, 0);
#endif
  Result result = {0};
  run_download(fixture, "large", NULL, &result);
  g_assert_true(result.ok);
  g_assert_no_error(result.error);
  g_assert_cmpint(result.current, ==, 1024 * 1024);
  g_assert_cmpint(result.total, ==, 1024 * 1024);
  g_autofree char *contents = NULL;
  gsize length = 0;
  g_assert_true(g_file_get_contents(fixture->destination, &contents, &length, NULL));
  g_assert_cmpuint(length, ==, 1024 * 1024);
  for (gsize i = 0; i < length; ++i) {
    g_assert_cmpint(contents[i], ==, 0);
  }
#ifndef G_OS_WIN32
  GStatBuf statbuf;
  g_assert_cmpint(g_stat(fixture->destination, &statbuf), ==, 0);
  g_assert_cmpuint(statbuf.st_mode & 0777, ==, 0640);
#endif
}

static void
test_commit_failure(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_assert_cmpint(g_mkdir(fixture->destination, 0755), ==, 0);
  Result result = {0};
  run_download(fixture, "ok", NULL, &result);
  g_assert_false(result.ok);
  g_assert_nonnull(result.error);
  g_assert_true(g_file_test(fixture->destination, G_FILE_TEST_IS_DIR));
  g_clear_error(&result.error);
}

static void
test_reject_text(Fixture *fixture, gconstpointer user_data)
{
  (void)user_data;
  g_assert_true(g_file_set_contents(fixture->destination, "existing plugin", -1, NULL));
  Result result = {0};
  run_download(fixture, "text", NULL, &result);
  g_assert_false(result.ok);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_autofree char *contents = NULL;
  g_assert_true(g_file_get_contents(fixture->destination, &contents, NULL, NULL));
  g_assert_cmpstr(contents, ==, "existing plugin");
  g_clear_error(&result.error);
}

static void
test_invalid_url(Fixture *fixture, gconstpointer user_data)
{
  Result result = {0};
  run_download_url(fixture, user_data, NULL, &result);
  g_assert_false(result.ok);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_false(g_file_test(fixture->destination, G_FILE_TEST_EXISTS));
  g_clear_error(&result.error);
}

static void
test_message_is_not_status(void)
{
  g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
    "An unrelated message containing HTTP status 401 and HTTP status 403");
  g_assert_false(g_error_matches(error, PUMPKIN_DOWNLOAD_HTTP_ERROR, SOUP_STATUS_UNAUTHORIZED));
  g_assert_false(g_error_matches(error, PUMPKIN_DOWNLOAD_HTTP_ERROR, SOUP_STATUS_FORBIDDEN));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add("/download-http/401", Fixture, GUINT_TO_POINTER(401), fixture_setup, test_http_status, fixture_teardown);
  g_test_add("/download-http/403", Fixture, GUINT_TO_POINTER(403), fixture_setup, test_http_status, fixture_teardown);
  g_test_add("/download-http/503", Fixture, GUINT_TO_POINTER(503), fixture_setup, test_http_status, fixture_teardown);
  g_test_add("/download-http/success", Fixture, NULL, fixture_setup, test_success, fixture_teardown);
  g_test_add("/download-http/staging-cleanup", Fixture, NULL,
    fixture_setup, test_staging_cleanup, fixture_teardown);
  g_test_add("/download-http/cancel", Fixture, NULL, fixture_setup, test_cancel, fixture_teardown);
  g_test_add("/download-http/cancel-midstream-existing", Fixture, GINT_TO_POINTER(TRUE),
    fixture_setup, test_cancel_midstream, fixture_teardown);
  g_test_add("/download-http/cancel-midstream-new", Fixture, GINT_TO_POINTER(FALSE),
    fixture_setup, test_cancel_midstream, fixture_teardown);
  g_test_add("/download-http/replace-success", Fixture, NULL,
    fixture_setup, test_replace_success, fixture_teardown);
  g_test_add("/download-http/commit-failure", Fixture, NULL,
    fixture_setup, test_commit_failure, fixture_teardown);
  g_test_add("/download-http/reject-text", Fixture, NULL,
    fixture_setup, test_reject_text, fixture_teardown);
  g_test_add("/download-http/invalid-url", Fixture, "not a URL",
    fixture_setup, test_invalid_url, fixture_teardown);
  g_test_add("/download-http/missing-url", Fixture, NULL,
    fixture_setup, test_invalid_url, fixture_teardown);
  g_test_add_func("/download-http/message-is-not-status", test_message_is_not_status);
  return g_test_run();
}
