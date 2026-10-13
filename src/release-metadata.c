#include "release-metadata.h"
#include "vendor/cjson/cJSON.h"
#include <math.h>
#include <string.h>

static const char *
string_value(const cJSON *object, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(value) ? value->valuestring : NULL;
}

void
pumpkin_resolved_download_free(PumpkinResolvedDownload *resolved)
{
  if (resolved == NULL) {
    return;
  }
  g_free(resolved->url);
  g_free(resolved->build_id);
  g_free(resolved->build_label);
  g_free(resolved);
}

static gboolean
valid_digest(const char *digest)
{
  if (digest == NULL || !g_str_has_prefix(digest, "sha256:") || strlen(digest) != 71) {
    return FALSE;
  }
  for (guint i = 7; i < 71; ++i) {
    if (!g_ascii_isxdigit(digest[i])) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean
valid_tag(const char *tag)
{
  if (tag == NULL || *tag == '\0' || strlen(tag) > 128) {
    return FALSE;
  }
  for (const char *p = tag; *p != '\0'; ++p) {
    if (!g_ascii_isalnum(*p) && strchr(".-_+", *p) == NULL) {
      return FALSE;
    }
  }
  return !g_str_equal(tag, ".") && !g_str_equal(tag, "..");
}

PumpkinResolvedDownload *
pumpkin_release_parse_metadata(const char *json, PumpkinReleaseChannel channel,
                                                       const char *asset_name, GError **error)
{
  cJSON *root = json != NULL ? cJSON_ParseWithOpts(json, NULL, TRUE) : NULL;
  const char *tag = string_value(root, "tag_name");
  const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
  gboolean valid = (channel == PUMPKIN_RELEASE_NIGHTLY || channel == PUMPKIN_RELEASE_STABLE) &&
    cJSON_IsObject(root) && valid_tag(tag) &&
    cJSON_IsArray(assets) && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "draft"));
  if (valid) {
    valid = channel == PUMPKIN_RELEASE_NIGHTLY ? g_str_equal(tag, "nightly") :
      !g_str_equal(tag, "nightly") && !g_str_equal(tag, "canary") &&
      cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "prerelease"));
  }
  if (!valid || asset_name == NULL || *asset_name == '\0') {
    cJSON_Delete(root);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "GitHub returned invalid or mismatched Pumpkin release metadata");
    return NULL;
  }
  const cJSON *asset = NULL;
  cJSON_ArrayForEach(asset, assets) {
    if (g_strcmp0(string_value(asset, "name"), asset_name) != 0) {
      continue;
    }
    const char *url = string_value(asset, "browser_download_url");
    const char *updated = string_value(asset, "updated_at");
    const char *digest = string_value(asset, "digest");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(asset, "id");
    g_autofree char *escaped_tag = g_uri_escape_string(tag, NULL, FALSE);
    g_autofree char *expected = g_strdup_printf("https://github.com/Pumpkin-MC/Pumpkin/releases/download/%s/%s", escaped_tag, asset_name);
    g_autofree char *unescaped = g_strdup_printf("https://github.com/Pumpkin-MC/Pumpkin/releases/download/%s/%s", tag, asset_name);
    g_autoptr(GDateTime) date = updated != NULL ? g_date_time_new_from_iso8601(updated, NULL) : NULL;
    if ((g_strcmp0(url, expected) != 0 && g_strcmp0(url, unescaped) != 0) || date == NULL ||
        g_strcmp0(string_value(asset, "state"), "uploaded") != 0 ||
        !cJSON_IsNumber(id) || !isfinite(id->valuedouble) || id->valuedouble < 1 ||
        id->valuedouble > 9007199254740991.0 || floor(id->valuedouble) != id->valuedouble) {
      cJSON_Delete(root);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Pumpkin release asset has an invalid URL, state or build identity");
      return NULL;
    }
    PumpkinResolvedDownload *resolved = g_new0(PumpkinResolvedDownload, 1);
    resolved->url = g_strdup(url);
    /* Rolling Nightly releases reuse their tag and published_at date. */
    resolved->build_id = valid_digest(digest) ? g_ascii_strdown(digest, -1) :
      g_strdup_printf("github:%s:%.0f:%s", tag, id->valuedouble, updated);
    g_autoptr(GDateTime) utc = g_date_time_to_utc(date);
    g_autofree char *formatted = g_date_time_format(utc, "%Y-%m-%d %H:%M UTC");
    /* Legacy metadata grammar used by persistence/cache identities. These
     * prefixes must remain stable; translate only a separately formatted label. */
    resolved->build_label = channel == PUMPKIN_RELEASE_STABLE ? g_strdup_printf("Stable %s", tag) :
      g_strdup_printf("Nightly %s", formatted);
    cJSON_Delete(root);
    return resolved;
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Pumpkin %s (%s) has no %s download for this platform",
    pumpkin_release_channel_id(channel), tag, asset_name);
  cJSON_Delete(root);
  return NULL;
}
