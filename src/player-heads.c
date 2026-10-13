#include "player-heads.h"
#include "vendor/cjson/cJSON.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <time.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(cJSON, cJSON_Delete)

typedef enum { HEAD_PROFILE, HEAD_SKIN, HEAD_FLOODGATE } HeadStage;
typedef struct {
  char *identifier;
  char *path;
  GPtrArray *images;
  SoupMessage *message;
  HeadStage stage;
  gboolean verified_java;
} HeadRequest;

static SoupSession *session;
static GHashTable *requests;
static GHashTable *retry_after;

static void
weak_image_free(gpointer data)
{
  GWeakRef *image = data;
  g_weak_ref_clear(image);
  g_free(image);
}

gboolean
pumpkin_player_head_is_java(GtkImage *image)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(image), "verified-java-profile")) != 0;
}

/* Only immutable Mojang texture hashes are allowed, not arbitrary URLs from
 * profile data. Matching the UUID also prevents cross-account skin mixups. */
char *
pumpkin_player_skin_url(const char *profile_json, const char *identifier)
{
  if (profile_json == NULL || identifier == NULL) return NULL;
  g_autoptr(cJSON) profile = cJSON_Parse(profile_json);
  cJSON *id = cJSON_GetObjectItemCaseSensitive(profile, "id");
  if (!cJSON_IsString(id)) return NULL;
  GString *compact = g_string_new(NULL);
  for (const char *p = identifier; *p; p++) if (*p != '-') g_string_append_c(compact, *p);
  gboolean matches = g_ascii_strcasecmp(id->valuestring, compact->str) == 0;
  g_string_free(compact, TRUE);
  if (!matches) return NULL;
  cJSON *properties = cJSON_GetObjectItemCaseSensitive(profile, "properties");
  cJSON *property;
  cJSON_ArrayForEach(property, properties) {
    cJSON *name = cJSON_GetObjectItemCaseSensitive(property, "name");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(property, "value");
    if (!cJSON_IsString(name) || strcmp(name->valuestring, "textures") != 0 ||
        !cJSON_IsString(value) || strlen(value->valuestring) > 64 * 1024) continue;
    gsize size = 0;
    g_autofree guchar *decoded = g_base64_decode(value->valuestring, &size);
    g_autoptr(cJSON) textures = cJSON_ParseWithLength((const char *)decoded, size);
    cJSON *root = cJSON_GetObjectItemCaseSensitive(textures, "textures");
    cJSON *skin = cJSON_GetObjectItemCaseSensitive(root, "SKIN");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(skin, "url");
    if (!cJSON_IsString(url)) continue;
    const char *hash = NULL;
    if (g_str_has_prefix(url->valuestring, "http://textures.minecraft.net/texture/"))
      hash = url->valuestring + strlen("http://textures.minecraft.net/texture/");
    else if (g_str_has_prefix(url->valuestring, "https://textures.minecraft.net/texture/"))
      hash = url->valuestring + strlen("https://textures.minecraft.net/texture/");
    if (hash == NULL || strlen(hash) != 64) continue;
    gboolean valid = TRUE;
    for (const char *p = hash; *p; p++) if (!g_ascii_isxdigit(*p)) valid = FALSE;
    if (valid) return g_strdup_printf("https://textures.minecraft.net/texture/%s", hash);
  }
  return NULL;
}

static guint32
png_uint32(const guchar *data)
{
  return ((guint32)data[0] << 24) | ((guint32)data[1] << 16) |
    ((guint32)data[2] << 8) | (guint32)data[3];
}

static guint32
png_crc32(const guchar *data, gsize size)
{
  guint32 crc = 0xffffffff;
  for (gsize i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return crc ^ 0xffffffff;
}

/* Walk the chunk structure and verify every chunk CRC up to IEND. Image
 * decoders differ in how much they validate (some accept corrupt CRCs), so
 * the app-side gate must be deterministic across decoder versions. */
static gboolean
png_chunks_valid(const guchar *data, gsize size)
{
  gsize offset = 8;
  while (offset + 12 <= size) {
    guint32 length = png_uint32(data + offset);
    if (length > size - offset - 12) return FALSE;
    guint32 stored = png_uint32(data + offset + 8 + length);
    if (png_crc32(data + offset + 4, length + 4) != stored) return FALSE;
    if (memcmp(data + offset + 4, "IEND", 4) == 0) return TRUE;
    offset += 12 + length;
  }
  return FALSE;
}

static GdkPixbuf *
decode_head_png(GBytes *bytes, gboolean allow_legacy)
{
  static const guchar signature[] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
  if (bytes == NULL) return NULL;
  gsize size;
  const guchar *data = g_bytes_get_data(bytes, &size);
  if (size < 33 || size > HEAD_MAX_PNG_BYTES ||
      memcmp(data, signature, sizeof signature) != 0 ||
      png_uint32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) != 0) return NULL;
  guint32 width = png_uint32(data + 16);
  guint32 height = png_uint32(data + 20);
  /* Validate the mandatory first PNG chunk before giving any data to a
   * decoder: a small compressed response can otherwise allocate a huge image.
   * Never downscale an invalid size into an accepted skin or cached avatar.
   * Only 8-bit truecolor PNGs (RGB or RGBA, no interlacing) are accepted;
   * the texture wrapper assumes exactly that layout. */
  if (width != 64 || (height != 64 && !(allow_legacy && height == 32)) ||
      data[24] != 8 || (data[25] != 2 && data[25] != 6) ||
      data[26] != 0 || data[27] != 0 || data[28] != 0) return NULL;
  /* Decoders disagree on how much of the rest they validate: verify every
   * chunk CRC ourselves so malformed input is rejected on every platform. */
  if (!png_chunks_valid(data, size)) return NULL;
  g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new_with_type("png", NULL);
  if (loader == NULL) return NULL;
  g_autoptr(GError) loader_error = NULL;
  gboolean written = gdk_pixbuf_loader_write(loader, data, size, &loader_error);
  gboolean closed = written && gdk_pixbuf_loader_close(loader, &loader_error);
  if (!written || !closed) {
    if (loader_error != NULL)
      g_debug("Rejected head PNG: %s", loader_error->message);
    return NULL;
  }
  GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
  if (pixbuf == NULL || gdk_pixbuf_get_width(pixbuf) != (int)width ||
      gdk_pixbuf_get_height(pixbuf) != (int)height) return NULL;
  /* The texture wrapper assumes 8-bit RGB(A) rows; loaders normally normalize
   * every PNG there, but never wrap a buffer with a different layout. */
  if (gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
      gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB) return NULL;
  return g_object_ref(pixbuf);
}

static GdkTexture *
head_texture_from_pixbuf(GdkPixbuf *pixbuf)
{
  g_autoptr(GBytes) pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
  return GDK_TEXTURE(gdk_memory_texture_new(64, 64,
    gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
    pixels, (gsize)gdk_pixbuf_get_rowstride(pixbuf)));
}

GdkTexture *
pumpkin_player_head_from_png(GBytes *bytes)
{
  g_autoptr(GdkPixbuf) pixbuf = decode_head_png(bytes, FALSE);
  return pixbuf != NULL ? head_texture_from_pixbuf(pixbuf) : NULL;
}

GdkTexture *
pumpkin_player_head_from_file(const char *path, goffset known_size, GError **error)
{
  if (path == NULL) return NULL;
  /* known_size is a hint from the caller's stat: allocate for the expected
   * file, still capped, and read one byte past the limit instead of trusting a
   * racy stat or loading an unbounded cache file in full. */
  gsize limit = known_size > 0 && known_size < HEAD_MAX_PNG_BYTES
    ? (gsize)known_size : HEAD_MAX_PNG_BYTES;
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, error);
  if (stream == NULL) return NULL;
  g_autofree guchar *data = g_malloc(limit + 1);
  gsize size;
  if (!g_input_stream_read_all(G_INPUT_STREAM(stream), data, limit + 1,
                               &size, NULL, error) || size > limit) return NULL;
  g_autoptr(GBytes) bytes = g_bytes_new_take(g_steal_pointer(&data), size);
  return pumpkin_player_head_from_png(bytes);
}

GdkTexture *
pumpkin_player_head_from_skin(GBytes *bytes)
{
  g_autoptr(GdkPixbuf) skin = decode_head_png(bytes, TRUE);
  if (skin == NULL) return NULL;
  g_autoptr(GdkPixbuf) face = gdk_pixbuf_new_subpixbuf(skin, 8, 8, 8, 8);
  g_autoptr(GdkPixbuf) result = gdk_pixbuf_copy(face);
  g_autoptr(GdkPixbuf) hat = gdk_pixbuf_new_subpixbuf(skin, 40, 8, 8, 8);
  if (gdk_pixbuf_get_has_alpha(skin))
    gdk_pixbuf_composite(hat, result, 0, 0, 8, 8, 0, 0, 1, 1, GDK_INTERP_NEAREST, 255);
  g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_scale_simple(result, 64, 64, GDK_INTERP_NEAREST);
  return head_texture_from_pixbuf(scaled);
}

static void
finish_request(HeadRequest *request, GdkTexture *texture)
{
  if (texture != NULL) {
    gdk_texture_save_to_png(texture, request->path);
    if (request->verified_java) {
      g_autofree char *marker = g_strconcat(request->path, ".java", NULL);
      g_file_set_contents(marker, "verified", -1, NULL);
    }
    g_hash_table_remove(retry_after, request->identifier);
  } else {
    gint64 *deadline = g_new(gint64, 1);
    *deadline = g_get_monotonic_time() + 5 * 60 * G_USEC_PER_SEC;
    g_hash_table_replace(retry_after, g_strdup(request->identifier), deadline);
  }
  for (guint i = 0; i < request->images->len; i++) {
    g_autoptr(GtkImage) image = g_weak_ref_get(g_ptr_array_index(request->images, i));
    if (image == NULL) continue;
    g_object_set_data(G_OBJECT(image), "verified-java-profile", GINT_TO_POINTER(request->verified_java));
    if (texture != NULL) gtk_image_set_from_paintable(image, GDK_PAINTABLE(texture));
    else if (request->verified_java) g_object_notify(G_OBJECT(image), "paintable");
  }
  g_hash_table_remove(requests, request->identifier);
  g_ptr_array_unref(request->images);
  g_object_unref(request->message);
  g_free(request->identifier);
  g_free(request->path);
  g_free(request);
}

static void on_head_loaded(GObject *source, GAsyncResult *result, gpointer data);

static void
send_request(HeadRequest *request, const char *url)
{
  g_clear_object(&request->message);
  request->message = soup_message_new("GET", url);
  soup_session_send_and_read_async(session, request->message, G_PRIORITY_DEFAULT, NULL,
                                   on_head_loaded, request);
}

static void
on_head_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  HeadRequest *request = data;
  g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL);
  g_autoptr(GdkTexture) texture = NULL;
  if (soup_message_get_status(request->message) == SOUP_STATUS_OK &&
      bytes != NULL && g_bytes_get_size(bytes) <= HEAD_MAX_PNG_BYTES) {
    if (request->stage == HEAD_PROFILE) {
      gsize size;
      const char *contents = g_bytes_get_data(bytes, &size);
      g_autofree char *json = g_strndup(contents, size);
      g_autofree char *url = pumpkin_player_skin_url(json, request->identifier);
      if (url != NULL) {
        request->verified_java = TRUE;
        request->stage = HEAD_SKIN;
        send_request(request, url);
        return;
      }
    } else if (request->stage == HEAD_SKIN) {
      texture = pumpkin_player_head_from_skin(bytes);
    } else {
      texture = pumpkin_player_head_from_png(bytes);
    }
  }
  finish_request(request, texture);
}

void
pumpkin_player_head_set(GtkImage *image, const char *uuid, const char *name,
                        gboolean java, gboolean bedrock)
{
  (void)name;
  (void)java;
  g_object_set_data(G_OBJECT(image), "verified-java-profile", NULL);
  gtk_image_set_from_icon_name(image, "avatar-default-symbolic");
  if (uuid == NULL || !g_uuid_string_is_valid(uuid)) return;
  gboolean floodgate = g_str_has_prefix(uuid, "00000000-0000-0000-");
  if (bedrock && !floodgate) return;
  g_autofree char *identifier = g_ascii_strdown(uuid, -1);
  /* Do not reuse third-party default Steve images cached by v1. */
  g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "smashed-pumpkin", "player-heads-v2", NULL);
  if (g_mkdir_with_parents(dir, 0700) != 0) return;
  g_autofree char *filename = g_strdup_printf("%s.png", identifier);
  g_autofree char *path = g_build_filename(dir, filename, NULL);
  GStatBuf statbuf;
  if (g_stat(path, &statbuf) == 0 && statbuf.st_size <= HEAD_MAX_PNG_BYTES) {
    g_autoptr(GError) cache_error = NULL;
    g_autoptr(GdkTexture) cached = pumpkin_player_head_from_file(path, statbuf.st_size, &cache_error);
    if (cache_error != NULL)
      g_debug("Ignoring cached head %s: %s", filename, cache_error->message);
    if (cached != NULL) {
      g_autofree char *marker = g_strconcat(path, ".java", NULL);
      g_object_set_data(G_OBJECT(image), "verified-java-profile",
        GINT_TO_POINTER(!floodgate && g_file_test(marker, G_FILE_TEST_IS_REGULAR)));
      gtk_image_set_from_paintable(image, GDK_PAINTABLE(cached));
      if (time(NULL) - statbuf.st_mtime < 24 * 60 * 60) return;
    }
  }
  if (session == NULL) {
    session = soup_session_new_with_options("timeout", 10, "user-agent", "SmashedPumpkin/PlayerHeads", NULL);
    requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    retry_after = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  }
  /* Failed identifiers that are never visited again would otherwise stay in
   * retry_after forever; drop every entry whose deadline has passed. */
  gint64 now = g_get_monotonic_time();
  GHashTableIter iter; gpointer iter_key, iter_value;
  g_hash_table_iter_init(&iter, retry_after);
  while (g_hash_table_iter_next(&iter, &iter_key, &iter_value)) {
    if (*(gint64 *)iter_value <= now) g_hash_table_iter_remove(&iter);
  }
  gint64 *deadline = g_hash_table_lookup(retry_after, identifier);
  if (deadline != NULL && g_get_monotonic_time() < *deadline) return;
  HeadRequest *request = g_hash_table_lookup(requests, identifier);
  gboolean start_request = request == NULL;
  if (start_request) {
    request = g_new0(HeadRequest, 1);
    request->identifier = g_strdup(identifier);
    request->path = g_strdup(path);
    request->images = g_ptr_array_new_with_free_func(weak_image_free);
    request->stage = floodgate ? HEAD_FLOODGATE : HEAD_PROFILE;
    g_hash_table_insert(requests, g_strdup(identifier), request);
  }
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, image);
  g_ptr_array_add(request->images, weak);
  if (start_request) {
    g_autofree char *url = floodgate
      ? g_strdup_printf("https://mc-heads.net/avatar/%s/64", identifier)
      : g_strdup_printf("https://sessionserver.mojang.com/session/minecraft/profile/%s", identifier);
    send_request(request, url);
  }
}
