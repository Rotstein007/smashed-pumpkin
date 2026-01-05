#include "download.h"

#include <libsoup/soup.h>
#include <gio/gio.h>

#define PUMPKIN_DOWNLOAD_PAGE "https://pumpkinmc.org/download.html"

typedef struct {
  char *url;
  char *dest;
  SoupSession *session;
  SoupMessage *message;
  GInputStream *stream;
  GOutputStream *output;
  GBytes *pending_bytes;
  goffset total;
  goffset current;
  PumpkinDownloadProgress progress_cb;
  gpointer progress_data;
} DownloadState;

typedef struct {
  SoupSession *session;
} ResolveState;

static void
download_state_free(DownloadState *state)
{
  if (state == NULL) {
    return;
  }
  if (state->session != NULL) {
    soup_session_abort(state->session);
  }
  g_clear_pointer(&state->url, g_free);
  g_clear_pointer(&state->dest, g_free);
  g_clear_object(&state->session);
  g_clear_object(&state->message);
  g_clear_object(&state->stream);
  g_clear_object(&state->output);
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
  g_free(state);
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
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  gsize size = g_bytes_get_size(bytes);
  if (size == 0) {
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  state->pending_bytes = g_bytes_ref(bytes);
  g_output_stream_write_bytes_async(state->output, bytes, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable(task), on_write_chunk, task);
}

static void
on_write_chunk(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  DownloadState *state = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  gssize written = g_output_stream_write_bytes_finish(G_OUTPUT_STREAM(source), res, &error);
  if (written < 0) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  if (state->pending_bytes != NULL) {
    state->current += g_bytes_get_size(state->pending_bytes);
    g_clear_pointer(&state->pending_bytes, g_bytes_unref);
  } else {
    state->current += written;
  }

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
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Download failed with HTTP status %u", status);
    g_object_unref(task);
    return;
  }
  SoupMessageHeaders *headers = soup_message_get_response_headers(state->message);
  state->total = soup_message_headers_get_content_length(headers);

  g_autoptr(GInputStream) request_stream = state->stream;

  GFile *file = g_file_new_for_path(state->dest);
  state->output = G_OUTPUT_STREAM(g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error));
  g_object_unref(file);
  if (state->output == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  state->stream = g_object_ref(request_stream);
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
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  DownloadState *state = g_new0(DownloadState, 1);
  state->url = g_strdup(url);
  state->dest = g_strdup(dest_path);
  state->session = soup_session_new();
  state->message = soup_message_new("GET", url);
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

static char *
extract_linux_x64_url(const char *html)
{
  g_autoptr(GRegex) regex = g_regex_new("https://github\\.com/Pumpkin-MC/Pumpkin/releases/download/[^\"]*?/pumpkin-?X64-?Linux",
                                      G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, html, 0, &match)) {
    return NULL;
  }

  return g_match_info_fetch(match, 0);
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

  gsize size = 0;
  const char *data = g_bytes_get_data(bytes, &size);
  g_autofree char *html = g_strndup(data, size);

  g_autofree char *url = extract_linux_x64_url(html);
  if (url == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to find Linux x64 download URL");
  } else {
    g_task_set_task_data(task, GUINT_TO_POINTER(PUMPKIN_DOWNLOAD_OK), NULL);
    g_task_return_pointer(task, g_strdup(url), g_free);
  }

  g_object_unref(task);
}

void
pumpkin_resolve_latest_linux_x64_async(GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  ResolveState *state = g_new0(ResolveState, 1);
  state->session = soup_session_new();
  g_task_set_task_data(task, state, (GDestroyNotify)resolve_state_free);

  SoupMessage *msg = soup_message_new("GET", PUMPKIN_DOWNLOAD_PAGE);
  soup_session_send_and_read_async(state->session, msg, G_PRIORITY_DEFAULT, cancellable,
                                   on_resolve_ready, task);
  g_object_unref(msg);
}

char *
pumpkin_resolve_latest_linux_x64_finish(GAsyncResult *result,
                                         PumpkinDownloadResult *result_code,
                                         GError **error)
{
  if (result_code != NULL) {
    gpointer code = g_task_get_task_data(G_TASK(result));
    *result_code = (PumpkinDownloadResult)GPOINTER_TO_UINT(code);
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}
