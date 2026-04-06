#include "config.h"
#include "download.h"

#include <libsoup/soup.h>
#include <gio/gio.h>
#include <stdio.h>

#define PUMPKIN_DOWNLOAD_PAGE "https://pumpkinmc.org/download/"
#define PUMPKIN_DOWNLOAD_API_URL "https://api.github.com/repos/Pumpkin-MC/Pumpkin/releases/tags/nightly"

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
  char *resolved_url;
  gboolean used_fallback;
  SoupMessage *metadata_message;
} ResolveState;

static SoupSession *shared_session = NULL;

static SoupSession *
get_shared_session(void)
{
  if (shared_session == NULL) {
    shared_session = soup_session_new_with_options("user-agent",
                                                   APP_NAME "/" APP_VERSION,
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
         g_str_equal(content_type, "application/json") ||
         g_str_equal(content_type, "application/xml") ||
         g_str_equal(content_type, "text/xml") ||
         g_str_equal(content_type, "application/xhtml+xml");
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
  g_clear_pointer(&state->resolved_url, g_free);
  g_clear_object(&state->session);
  g_clear_object(&state->metadata_message);
  g_free(state);
}

void
pumpkin_resolved_download_free(PumpkinResolvedDownload *resolved)
{
  if (resolved == NULL) {
    return;
  }
  g_clear_pointer(&resolved->url, g_free);
  g_clear_pointer(&resolved->build_id, g_free);
  g_clear_pointer(&resolved->build_label, g_free);
  g_free(resolved);
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
    if (!g_output_stream_close(state->output, g_task_get_cancellable(task), &error)) {
      g_task_return_error(task, g_steal_pointer(&error));
    } else {
      g_task_return_boolean(task, TRUE);
    }
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
  state->session = g_object_ref(get_shared_session());
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

static const char *
expected_asset_name(void)
{
#if defined(G_OS_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
  return "pumpkin-ARM64-Windows.exe";
#else
  return "pumpkin-X64-Windows.exe";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "pumpkin-ARM64-macOS";
#else
  return "pumpkin-X64-macOS";
#endif
#else
#if defined(__aarch64__) || defined(__arm64__)
  return "pumpkin-ARM64-Linux";
#else
  return "pumpkin-X64-Linux";
#endif
#endif
}

static char *
extract_asset_url(const char *html, const char *asset_name)
{
  if (html == NULL || asset_name == NULL) {
    return NULL;
  }

  g_autofree char *escaped = g_regex_escape_string(asset_name, -1);
  g_autofree char *pattern = g_strdup_printf(
    "https://github\\.com/Pumpkin-MC/Pumpkin/releases/download/[^\"\\s]*?/%s",
    escaped);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, html, 0, &match)) {
    return NULL;
  }

  return g_match_info_fetch(match, 0);
}

static char *
extract_asset_url_from_release_json(const char *json, const char *asset_name)
{
  if (json == NULL || asset_name == NULL) {
    return NULL;
  }

  g_autofree char *escaped_name = g_regex_escape_string(asset_name, -1);
  g_autofree char *pattern = g_strdup_printf(
    "\"name\"\\s*:\\s*\"%s\"(?s:.*?)\"browser_download_url\"\\s*:\\s*\"([^\"]+)\"",
    escaped_name);
  g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, json, 0, &match)) {
    return NULL;
  }

  return g_match_info_fetch(match, 1);
}

static gboolean
url_matches_keywords(const char *url, const char *const *os_keywords, const char *const *arch_keywords)
{
  if (url == NULL || os_keywords == NULL || arch_keywords == NULL) {
    return FALSE;
  }
  g_autofree char *lower = g_ascii_strdown(url, -1);

  gboolean os_match = FALSE;
  for (int i = 0; os_keywords[i] != NULL; i++) {
    if (strstr(lower, os_keywords[i]) != NULL) {
      os_match = TRUE;
      break;
    }
  }
  if (!os_match) {
    return FALSE;
  }

  for (int i = 0; arch_keywords[i] != NULL; i++) {
    if (strstr(lower, arch_keywords[i]) != NULL) {
      return TRUE;
    }
  }

  return FALSE;
}

static const char *const *
expected_os_keywords(void)
{
#if defined(G_OS_WIN32)
  static const char *const keywords[] = { "windows", "win", NULL };
  return keywords;
#elif defined(__APPLE__)
  static const char *const keywords[] = { "macos", "mac", "osx", "darwin", NULL };
  return keywords;
#else
  static const char *const keywords[] = { "linux", NULL };
  return keywords;
#endif
}

static const char *const *
expected_arch_keywords(void)
{
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
  static const char *const keywords[] = { "arm64", "aarch64", NULL };
  return keywords;
#else
  static const char *const keywords[] = { "x64", "x86_64", "amd64", NULL };
  return keywords;
#endif
}

static char *
extract_best_platform_url(const char *html)
{
  if (html == NULL) {
    return NULL;
  }

  const char *const *os_kw = expected_os_keywords();
  const char *const *arch_kw = expected_arch_keywords();

  g_autoptr(GRegex) regex = g_regex_new(
    "https://github\\.com/Pumpkin-MC/Pumpkin/releases/download/[^\"\\s]*?/[^\"\\s]+",
    G_REGEX_CASELESS, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;

  if (!g_regex_match(regex, html, 0, &match)) {
    return NULL;
  }

  while (g_match_info_matches(match)) {
    g_autofree char *url = g_match_info_fetch(match, 0);
    if (url != NULL && url_matches_keywords(url, os_kw, arch_kw)) {
      return g_strdup(url);
    }
    g_match_info_next(match, NULL);
  }

  return NULL;
}

static char *
fallback_nightly_url(void)
{
  const char *asset = expected_asset_name();
  if (asset == NULL) {
    return NULL;
  }
  return g_strdup_printf("https://github.com/Pumpkin-MC/Pumpkin/releases/download/nightly/%s", asset);
}

static int
month_to_num(const char *month)
{
  if (month == NULL) {
    return 0;
  }
  if (g_ascii_strcasecmp(month, "Jan") == 0) return 1;
  if (g_ascii_strcasecmp(month, "Feb") == 0) return 2;
  if (g_ascii_strcasecmp(month, "Mar") == 0) return 3;
  if (g_ascii_strcasecmp(month, "Apr") == 0) return 4;
  if (g_ascii_strcasecmp(month, "May") == 0) return 5;
  if (g_ascii_strcasecmp(month, "Jun") == 0) return 6;
  if (g_ascii_strcasecmp(month, "Jul") == 0) return 7;
  if (g_ascii_strcasecmp(month, "Aug") == 0) return 8;
  if (g_ascii_strcasecmp(month, "Sep") == 0) return 9;
  if (g_ascii_strcasecmp(month, "Oct") == 0) return 10;
  if (g_ascii_strcasecmp(month, "Nov") == 0) return 11;
  if (g_ascii_strcasecmp(month, "Dec") == 0) return 12;
  return 0;
}

static char *
normalize_etag(const char *etag)
{
  if (etag == NULL) {
    return NULL;
  }

  const char *start = etag;
  while (g_ascii_isspace(*start)) {
    start++;
  }
  if (g_str_has_prefix(start, "W/")) {
    start += 2;
    while (g_ascii_isspace(*start)) {
      start++;
    }
  }

  g_autofree char *tmp = g_strdup(start);
  g_strstrip(tmp);

  gsize len = strlen(tmp);
  if (len >= 2 && tmp[0] == '"' && tmp[len - 1] == '"') {
    tmp[len - 1] = '\0';
    return g_strdup(tmp + 1);
  }

  return g_strdup(tmp);
}

static char *
build_label_from_last_modified(const char *last_modified)
{
  if (last_modified == NULL || *last_modified == '\0') {
    return NULL;
  }

  char weekday[4] = { 0 };
  char month[4] = { 0 };
  char tz[4] = { 0 };
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int parsed = sscanf(last_modified, "%3[^,], %d %3s %d %d:%d:%d %3s",
                      weekday, &day, month, &year, &hour, &minute, &second, tz);
  int month_num = month_to_num(month);
  if (parsed == 8 && month_num > 0 && year > 0 && day > 0) {
    return g_strdup_printf("Build %04d-%02d-%02d %02d:%02d UTC",
                           year, month_num, day, hour, minute);
  }

  return g_strdup_printf("Build %s", last_modified);
}

static void
fill_build_metadata(SoupMessageHeaders *headers, PumpkinResolvedDownload *resolved)
{
  if (headers == NULL || resolved == NULL) {
    return;
  }

  const char *etag = soup_message_headers_get_one(headers, "ETag");
  const char *last_modified = soup_message_headers_get_one(headers, "Last-Modified");

  g_clear_pointer(&resolved->build_id, g_free);
  resolved->build_id = normalize_etag(etag);
  if (resolved->build_id == NULL && last_modified != NULL) {
    resolved->build_id = g_strdup(last_modified);
  }

  g_clear_pointer(&resolved->build_label, g_free);
  resolved->build_label = build_label_from_last_modified(last_modified);
  if (resolved->build_label == NULL && resolved->build_id != NULL) {
    resolved->build_label = g_strdup_printf("Build %s", resolved->build_id);
  }
}

static void on_resolve_metadata_ready(GObject *source, GAsyncResult *res, gpointer user_data);

static char *
resolve_asset_url_from_payload(const char *payload, gboolean *used_fallback)
{
  if (used_fallback != NULL) {
    *used_fallback = FALSE;
  }

  const char *asset = expected_asset_name();

  g_autofree char *url = extract_asset_url_from_release_json(payload, asset);
  if (url != NULL) {
    return g_steal_pointer(&url);
  }

  url = extract_asset_url(payload, asset);
  if (url != NULL) {
    return g_steal_pointer(&url);
  }

  url = extract_best_platform_url(payload);
  if (url != NULL) {
    return g_steal_pointer(&url);
  }

  if (used_fallback != NULL) {
    *used_fallback = TRUE;
  }
  return fallback_nightly_url();
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
    g_autofree char *fallback = fallback_nightly_url();
    if (fallback != NULL) {
      PumpkinResolvedDownload *resolved = g_new0(PumpkinResolvedDownload, 1);
      resolved->url = g_strdup(fallback);
      resolved->result_code = PUMPKIN_DOWNLOAD_FALLBACK_USED;
      g_task_return_pointer(task, resolved, (GDestroyNotify)pumpkin_resolved_download_free);
    } else {
      g_task_return_error(task, g_steal_pointer(&error));
    }
    g_object_unref(task);
    return;
  }

  gsize size = 0;
  const char *data = g_bytes_get_data(bytes, &size);
  g_autofree char *payload = g_strndup(data, size);
  gboolean used_fallback = FALSE;
  g_autofree char *resolved = resolve_asset_url_from_payload(payload, &used_fallback);
  if (resolved == NULL) {
    const char *asset = expected_asset_name();
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to find download URL for %s", asset);
    g_object_unref(task);
    return;
  }

  state->used_fallback = used_fallback;
  g_clear_pointer(&state->resolved_url, g_free);
  state->resolved_url = g_strdup(resolved);

  g_clear_object(&state->metadata_message);
  state->metadata_message = soup_message_new("HEAD", state->resolved_url);
  if (state->metadata_message == NULL) {
    PumpkinResolvedDownload *resolved = g_new0(PumpkinResolvedDownload, 1);
    resolved->url = g_strdup(state->resolved_url);
    resolved->result_code = state->used_fallback ? PUMPKIN_DOWNLOAD_FALLBACK_USED : PUMPKIN_DOWNLOAD_OK;
    g_task_return_pointer(task, resolved, (GDestroyNotify)pumpkin_resolved_download_free);
    g_object_unref(task);
    return;
  }

  soup_session_send_and_read_async(state->session, state->metadata_message, G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable(task), on_resolve_metadata_ready, task);
}

static void
on_resolve_metadata_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  ResolveState *state = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  g_autoptr(GBytes) ignored = soup_session_send_and_read_finish(state->session, res, &error);
  (void)ignored;

  PumpkinResolvedDownload *resolved = g_new0(PumpkinResolvedDownload, 1);
  resolved->url = g_strdup(state->resolved_url);
  resolved->result_code = state->used_fallback ? PUMPKIN_DOWNLOAD_FALLBACK_USED : PUMPKIN_DOWNLOAD_OK;

  if (error == NULL && state->metadata_message != NULL) {
    SoupMessageHeaders *headers = soup_message_get_response_headers(state->metadata_message);
    fill_build_metadata(headers, resolved);
  }

  g_task_return_pointer(task, resolved, (GDestroyNotify)pumpkin_resolved_download_free);
  g_object_unref(task);
}

void
pumpkin_resolve_latest_async(GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  ResolveState *state = g_new0(ResolveState, 1);
  state->session = g_object_ref(get_shared_session());
  g_task_set_task_data(task, state, (GDestroyNotify)resolve_state_free);

  SoupMessage *msg = soup_message_new("GET", PUMPKIN_DOWNLOAD_API_URL);
  soup_message_headers_append(soup_message_get_request_headers(msg),
                              "Accept",
                              "application/vnd.github+json");
  soup_message_headers_append(soup_message_get_request_headers(msg),
                              "X-GitHub-Api-Version",
                              "2022-11-28");
  soup_session_send_and_read_async(state->session, msg, G_PRIORITY_DEFAULT, cancellable,
                                   on_resolve_ready, task);
  g_object_unref(msg);
}

PumpkinResolvedDownload *
pumpkin_resolve_latest_finish(GAsyncResult *result,
                              PumpkinDownloadResult *result_code,
                              GError **error)
{
  PumpkinResolvedDownload *resolved = g_task_propagate_pointer(G_TASK(result), error);
  if (resolved != NULL && result_code != NULL) {
    *result_code = resolved->result_code;
  }
  return resolved;
}
