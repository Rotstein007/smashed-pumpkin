#include "i18n.h"
#include "language.h"
#include "build-metadata.h"
#include "vendor/cjson/cJSON.h"

#include <locale.h>

static gboolean translated;

static void
test_catalog_loading(void)
{
  g_test_message("Messages locale: %s; gettext: %s; GLib: %s",
    setlocale(LC_MESSAGES, NULL), dgettext(GETTEXT_PACKAGE, "Overview"), _("Overview"));
  g_assert_cmpstr(_("Overview"), ==, translated ? "Übersicht" : "Overview");
  g_assert_true(g_utf8_validate(_("Overview"), -1, NULL));
  g_assert_cmpstr(bind_textdomain_codeset(GETTEXT_PACKAGE, NULL), ==, "UTF-8");
  g_assert_cmpstr(bindtextdomain(GETTEXT_PACKAGE, NULL), ==, pumpkin_language_catalog_root());
  g_assert_cmpstr(_("Untranslated test message"), ==, "Untranslated test message");
}

static void
test_context_plural_and_deferred(void)
{
  const char *deferred = N_("Stop");
  g_assert_cmpstr(deferred, ==, "Stop");
  g_assert_cmpstr(_(deferred), ==, translated ? "Anhalten" : "Stop");
  g_assert_cmpstr(_("Open"), ==, translated ? "Öffnen" : "Open");
  g_assert_cmpstr(C_("connection-state", "Open"), ==, translated ? "Offen" : "Open");
  const unsigned int counts[] = {0, 1, 2, 42};
  for (guint i = 0; i < G_N_ELEMENTS(counts); ++i) {
    unsigned int count = counts[i];
    const char *format = g_dngettext(GETTEXT_PACKAGE, "%u server", "%u servers", count);
    g_autofree char *label = g_strdup_printf(format, count);
    g_autofree char *expected = g_strdup_printf(
      translated ? (count == 1 ? "%u Serverinstanz" : "%u Serverinstanzen") :
                   (count == 1 ? "%u server" : "%u servers"), count);
    g_assert_cmpstr(label, ==, expected);
  }
  /* Application lookups must not depend on another library's default domain. */
  textdomain("smashed-pumpkin-unrelated-test-domain");
  g_assert_cmpstr(_("Stop"), ==, translated ? "Anhalten" : "Stop");
  textdomain(GETTEXT_PACKAGE);
}

static void
test_metadata_boundary(void)
{
  g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 10, 7, 8, 8, 39);
  g_autofree char *raw = pumpkin_build_metadata_from_unix(g_date_time_to_unix(date));
  g_assert_cmpstr(raw, ==, "Build 2026-10-07 08:08 UTC");
  g_autofree char *display = pumpkin_build_metadata_format(raw, "%Y-%m-%d %H:%M");
  g_assert_cmpstr(display, ==, translated ? "Erstellt 2026-10-07 08:08" : "Build 2026-10-07 08:08");
  g_assert_true(pumpkin_build_metadata_equal(raw, "2026-10-07 08:08 UTC"));
  g_assert_false(pumpkin_build_metadata_equal(raw, "2026-10-07 08:09 UTC"));
  g_assert_cmpstr(raw, ==, "Build 2026-10-07 08:08 UTC");
  g_autofree char *stable = pumpkin_build_metadata_format("Stable v1.2.3", NULL);
  g_autofree char *nightly = pumpkin_build_metadata_format("Nightly build-id-123", NULL);
  g_assert_cmpstr(stable, ==, translated ? "Stabile Version v1.2.3" : "Stable v1.2.3");
  g_assert_cmpstr(nightly, ==, translated ? "Nächtlicher Build build-id-123" : "Nightly build-id-123");
  g_autofree char *opaque = pumpkin_build_metadata_format("Custom user metadata", NULL);
  g_assert_cmpstr(opaque, ==, "Custom user metadata");
}

static void
test_numeric_wire_formats(void)
{
  g_assert_cmpstr(setlocale(LC_NUMERIC, NULL), ==, "C");
  g_assert_cmpstr(localeconv()->decimal_point, ==, ".");
  g_autofree char *command = g_strdup_printf("limit %.2f", 1.25);
  g_assert_cmpstr(command, ==, "limit 1.25");
  cJSON *json = cJSON_Parse("{\"value\":1.25}");
  g_assert_nonnull(json);
  g_assert_cmpfloat(cJSON_GetObjectItemCaseSensitive(json, "value")->valuedouble, ==, 1.25);
  char *serialized = cJSON_PrintUnformatted(json);
  g_assert_cmpstr(serialized, ==, "{\"value\":1.25}");
  cJSON_free(serialized);
  cJSON_Delete(json);
}

int
main(int argc, char **argv)
{
  pumpkin_i18n_init();
  g_test_init(&argc, &argv, NULL);
  g_assert_cmpint(argc, ==, 2);
  g_assert_true(g_str_equal(argv[1], "translated") || g_str_equal(argv[1], "english") ||
                g_str_equal(argv[1], "missing-catalog"));
  translated = g_str_equal(argv[1], "translated");
  gboolean requires_messages_locale = translated || g_str_equal(argv[1], "missing-catalog");
  if (requires_messages_locale && g_strcmp0(setlocale(LC_MESSAGES, NULL), "C") == 0) {
    g_printerr("The translation integration tests require en_US.UTF-8 (not a German locale).\n");
    return 77;
  }
  g_test_add_func("/i18n/catalog-loading", test_catalog_loading);
  g_test_add_func("/i18n/context-plural-deferred", test_context_plural_and_deferred);
  g_test_add_func("/i18n/raw-metadata-vs-display", test_metadata_boundary);
  g_test_add_func("/i18n/numeric-wire-formats", test_numeric_wire_formats);
  return g_test_run();
}
