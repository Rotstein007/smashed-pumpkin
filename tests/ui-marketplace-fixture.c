#include "marketplace-request.h"

#include <libsoup/soup.h>
#include <string.h>

/* The UI executable links this wrapper only; the application keeps its real
 * endpoint. Delegate to the production HTTP implementation on loopback so
 * parsing, cancellation and request deadlines still run normally. */
void __real_pumpkin_marketplace_request_async(const char *url, guint timeout_ms,
  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);

static SoupServer *server;
static char *base_url;

static void
serve_plugins(SoupServer *source, SoupServerMessage *message, const char *path,
              GHashTable *query, gpointer data)
{
  (void)source;
  (void)query;
  (void)data;
  g_assert_cmpstr(path, ==, "/api/v1/rest/plugins");
  const char *json = "[{\"id\":104,\"public_id\":\"ui-fixture\","
    "\"name\":\"UI Fixture Plugin\",\"version\":\"1.0.0\","
    "\"category\":\"utility\",\"type\":\"plugin\",\"price\":0.0,"
    "\"downloads\":12,\"views\":34,\"dev_name\":\"Fixture Author\"}]";
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, json, strlen(json));
}

void
__wrap_pumpkin_marketplace_request_async(const char *url, guint timeout_ms,
  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  if (server == NULL) {
    g_autoptr(GError) error = NULL;
    server = soup_server_new(NULL, NULL);
    soup_server_add_handler(server, NULL, serve_plugins, NULL, NULL);
    g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
    g_assert_no_error(error);
    GSList *uris = soup_server_get_uris(server);
    base_url = g_uri_to_string(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  }
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  g_assert_nonnull(uri);
  g_assert_cmpstr(g_uri_get_host(uri), ==, "market.pumpkinmc.org");
  g_assert_cmpstr(g_uri_get_path(uri), ==, "/api/v1/rest/plugins");
  g_autofree char *local_url = g_strdup_printf("%sapi/v1/rest/plugins%s%s", base_url,
    g_uri_get_query(uri) != NULL ? "?" : "",
    g_uri_get_query(uri) != NULL ? g_uri_get_query(uri) : "");
  __real_pumpkin_marketplace_request_async(local_url, timeout_ms, cancellable, callback, user_data);
}
