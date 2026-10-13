#include "player-heads.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
  int height;
  gboolean alpha;
} SkinCase;

static GBytes *
pixbuf_bytes(GdkPixbuf *pixbuf, const char *format)
{
  char *buffer = NULL;
  gsize size;
  g_autoptr(GError) error = NULL;
  g_assert_true(gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &size, format, &error, NULL));
  g_assert_no_error(error);
  return g_bytes_new_take(buffer, size);
}

static GdkTexture *
reference_texture(GdkPixbuf *pixbuf)
{
  g_autoptr(GBytes) pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
  return GDK_TEXTURE(gdk_memory_texture_new(64, 64,
    gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
    pixels, (gsize)gdk_pixbuf_get_rowstride(pixbuf)));
}

static void
assert_same_texture(GdkTexture *actual, GdkTexture *expected)
{
  g_assert_nonnull(actual);
  g_assert_nonnull(expected);
  g_assert_cmpint(gdk_texture_get_width(actual), ==, 64);
  g_assert_cmpint(gdk_texture_get_height(actual), ==, 64);
  g_assert_cmpint(gdk_texture_get_format(actual), ==, gdk_texture_get_format(expected));
  guchar actual_pixels[64 * 64 * 4];
  guchar expected_pixels[64 * 64 * 4];
  gdk_texture_download(actual, actual_pixels, 64 * 4);
  gdk_texture_download(expected, expected_pixels, 64 * 4);
  g_assert_cmpmem(actual_pixels, sizeof actual_pixels, expected_pixels, sizeof expected_pixels);
}

static GdkTexture *
cache_texture(GBytes *bytes)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *dir = g_dir_make_tmp("smashed-player-heads-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(dir);
  g_autofree char *path = g_build_filename(dir, "head.png", NULL);
  gsize size;
  const char *data = g_bytes_get_data(bytes, &size);
  g_assert_true(g_file_set_contents(path, data, size, &error));
  g_assert_no_error(error);
  GStatBuf statbuf;
  g_assert_cmpint(g_stat(path, &statbuf), ==, 0);
  GdkTexture *texture = pumpkin_player_head_from_file(path, statbuf.st_size, NULL);
  g_assert_cmpint(g_remove(path), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  return texture;
}

static void
assert_rejected(GBytes *bytes)
{
  g_assert_null(pumpkin_player_head_from_skin(bytes));
  g_assert_null(pumpkin_player_head_from_png(bytes));
  g_assert_null(cache_texture(bytes));
}

static GdkPixbuf *
make_skin(int height, gboolean alpha)
{
  GdkPixbuf *skin = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, 64, height);
  gdk_pixbuf_fill(skin, 0x5b6271ff);
  int channels = gdk_pixbuf_get_n_channels(skin);
  int stride = gdk_pixbuf_get_rowstride(skin);
  guchar *pixels = gdk_pixbuf_get_pixels(skin);
  const guchar hat_alpha[] = { 0, 128, 255, 64 };
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      guchar *face = pixels + (y + 8) * stride + (x + 8) * channels;
      face[0] = 20 + 13 * x;
      face[1] = 40 + 11 * y;
      face[2] = 60 + 7 * (x + y);
      if (alpha) face[3] = 255;
      guchar *hat = pixels + (y + 8) * stride + (x + 40) * channels;
      hat[0] = 200 - 7 * x;
      hat[1] = 10 + 5 * y;
      hat[2] = 240 - 3 * y - 4 * x;
      if (alpha) hat[3] = hat_alpha[(x + 2 * y) % G_N_ELEMENTS(hat_alpha)];
    }
  }
  return skin;
}

static GdkTexture *
reference_skin_head(GdkPixbuf *skin)
{
  /* The previous crop/composite/nearest pipeline, on a known small fixture. */
  g_autoptr(GdkPixbuf) face = gdk_pixbuf_new_subpixbuf(skin, 8, 8, 8, 8);
  g_autoptr(GdkPixbuf) result = gdk_pixbuf_copy(face);
  g_autoptr(GdkPixbuf) hat = gdk_pixbuf_new_subpixbuf(skin, 40, 8, 8, 8);
  if (gdk_pixbuf_get_has_alpha(skin))
    gdk_pixbuf_composite(hat, result, 0, 0, 8, 8, 0, 0, 1, 1, GDK_INTERP_NEAREST, 255);
  g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_scale_simple(result, 64, 64, GDK_INTERP_NEAREST);
  return reference_texture(scaled);
}

static void
assert_pixel(GdkPixbuf *pixbuf, int x, int y, guchar red, guchar green, guchar blue)
{
  const guchar *pixel = gdk_pixbuf_read_pixels(pixbuf) +
    y * gdk_pixbuf_get_rowstride(pixbuf) + x * gdk_pixbuf_get_n_channels(pixbuf);
  g_assert_cmpuint(pixel[0], ==, red);
  g_assert_cmpuint(pixel[1], ==, green);
  g_assert_cmpuint(pixel[2], ==, blue);
  if (gdk_pixbuf_get_has_alpha(pixbuf)) g_assert_cmpuint(pixel[3], ==, 255);
}

static void
test_skin_pixels(gconstpointer data)
{
  const SkinCase *test = data;
  g_autoptr(GdkPixbuf) skin = make_skin(test->height, test->alpha);
  g_autoptr(GBytes) png = pixbuf_bytes(skin, "png");
  g_autoptr(GdkTexture) head = pumpkin_player_head_from_skin(png);
  g_autoptr(GdkTexture) expected = reference_skin_head(skin);
  assert_same_texture(head, expected);
  g_autoptr(GBytes) output = gdk_texture_save_to_png_bytes(head);
  g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(output);
  g_autoptr(GdkPixbuf) decoded = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
  g_assert_nonnull(decoded);
  /* Transparent hat preserves the face; opaque hat replaces it. RGB skins
   * retain the existing rule of ignoring the hat rather than overlaying it. */
  assert_pixel(decoded, 0, 0, 20, 40, 60);
  assert_pixel(decoded, 7, 7, 20, 40, 60);
  if (test->alpha) {
    assert_pixel(decoded, 8, 0, 113, 24, 151);
    assert_pixel(decoded, 16, 0, 186, 10, 232);
    assert_pixel(decoded, 23, 7, 186, 10, 232);
  } else {
    assert_pixel(decoded, 8, 0, 33, 40, 67);
    assert_pixel(decoded, 16, 0, 46, 40, 74);
  }
}

static GdkPixbuf *
make_avatar(gboolean alpha)
{
  GdkPixbuf *avatar = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, 64, 64);
  int stride = gdk_pixbuf_get_rowstride(avatar);
  int channels = gdk_pixbuf_get_n_channels(avatar);
  guchar *pixels = gdk_pixbuf_get_pixels(avatar);
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 64; x++) {
      guchar *pixel = pixels + y * stride + x * channels;
      pixel[0] = x * 3;
      pixel[1] = y * 3;
      pixel[2] = (x + y) * 2;
      if (alpha) pixel[3] = ((x + y) % 4) * 85;
    }
  }
  return avatar;
}

static void
test_avatar_pixels(gconstpointer data)
{
  gboolean alpha = GPOINTER_TO_INT(data);
  g_autoptr(GdkPixbuf) avatar = make_avatar(alpha);
  g_autoptr(GBytes) png = pixbuf_bytes(avatar, "png");
  g_autoptr(GdkTexture) expected = reference_texture(avatar);
  g_autoptr(GdkTexture) floodgate = pumpkin_player_head_from_png(png);
  g_autoptr(GdkTexture) cached = cache_texture(png);
  assert_same_texture(floodgate, expected);
  assert_same_texture(cached, expected);
  /* Pin the production format contract instead of re-deriving it here. */
  g_assert_cmpint(gdk_texture_get_format(floodgate), ==,
                  alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8);
  g_assert_cmpint(gdk_texture_get_format(cached), ==,
                  alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8);
}

static void
test_legacy_avatar_rejected(void)
{
  g_autoptr(GdkPixbuf) skin = make_skin(32, TRUE);
  g_autoptr(GBytes) png = pixbuf_bytes(skin, "png");
  g_autoptr(GdkTexture) head = pumpkin_player_head_from_skin(png);
  g_assert_nonnull(head);
  g_assert_null(pumpkin_player_head_from_png(png));
  g_assert_null(cache_texture(png));
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

static void
write_uint32(guchar *data, guint32 value)
{
  data[0] = value >> 24;
  data[1] = value >> 16;
  data[2] = value >> 8;
  data[3] = value;
}

static void
append_chunk(GByteArray *png, const char *type, const guchar *data, guint32 size)
{
  gsize start = png->len;
  guchar number[4];
  write_uint32(number, size);
  g_byte_array_append(png, number, sizeof number);
  g_byte_array_append(png, (const guchar *)type, 4);
  if (size != 0) g_byte_array_append(png, data, size);
  write_uint32(number, png_crc32(png->data + start + 4, size + 4));
  g_byte_array_append(png, number, sizeof number);
}

static GBytes *
png_with_dimensions(GBytes *source, guint32 width, guint32 height)
{
  gsize size;
  const guchar *original = g_bytes_get_data(source, &size);
  guchar *data = g_memdup2(original, size);
  write_uint32(data + 16, width);
  write_uint32(data + 20, height);
  write_uint32(data + 29, png_crc32(data + 12, 17));
  return g_bytes_new_take(data, size);
}

static void
test_bad_formats(void)
{
  g_assert_null(pumpkin_player_head_from_skin(NULL));
  g_assert_null(pumpkin_player_head_from_png(NULL));
  g_assert_null(pumpkin_player_head_from_file(NULL, 0, NULL));
  g_autoptr(GBytes) invalid = g_bytes_new_static("not-png", 7);
  assert_rejected(invalid);
  const char svg[] = "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'/>";
  g_autoptr(GBytes) vector = g_bytes_new_static(svg, sizeof svg - 1);
  assert_rejected(vector);
  g_autoptr(GdkPixbuf) avatar = make_avatar(FALSE);
  g_autoptr(GBytes) jpeg = pixbuf_bytes(avatar, "jpeg");
  g_autoptr(GBytes) bmp = pixbuf_bytes(avatar, "bmp");
  assert_rejected(jpeg);
  assert_rejected(bmp);
  g_autoptr(GError) error = NULL;
  g_autofree char *dir = g_dir_make_tmp("smashed-player-heads-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree char *missing = g_build_filename(dir, "missing.png", NULL);
  g_assert_null(pumpkin_player_head_from_file(missing, 0, NULL));
  g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void
test_png_header_errors(void)
{
  g_autoptr(GdkPixbuf) avatar = make_avatar(TRUE);
  g_autoptr(GBytes) png = pixbuf_bytes(avatar, "png");
  gsize size;
  const guchar *original = g_bytes_get_data(png, &size);
  for (gsize length = 0; length < 33; length++) {
    g_autoptr(GBytes) truncated = g_bytes_new(original, length);
    g_assert_null(pumpkin_player_head_from_skin(truncated));
    g_assert_null(pumpkin_player_head_from_png(truncated));
  }
  const gsize lengths[] = { 33, size / 2 };
  for (guint i = 0; i < G_N_ELEMENTS(lengths); i++) {
    g_autoptr(GBytes) truncated = g_bytes_new(original, lengths[i]);
    assert_rejected(truncated);
  }
  for (int test = 0; test < 6; test++) {
    guchar *data = g_memdup2(original, size);
    switch (test) {
    case 0: data[0] = 0; break;
    case 1: write_uint32(data + 8, 12); break;
    case 2: write_uint32(data + 8, 0xffffffff); break;
    case 3: memcpy(data + 12, "IDAT", 4); break;
    case 4: data[29] ^= 1; break;
    case 5:
      data[24] = 0;
      write_uint32(data + 29, png_crc32(data + 12, 17));
      break;
    }
    g_autoptr(GBytes) invalid = g_bytes_new_take(data, size);
    assert_rejected(invalid);
  }
}

static void
test_dimension_limits(void)
{
  const guint32 dimensions[][2] = {
    { 0, 64 }, { 64, 0 }, { 1, 1 }, { 32, 32 }, { 63, 64 }, { 65, 64 },
    { 64, 31 }, { 64, 33 }, { 64, 63 }, { 64, 65 }, { 128, 64 }, { 64, 128 },
    { 2048, 2048 }, { 0x80000000, 64 }, { 64, 0x80000000 }, { 0xffffffff, 0xffffffff }
  };
  g_autoptr(GdkPixbuf) avatar = make_avatar(TRUE);
  g_autoptr(GBytes) png = pixbuf_bytes(avatar, "png");
  for (guint i = 0; i < G_N_ELEMENTS(dimensions); i++) {
    g_autoptr(GBytes) invalid = png_with_dimensions(png, dimensions[i][0], dimensions[i][1]);
    assert_rejected(invalid);
  }
}

static GBytes *
png_with_byte_size(GBytes *source, gsize target_size)
{
  gsize size;
  const guchar *original = g_bytes_get_data(source, &size);
  g_assert_cmpuint(target_size, >, size + 12);
  gsize padding_size = target_size - size - 12;
  g_autofree guchar *padding = g_malloc0(padding_size);
  g_autoptr(GByteArray) png = g_byte_array_new();
  g_byte_array_append(png, original, 33);
  /* A valid, private ancillary chunk exercises the exact input byte limit. */
  append_chunk(png, "npAD", padding, padding_size);
  g_byte_array_append(png, original + 33, size - 33);
  return g_byte_array_free_to_bytes(g_steal_pointer(&png));
}

static void
test_byte_limits(void)
{
  g_autoptr(GdkPixbuf) skin = make_skin(64, TRUE);
  g_autoptr(GBytes) png = pixbuf_bytes(skin, "png");
  g_autoptr(GBytes) boundary = png_with_byte_size(png, HEAD_MAX_PNG_BYTES);
  g_assert_cmpuint(g_bytes_get_size(boundary), ==, HEAD_MAX_PNG_BYTES);
  g_autoptr(GdkTexture) head = pumpkin_player_head_from_skin(boundary);
  g_autoptr(GdkTexture) expected_head = reference_skin_head(skin);
  assert_same_texture(head, expected_head);
  g_autoptr(GdkTexture) expected_avatar = reference_texture(skin);
  g_autoptr(GdkTexture) floodgate = pumpkin_player_head_from_png(boundary);
  g_autoptr(GdkTexture) cached = cache_texture(boundary);
  assert_same_texture(floodgate, expected_avatar);
  assert_same_texture(cached, expected_avatar);
  g_autoptr(GBytes) too_large = png_with_byte_size(png, HEAD_MAX_PNG_BYTES + 1);
  assert_rejected(too_large);
}

static GBytes *
compressed_oversize_png(void)
{
  /* Produce a genuine 2048x2048 RGBA PNG with a single row buffer instead of
   * allocating its 16 MiB raster in the test itself. */
  static const guchar signature[] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
  guchar header[13] = { 0 };
  write_uint32(header, 2048);
  write_uint32(header + 4, 2048);
  header[8] = 8;
  header[9] = 6;
  g_autoptr(GOutputStream) memory = g_memory_output_stream_new_resizable();
  g_autoptr(GZlibCompressor) compressor = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 9);
  g_autoptr(GOutputStream) stream = g_converter_output_stream_new(memory, G_CONVERTER(compressor));
  guchar row[2048 * 4 + 1] = { 0 };
  for (int x = 0; x < 2048; x++) {
    row[1 + x * 4] = 17;
    row[2 + x * 4] = 34;
    row[3 + x * 4] = 51;
    row[4 + x * 4] = 255;
  }
  g_autoptr(GError) error = NULL;
  for (int y = 0; y < 2048; y++) {
    g_assert_true(g_output_stream_write_all(stream, row, sizeof row, NULL, NULL, &error));
    g_assert_no_error(error);
  }
  g_assert_true(g_output_stream_close(stream, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GBytes) compressed = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(memory));
  gsize size;
  const guchar *data = g_bytes_get_data(compressed, &size);
  g_autoptr(GByteArray) png = g_byte_array_new();
  g_byte_array_append(png, signature, sizeof signature);
  append_chunk(png, "IHDR", header, sizeof header);
  append_chunk(png, "IDAT", data, size);
  append_chunk(png, "IEND", NULL, 0);
  return g_byte_array_free_to_bytes(g_steal_pointer(&png));
}

static void
test_compressed_oversize(void)
{
  g_autoptr(GBytes) png = compressed_oversize_png();
  g_assert_cmpuint(g_bytes_get_size(png), <, HEAD_MAX_PNG_BYTES);
  g_test_message("2048x2048 RGBA raster: 16777216 bytes; compressed PNG: %" G_GSIZE_FORMAT " bytes",
    g_bytes_get_size(png));
  assert_rejected(png);
}

static gboolean
record_size_prepared(GSignalInvocationHint *hint, guint n_values, const GValue *values,
                     gpointer data)
{
  (void)hint;
  g_assert_cmpuint(n_values, ==, 3);
  g_assert_cmpint(g_value_get_int(&values[1]), >, 0);
  g_assert_cmpint(g_value_get_int(&values[2]), >, 0);
  guint *prepared = data;
  (*prepared)++;
  return TRUE;
}

static void
test_preallocation_limit(void)
{
  g_autoptr(GdkPixbuf) avatar = make_avatar(TRUE);
  g_autoptr(GBytes) small = pixbuf_bytes(avatar, "png");
  g_autoptr(GBytes) large = compressed_oversize_png();
  g_autoptr(GdkTexture) warm = pumpkin_player_head_from_png(small);
  g_assert_nonnull(warm);
  guint signal = g_signal_lookup("size-prepared", GDK_TYPE_PIXBUF_LOADER);
  g_assert_cmpuint(signal, !=, 0);
  guint prepared = 0;
  gulong hook = g_signal_add_emission_hook(signal, 0, record_size_prepared, &prepared, NULL);
  g_assert_cmpuint(hook, !=, 0);
  /* Confirm each production entry uses the observable PNG loader. An RSS
   * threshold cannot reliably catch allocations made by isolated decoders. */
  g_autoptr(GdkTexture) skin = pumpkin_player_head_from_skin(small);
  g_assert_nonnull(skin);
  g_assert_cmpuint(prepared, ==, 1);
  g_autoptr(GdkTexture) floodgate = pumpkin_player_head_from_png(small);
  g_assert_nonnull(floodgate);
  g_assert_cmpuint(prepared, ==, 2);
  g_autoptr(GdkTexture) cached = cache_texture(small);
  g_assert_nonnull(cached);
  g_assert_cmpuint(prepared, ==, 3);
  prepared = 0;
  assert_rejected(large);
  g_signal_remove_emission_hook(signal, hook);
  g_test_message("Oversize PNG loader size-prepared emissions: %u", prepared);
  /* A decode-then-reject implementation still returns NULL, but fails here. */
  g_assert_cmpuint(prepared, ==, 0);
}

static char *
profile_for_url(const char *url)
{
  g_autofree char *payload = g_strdup_printf("{\"textures\":{\"SKIN\":{\"url\":\"%s\"}}}", url);
  g_autofree char *encoded = g_base64_encode((const guchar *)payload, strlen(payload));
  return g_strdup_printf("{\"id\":\"faf39d189aae42949289ba3836ccb271\","
    "\"properties\":[{\"name\":\"textures\",\"value\":\"%s\"}]}", encoded);
}

static void
test_profile_validation(void)
{
  const char *uuid = "faf39d18-9aae-4294-9289-ba3836ccb271";
  const char *hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *prefixes[] = { "http://textures.minecraft.net/texture/", "https://textures.minecraft.net/texture/" };
  g_autofree char *expected = g_strconcat(prefixes[1], hash, NULL);
  for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++) {
    g_autofree char *input = g_strconcat(prefixes[i], hash, NULL);
    g_autofree char *profile = profile_for_url(input);
    g_autofree char *url = pumpkin_player_skin_url(profile, uuid);
    g_assert_cmpstr(url, ==, expected);
    g_assert_null(pumpkin_player_skin_url(profile, "069a79f4-44e9-4726-a5be-fca90e38aaf5"));
  }
  const char *invalid[] = {
    "https://evil.invalid/texture/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "https://textures.minecraft.net.evil.invalid/texture/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "https://textures.minecraft.net/texture/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa?x=1",
    "https://textures.minecraft.net/texture/short",
    "https://textures.minecraft.net/texture/gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  };
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_autofree char *profile = profile_for_url(invalid[i]);
    g_assert_null(pumpkin_player_skin_url(profile, uuid));
  }
  g_assert_null(pumpkin_player_skin_url("malformed", uuid));
  g_assert_null(pumpkin_player_skin_url(NULL, uuid));
  g_assert_null(pumpkin_player_skin_url("{}", NULL));
}

int
main(int argc, char **argv)
{
  /* Deliberately no gtk_init(), GtkImage, SoupSession or external requests. */
  g_test_init(&argc, &argv, NULL);
  static const SkinCase skins[] = { { 64, TRUE }, { 32, TRUE }, { 64, FALSE }, { 32, FALSE } };
  g_test_add_data_func("/player-heads/skin/modern-rgba", &skins[0], test_skin_pixels);
  g_test_add_data_func("/player-heads/skin/legacy-rgba", &skins[1], test_skin_pixels);
  g_test_add_data_func("/player-heads/skin/modern-rgb", &skins[2], test_skin_pixels);
  g_test_add_data_func("/player-heads/skin/legacy-rgb", &skins[3], test_skin_pixels);
  g_test_add_data_func("/player-heads/avatar/rgba", GINT_TO_POINTER(TRUE), test_avatar_pixels);
  g_test_add_data_func("/player-heads/avatar/rgb", GINT_TO_POINTER(FALSE), test_avatar_pixels);
  g_test_add_func("/player-heads/avatar/reject-legacy-size", test_legacy_avatar_rejected);
  g_test_add_func("/player-heads/png/bad-formats", test_bad_formats);
  g_test_add_func("/player-heads/png/header-errors", test_png_header_errors);
  g_test_add_func("/player-heads/png/dimensions", test_dimension_limits);
  g_test_add_func("/player-heads/png/byte-limit", test_byte_limits);
  g_test_add_func("/player-heads/png/compressed-oversize", test_compressed_oversize);
  g_test_add_func("/player-heads/png/preallocation", test_preallocation_limit);
  g_test_add_func("/player-heads/profile-validation", test_profile_validation);
  return g_test_run();
}
