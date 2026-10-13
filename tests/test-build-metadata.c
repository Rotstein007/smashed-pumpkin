#include "build-metadata.h"

static void
test_metadata_and_display(void)
{
  g_autofree char *original_tz = g_strdup(g_getenv("TZ"));
  g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 10, 7, 8, 8, 39);
  const char *formats[] = {"%Y-%m-%d %H:%M", "%d.%m.%Y %H:%M", "%m/%d/%Y %I:%M %p"};
  const char *zones[] = {"UTC", "Asia/Shanghai", "America/New_York"};
  const char *raw = "Build 2026-10-07 08:08 UTC";
  for (guint z = 0; z < G_N_ELEMENTS(zones); ++z) {
    g_setenv("TZ", zones[z], TRUE);
    g_autofree char *metadata = pumpkin_build_metadata_from_unix(g_date_time_to_unix(date));
    g_assert_cmpstr(metadata, ==, raw);
    for (guint f = 0; f < G_N_ELEMENTS(formats); ++f) {
      g_autofree char *display = pumpkin_build_metadata_format(metadata, formats[f]);
      g_autoptr(GDateTime) local = g_date_time_to_local(date);
      g_autofree char *formatted = g_date_time_format(local, formats[f]);
      g_autofree char *expected = g_strdup_printf("Build %s", formatted);
      g_assert_cmpstr(display, ==, expected);
      g_assert_cmpstr(metadata, ==, raw);
      g_assert_true(pumpkin_build_metadata_equal(metadata, "2026-10-07 08:08 UTC"));
      g_assert_false(pumpkin_build_metadata_equal(metadata, "2026-10-07 08:09 UTC"));
    }
  }
  if (original_tz != NULL) g_setenv("TZ", original_tz, TRUE);
  else g_unsetenv("TZ");
}

static void
test_legacy_wall_time(void)
{
  /* Missing UTC is not proof of UTC: preserve old wall-time components rather
   * than shifting them, and do not compare them to UTC metadata as one instant. */
  g_autofree char *display = pumpkin_build_metadata_format("Build 2026-10-07 08:08", "%Y-%m-%d %H:%M");
  g_assert_cmpstr(display, ==, "Build 2026-10-07 08:08");
  g_assert_true(pumpkin_build_metadata_equal("Build 2026-10-07 08:08", "2026-10-07 08:08"));
  g_assert_false(pumpkin_build_metadata_equal("Build 2026-10-07 08:08 UTC", "2026-10-07 08:08"));
  g_assert_true(pumpkin_build_metadata_equal("Build 2026-10-07", "2026-10-07"));
  g_assert_false(pumpkin_build_metadata_equal("Build 2026-10-07", "2026-10-07 00:00"));
}

static void
test_opaque_metadata(void)
{
  const char *labels[] = {
    "Stable 0.2.0+26.3", "Nightly 2026-10-07 08:08 UTC",
    "Build 07.10.2026 16:08", "Build 10/07/2026 04:08 PM", "Benutzerdefinierter Build",
    "Build 2026-99-07 08:08 UTC", "Build 2026-10-07 08:08 trailing data",
    "Build 2026-10-07 08:08 UTC trailing data", "Build 999999999999-10-07"
  };
  for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
    g_autofree char *display = pumpkin_build_metadata_format(labels[i], "%Y-%m-%d %H:%M");
    g_assert_cmpstr(display, ==, labels[i]);
    g_assert_true(pumpkin_build_metadata_equal(labels[i], labels[i]));
    g_assert_false(pumpkin_build_metadata_equal(labels[i], "2026-10-07 08:08 UTC"));
  }
  g_assert_false(pumpkin_build_metadata_equal("Stable v1", "Nightly v1"));
  g_assert_true(pumpkin_build_metadata_equal(NULL, NULL));
  g_assert_false(pumpkin_build_metadata_equal(NULL, "Stable v1"));
  g_assert_null(pumpkin_build_metadata_format(NULL, NULL));
  g_assert_null(pumpkin_build_metadata_format("", NULL));
}

static void
test_strict_legacy_grammar(void)
{
  const struct {
    const char *metadata;
    const char *canonical;
  } malformed[] = {
    {"Build 2026-1-02", "2026-01-02"},
    {"Build 2026-01-2", "2026-01-02"},
    {"Build 2026-+1-02", "2026-01-02"},
    {"Build 2026-01-+2", "2026-01-02"},
    {"Build 2026- 1-02", "2026-01-02"},
    {"Build 2026-01- 2", "2026-01-02"},
    {"Build  2026-01-02", "2026-01-02"},
    {"Build 2026-01-02 3:04", "2026-01-02 03:04"},
    {"Build 2026-01-02 03:4", "2026-01-02 03:04"},
    {"Build 2026-01-02 +3:04", "2026-01-02 03:04"},
    {"Build 2026-01-02 03:+4", "2026-01-02 03:04"},
    {"Build 2026-01-02\t03:04", "2026-01-02 03:04"},
    {"Build 2026-01-02\n03:04 UTC", "2026-01-02 03:04 UTC"},
    {"Build 2026-01-0203:04 UTC", "2026-01-02 03:04 UTC"},
    {"Build 2026-01-02 03:04UTC", "2026-01-02 03:04 UTC"},
    {"Build 2026-01-02 03:04  UTC", "2026-01-02 03:04 UTC"},
    {"Build 2026-01-02 03:04 utc", "2026-01-02 03:04 UTC"},
    {"Build 2026-01-02 03:04 UTC ", "2026-01-02 03:04 UTC"},
    {"Build 2026-02-29", "2026-02-28"},
    {"Build 2026-01-02 24:04", "2026-01-02 00:04"},
    {"Build 2026-01-02 03:60", "2026-01-02 03:00"},
    {"Build 2026-01-02 03:04:00 UTC", "2026-01-02 03:04 UTC"}
  };
  const char *formats[] = {"%Y-%m-%d %H:%M", "%d.%m.%Y %H:%M", "%m/%d/%Y %I:%M %p"};
  for (guint i = 0; i < G_N_ELEMENTS(malformed); ++i) {
    g_test_message("Preserving opaque metadata: %s", malformed[i].metadata);
    for (guint f = 0; f < G_N_ELEMENTS(formats); ++f) {
      g_autofree char *display = pumpkin_build_metadata_format(malformed[i].metadata, formats[f]);
      g_assert_cmpstr(display, ==, malformed[i].metadata);
    }
    g_assert_true(pumpkin_build_metadata_equal(malformed[i].metadata, malformed[i].metadata));
    g_assert_false(pumpkin_build_metadata_equal(malformed[i].metadata, malformed[i].canonical));
    g_assert_false(pumpkin_build_metadata_equal(malformed[i].canonical, malformed[i].metadata));
  }
  g_assert_true(pumpkin_build_metadata_equal("Build 2024-02-29", "2024-02-29"));
  g_assert_true(pumpkin_build_metadata_equal("Build 2026-01-02 00:00 UTC", "2026-01-02 00:00 UTC"));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/build-metadata/data-vs-display", test_metadata_and_display);
  g_test_add_func("/build-metadata/legacy-wall-time", test_legacy_wall_time);
  g_test_add_func("/build-metadata/opaque", test_opaque_metadata);
  g_test_add_func("/build-metadata/strict-legacy-grammar", test_strict_legacy_grammar);
  return g_test_run();
}
