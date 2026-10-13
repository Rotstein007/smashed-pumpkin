#include "config.h"
#include "download.h"
#include "release-metadata.h"
#include <string.h>

#include <libsoup/soup.h>
#include <gio/gio.h>
#include <stdio.h>
#include <time.h>

typedef struct {
  char *url;
  char *dest;
  SoupSession *session;
  SoupMessage *message;
  GInputStream *stream;
  GOutputStream *output;
  GFile *staging_file;
  GBytes *pending_bytes;
  goffset total;
  goffset current;
  PumpkinDownloadProgress progress_cb;
  gpointer progress_data;
} DownloadState;

typedef struct {
  SoupSession *session;
  SoupMessage *message;
  PumpkinReleaseChannel channel;
} ResolveState;

G_DEFINE_QUARK(pumpkin-download-http-error, pumpkin_download_http_error)

static SoupSession *shared_session = NULL;

static SoupSession *
get_shared_session(void)
{
  if (shared_session == NULL) {
    shared_session = soup_session_new_with_options("user-agent",
                                                   APP_NAME "/" APP_VERSION,
                                                   /* Abort messages that see no network
                                                    * activity for a minute instead of
                                                    * hanging on a stalled server. */
                                                   "timeout", 60,
                                                   NULL);
  }
  return shared_session;
}

static gboolean
content_type_is_probably_text(const char *content_type)
{
  if (content_type == NULL || *content_type == '\0') {
    return FALSE;
  }

  return g_str_has_prefix(content_type, "text/") ||
         g_str_has_prefix(content_type, "application/json") ||
         g_str_has_prefix(content_type, "application/xml") ||
         g_str_has_prefix(content_type, "text/xml") ||
         g_str_has_prefix(content_type, "application/xhtml+xml");
}

static void
clear_staging_file(DownloadState *state)
{
  if (state->output != NULL) {
    g_output_stream_close(state->output, NULL, NULL);
    g_clear_object(&state->output);
  }
  if (state->staging_file != NULL) {
    g_file_delete(state->staging_file, NULL, NULL);
    g_clear_object(&state->staging_file);
  }
}

static void
download_state_free(DownloadState *state)
{
  if (state == NULL) {
    return;
  }
  g_clear_pointer(&state->url, g_free);
  g_clear_pointer(&state->dest, g_free);
  g_clear_object(&state->session);
  g_clear_object(&state->message);
  g_clear_object(&state->stream);
  clear_staging_file(state);
  g_clear_pointer(&state->pending_bytes, g_bytes_unref);
  g_free(state);
}

static void
resolve_state_free(ResolveState *state)
{
  if (state == NULL) {
    return;
  }
  g_clear_object(&state->session);
  g_clear_object(&state->message);
  g_free(state);
}

static void
fail_download(GTask *task, GError *error)
{
  DownloadState *state = g_task_get_task_data(task);
  /* Close only our own staging file, never a replacement stream for the destination. */
  clear_staging_file(state);
  g_task_return_error(task, error);
  g_object_unref(task);
}

/* A crash or forced kill leaves UUID staging files behind. Sweep files older
 * than a day from the destination directory before starting a new download;
 * active downloads rewrite their staging file constantly, so age is a safe
 * liveness signal. */
static void
sweep_stale_staging_files(GFile *parent)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileEnumerator) children = g_file_enumerate_children(parent,
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
  if (children == NULL) return;
  gint64 cutoff = (gint64)time(NULL) - 24 * 60 * 60;
  while (TRUE) {
    g_autoptr(GFileInfo) info = g_file_enumerator_next_file(children, NULL, NULL);
    if (info == NULL) break;
    const char *name = g_file_info_get_name(info);
    if (!g_str_has_prefix(name, ".smashed-pumpkin-download-")) continue;
    g_autoptr(GDateTime) modified = g_file_info_get_modification_date_time(info);
    if (modified == NULL || g_date_time_to_unix(modified) >= cutoff) continue;
    g_autoptr(GFile) stale = g_file_get_child(parent, name);
    g_file_delete(stale, NULL, NULL);
  }
}

static gboolean
create_staging_file(DownloadState *state, GCancellable *cancellable, GError **error)
{
  g_autoptr(GFile) destination = g_file_new_for_path(state->dest);
  g_autoptr(GFile) parent = g_file_get_parent(destination);
  if (parent == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid download destination");
    return FALSE;
  }

  sweep_stale_staging_files(parent);

  g_autofree char *uuid = g_uuid_string_random();
  g_autofree char *name = g_strdup_printf(".smashed-pumpkin-download-%s", uuid);
  g_autoptr(GFile) staging = g_file_get_child(parent, name);
  state->output = G_OUTPUT_STREAM(g_file_create(staging, G_FILE_CREATE_NONE, cancellable, error));
  if (state->output == NULL) {
    return FALSE;
  }
  state->staging_file = g_steal_pointer(&staging);

#ifndef G_OS_WIN32
  /* Preserve the permissions of an existing regular file, as g_file_replace() did.
   * Best effort only: filesystems without chmod support (FAT/exFAT, some network
   * shares) must not turn a permission-copy failure into a download failure. */
  g_autoptr(GError) info_error = NULL;
  g_autoptr(GFileInfo) info = g_file_query_info(destination,
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_UNIX_MODE,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &info_error);
  if (info == NULL && !g_error_matches(info_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
    g_propagate_error(error, g_steal_pointer(&info_error));
    return FALSE;
  }
  if (info != NULL && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
      g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
    guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 0777;
    g_file_set_attribute_uint32(state->staging_file, G_FILE_ATTRIBUTE_UNIX_MODE, mode,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
  }
#endif
  return TRUE;
}

static gboolean
commit_download(GTask *task, GError **error)
{
  DownloadState *state = g_task_get_task_data(task);
  GCancellable *cancellable = g_task_get_cancellable(task);
  if (!g_output_stream_close(state->output, cancellable, error)) {
    return FALSE;
  }
  if (cancellable != NULL && g_cancellable_set_error_if_cancelled(cancellable, error)) {
    return FALSE;
  }

  g_autoptr(GFile) destination = g_file_new_for_path(state->dest);
  /* Same-directory rename is the commit point. Do not fall back to a partial copy,
   * or report late cancellation after a complete file has already been committed. */
  if (!g_file_move(state->staging_file, destination,
                   G_FILE_COPY_OVERWRITE | G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                   NULL, NULL, NULL, error)) {
    return FALSE;
  }
  g_clear_object(&state->staging_file);
  /* The stream is closed and the staging file committed; drop the handle so
   * cleanup never touches them again. */
  g_clear_object(&state->output);
  g_task_set_check_cancellable(task, FALSE);
  return TRUE;
}

static void on_read_chunk(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_write_chunk(GObject *source, GAsyncResult *res, gpointer user_data);

static void
on_read_chunk(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  DownloadState *state = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), res, &error);
  if (bytes == NULL) {
    fail_download(task, g_steal_pointer(&error));
    return;
  }

  gsize size = g_bytes_get_size(bytes);
  if (size == 0) {
    if (!commit_download(task, &error)) {
      fail_download(task, g_steal_pointer(&error));
      return;
    }
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  state->pending_bytes = g_bytes_ref(bytes);
  g_output_stream_write_all_async(state->output, g_bytes_get_data(bytes, NULL), size,
                                  G_PRIORITY_DEFAULT, g_task_get_cancellable(task),
                                  on_write_chunk, task);
}

static void
on_write_chunk(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  DownloadState *state = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  gsize written = 0;
  if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), res, &written, &error)) {
    fail_download(task, g_steal_pointer(&error));
    return;
  }

  state->current += written;
  g_clear_pointer(&state->pending_bytes, g_bytes_unref);

  if (state->progress_cb != NULL) {
    state->progress_cb(state->current, state->total, state->progress_data);
  }

  g_input_stream_read_bytes_async(state->stream, 64 * 1024, G_PRIORITY_DEFAULT,
                                  g_task_get_cancellable(task), on_read_chunk, task);
}

static void
on_download_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  DownloadState *state = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  state->stream = soup_session_send_finish(state->session, res, &error);
  if (state->stream == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  guint status = soup_message_get_status(state->message);
  if (status < 200 || status >= 300) {
    g_task_return_new_error(task, PUMPKIN_DOWNLOAD_HTTP_ERROR, (gint)status,
                            "Download failed with HTTP status %u", status);
    g_object_unref(task);
    return;
  }
  SoupMessageHeaders *headers = soup_message_get_response_headers(state->message);
  const char *content_type = soup_message_headers_get_content_type(headers, NULL);
  GUri *uri = soup_message_get_uri(state->message);
  g_autofree char *final_url = uri != NULL ? g_uri_to_string_partial(uri, G_URI_HIDE_PASSWORD) : NULL;
  state->total = soup_message_headers_get_content_length(headers);

  if (content_type_is_probably_text(content_type)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Download returned %s instead of a binary (HTTP %u, URL %s)",
                            content_type, status, final_url != NULL ? final_url : state->url);
    g_object_unref(task);
    return;
  }

  if (!create_staging_file(state, g_task_get_cancellable(task), &error)) {
    fail_download(task, g_steal_pointer(&error));
    return;
  }

  state->current = 0;
  if (state->progress_cb != NULL) {
    state->progress_cb(state->current, state->total, state->progress_data);
  }
  g_input_stream_read_bytes_async(state->stream, 64 * 1024, G_PRIORITY_DEFAULT,
                                  g_task_get_cancellable(task), on_read_chunk, task);
}

void
pumpkin_download_file_async(const char *url,
                            const char *dest_path,
                            GCancellable *cancellable,
                            PumpkinDownloadProgress progress_cb,
                            gpointer progress_data,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  pumpkin_download_file_authorized_async(url, dest_path, NULL, cancellable,
                                         progress_cb, progress_data, callback, user_data);
}

void
pumpkin_download_file_authorized_async(const char *url,
                                       const char *dest_path,
                                       const char *authorization,
                                       GCancellable *cancellable,
                                       PumpkinDownloadProgress progress_cb,
                                       gpointer progress_data,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  if (url == NULL || *url == '\0' || dest_path == NULL || *dest_path == '\0') {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A download URL and destination are required");
    g_object_unref(task);
    return;
  }

  g_autoptr(SoupMessage) message = soup_message_new("GET", url);
  if (message == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid download URL");
    g_object_unref(task);
    return;
  }

  DownloadState *state = g_new0(DownloadState, 1);
  state->url = g_strdup(url);
  state->dest = g_strdup(dest_path);
  state->session = g_object_ref(get_shared_session());
  state->message = g_steal_pointer(&message);
  if (authorization != NULL && *authorization != '\0') {
    soup_message_headers_append(soup_message_get_request_headers(state->message),
                                "Authorization", authorization);
  }
  state->progress_cb = progress_cb;
  state->progress_data = progress_data;
  state->total = -1;
  state->current = 0;

  g_task_set_task_data(task, state, (GDestroyNotify)download_state_free);

  soup_session_send_async(state->session, state->message, G_PRIORITY_DEFAULT, cancellable,
                          on_download_ready, task);
}

gboolean
pumpkin_download_file_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
on_resolve_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  ResolveState *state = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(state->session, res, &error);
  if (bytes == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  guint status = soup_message_get_status(state->message);
  if (status != SOUP_STATUS_OK) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
      status == 403 || status == 429 ? "GitHub update check was rate limited (HTTP %u). Try again later." :
      "Pumpkin update check failed (HTTP %u). No other release channel was selected.", status);
    g_object_unref(task);
    return;
  }
  gsize size = 0;
  const char *data = g_bytes_get_data(bytes, &size);
  if (size == 0 || size > 2 * 1024 * 1024 || memchr(data, '\0', size) != NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "GitHub returned an empty, oversized or binary release response");
    g_object_unref(task);
    return;
  }
  g_autofree char *json = g_strndup(data, size);
  PumpkinResolvedDownload *resolved = pumpkin_release_parse_metadata(json, state->channel, pumpkin_release_asset_name(), &error);
  if (resolved != NULL) {
    g_task_return_pointer(task, resolved, (GDestroyNotify)pumpkin_resolved_download_free);
  } else {
    g_task_return_error(task, g_steal_pointer(&error));
  }
  g_object_unref(task);
}

void
pumpkin_resolve_channel_async(PumpkinReleaseChannel channel, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  if (channel != PUMPKIN_RELEASE_NIGHTLY && channel != PUMPKIN_RELEASE_STABLE) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Unknown Pumpkin release channel");
    g_object_unref(task);
    return;
  }
  ResolveState *state = g_new0(ResolveState, 1);
  state->channel = channel;
  state->session = soup_session_new_with_options("user-agent", APP_NAME "/" APP_VERSION, "timeout", 30, NULL);
  const char *api = channel == PUMPKIN_RELEASE_STABLE ?
    "https://api.github.com/repos/Pumpkin-MC/Pumpkin/releases/latest" :
    "https://api.github.com/repos/Pumpkin-MC/Pumpkin/releases/tags/nightly";
  state->message = soup_message_new("GET", api);
  SoupMessageHeaders *headers = soup_message_get_request_headers(state->message);
  soup_message_headers_append(headers, "Accept", "application/vnd.github+json");
  soup_message_headers_append(headers, "X-GitHub-Api-Version", "2026-03-10");
  soup_message_headers_append(headers, "Cache-Control", "no-cache");
  g_task_set_task_data(task, state, (GDestroyNotify)resolve_state_free);
  soup_session_send_and_read_async(state->session, state->message, G_PRIORITY_DEFAULT, cancellable, on_resolve_ready, task);
}

void
pumpkin_resolve_latest_async(GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  pumpkin_resolve_channel_async(PUMPKIN_RELEASE_NIGHTLY, cancellable, callback, user_data);
}

PumpkinResolvedDownload *
pumpkin_resolve_latest_finish(GAsyncResult *result, PumpkinDownloadResult *result_code, GError **error)
{
  PumpkinResolvedDownload *resolved = g_task_propagate_pointer(G_TASK(result), error);
  if (resolved != NULL && result_code != NULL) {
    *result_code = resolved->result_code;
  }
  return resolved;
}
