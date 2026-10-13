#include "minimap-memory-cache.h"

typedef struct { guint64 reads; guint keys_freed, values_freed; } CacheStats;
typedef struct { guint id; CacheStats *stats; } CacheKey;
typedef struct { gint64 used; CacheStats *stats; } CacheValue;

static guint cache_key_hash(gconstpointer data)
{ return ((const CacheKey *)data)->id; }
static gboolean cache_key_equal(gconstpointer a, gconstpointer b)
{ return ((const CacheKey *)a)->id == ((const CacheKey *)b)->id; }
static void cache_key_free(gpointer data)
{
  CacheKey *key = data;
  ++key->stats->keys_freed; g_free(key);
}
static void cache_value_free(gpointer data)
{
  CacheValue *value = data;
  ++value->stats->values_freed; g_free(value);
}
static GHashTable *cache_new(void)
{
  return g_hash_table_new_full(cache_key_hash, cache_key_equal, cache_key_free, cache_value_free);
}
static void cache_insert(GHashTable *cache, CacheStats *stats, guint id, gint64 used)
{
  CacheKey *key = g_new(CacheKey, 1); CacheValue *value = g_new(CacheValue, 1);
  *key = (CacheKey){id, stats}; *value = (CacheValue){used, stats};
  g_hash_table_replace(cache, key, value);
}
static CacheValue *cache_value(GHashTable *cache, guint id)
{
  CacheKey key = {id, NULL};
  return g_hash_table_lookup(cache, &key);
}
static gint64 cache_used(gconstpointer data, gpointer user_data)
{
  CacheStats *stats = user_data;
  ++stats->reads;
  return ((const CacheValue *)data)->used;
}
static void assert_freed(const CacheStats *stats, guint count)
{
  g_assert_cmpuint(stats->keys_freed, ==, count);
  g_assert_cmpuint(stats->values_freed, ==, count);
}
static void test_empty(void)
{
  CacheStats stats = {0};
  g_autoptr(GHashTable) cache = cache_new();
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 32768, cache_used, &stats), ==, 0);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 0, cache_used, &stats), ==, 0);
  g_assert_cmpuint(g_hash_table_size(cache), ==, 0);
  g_assert_cmpuint(stats.reads, ==, 0); assert_freed(&stats, 0);
}
static void test_no_eviction(void)
{
  CacheStats stats = {0};
  g_autoptr(GHashTable) cache = cache_new();
  for (guint i = 0; i < 3; i++) cache_insert(cache, &stats, i, i);
  CacheValue *saved = cache_value(cache, 1);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 4, cache_used, &stats), ==, 0);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 3, cache_used, &stats), ==, 0);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, G_MAXUINT, cache_used, &stats), ==, 0);
  g_assert_cmpuint(g_hash_table_size(cache), ==, 3);
  g_assert_true(cache_value(cache, 1) == saved); g_assert_cmpint(saved->used, ==, 1);
  g_assert_cmpuint(stats.reads, ==, 0); assert_freed(&stats, 0);
  g_clear_pointer(&cache, g_hash_table_unref); assert_freed(&stats, 3);
}
static void test_limit_zero(void)
{
  CacheStats stats = {0};
  g_autoptr(GHashTable) cache = cache_new();
  for (guint i = 0; i < 5; i++) cache_insert(cache, &stats, i, i);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 0, cache_used, &stats), ==, 5);
  g_assert_cmpuint(g_hash_table_size(cache), ==, 0);
  g_assert_cmpuint(stats.reads, ==, 5); assert_freed(&stats, 5);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 0, cache_used, &stats), ==, 0);
  g_assert_cmpuint(stats.reads, ==, 5);
  g_clear_pointer(&cache, g_hash_table_unref); assert_freed(&stats, 5);
}
static void test_oldest_n(void)
{
  const gint64 times[] = {G_MAXINT64, 19, G_MININT64, 0, 19, -1};
  CacheStats stats = {0};
  g_autoptr(GHashTable) cache = cache_new();
  for (guint i = 0; i < G_N_ELEMENTS(times); i++) cache_insert(cache, &stats, i, times[i]);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 3, cache_used, &stats), ==, 3);
  g_assert_cmpuint(g_hash_table_size(cache), ==, 3);
  for (guint i = 0; i < G_N_ELEMENTS(times); i++) {
    CacheValue *value = cache_value(cache, i);
    if (times[i] <= 0) g_assert_null(value);
    else { g_assert_nonnull(value); g_assert_cmpint(value->used, ==, times[i]); }
  }
  g_assert_cmpuint(stats.reads, ==, G_N_ELEMENTS(times)); assert_freed(&stats, 3);
  g_clear_pointer(&cache, g_hash_table_unref); assert_freed(&stats, G_N_ELEMENTS(times));
}
static void test_same_time(void)
{
  CacheStats stats = {0};
  g_autoptr(GHashTable) cache = cache_new();
  for (guint i = 0; i < 12; i++) cache_insert(cache, &stats, i, 42);
  guint ids[12], count = 0;
  GHashTableIter iter; gpointer key, value;
  g_hash_table_iter_init(&iter, cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ids[count++] = ((CacheKey *)key)->id;
  }
  g_assert_cmpuint(count, ==, G_N_ELEMENTS(ids));
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 8, cache_used, &stats), ==, 4);
  g_assert_cmpuint(g_hash_table_size(cache), ==, 8);
  /* Equal used times give an arbitrary survivor set: g_array_sort does not
   * guarantee stability. Assert only the count and that every survivor keeps
   * its original value with the same timestamp. */
  for (guint i = 0; i < count; i++) {
    CacheValue *remaining = cache_value(cache, ids[i]);
    if (remaining == NULL) continue;
    g_assert_cmpint(remaining->used, ==, 42);
  }
  g_assert_cmpuint(stats.reads, ==, count); assert_freed(&stats, 4);
  g_clear_pointer(&cache, g_hash_table_unref); assert_freed(&stats, count);
}
static void test_used_refresh(void)
{
  CacheStats stats = {0};
  g_autoptr(GHashTable) cache = cache_new();
  for (guint i = 0; i < 10; i++) cache_insert(cache, &stats, i, i);
  CacheValue *touched = cache_value(cache, 0); touched->used = 100;
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 7, cache_used, &stats), ==, 3);
  g_assert_cmpuint(g_hash_table_size(cache), ==, 7);
  g_assert_true(cache_value(cache, 0) == touched); g_assert_cmpint(touched->used, ==, 100);
  for (guint i = 1; i < 10; i++) {
    if (i <= 3) g_assert_null(cache_value(cache, i));
    else g_assert_nonnull(cache_value(cache, i));
  }
  g_assert_cmpuint(stats.reads, ==, 10); assert_freed(&stats, 3);
  g_clear_pointer(&cache, g_hash_table_unref); assert_freed(&stats, 10);
}
/* The previous main-thread loop, retained only as a count/equivalence baseline. */
static void trim_repeated_scan(GHashTable *cache, guint limit, CacheStats *stats)
{
  while (g_hash_table_size(cache) > limit) {
    GHashTableIter iter; gpointer key, value, oldest = NULL; gint64 time = G_MAXINT64;
    g_hash_table_iter_init(&iter, cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      gint64 used = cache_used(value, stats);
      if (used < time) { oldest = key; time = used; }
    }
    g_assert_nonnull(oldest); g_assert_true(g_hash_table_remove(cache, oldest));
  }
}
static void test_batch_1024(void)
{
  const guint limit = 32768, batch = 1024, size = limit + batch;
  CacheStats stats = {0}, repeated = {0};
  g_autoptr(GHashTable) cache = cache_new();
  g_autoptr(GHashTable) reference = cache_new();
  for (guint i = size; i > 0; i--) {
    cache_insert(cache, &stats, i - 1, i - 1);
    cache_insert(reference, &repeated, i - 1, i - 1);
  }
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, limit, cache_used, &stats), ==, batch);
  trim_repeated_scan(reference, limit, &repeated);
  g_assert_cmpuint(g_hash_table_size(cache), ==, limit);
  g_assert_cmpuint(g_hash_table_size(reference), ==, limit);
  for (guint i = 0; i < size; i++) {
    CacheValue *value = cache_value(cache, i), *old = cache_value(reference, i);
    if (i < batch) { g_assert_null(value); g_assert_null(old); }
    else {
      g_assert_nonnull(value); g_assert_nonnull(old);
      g_assert_cmpint(value->used, ==, i); g_assert_cmpint(old->used, ==, i);
    }
  }
  guint64 repeated_reads = (guint64)batch * (2 * (guint64)size - batch + 1) / 2;
  g_assert_cmpuint(stats.reads, ==, size); g_assert_cmpuint(repeated.reads, ==, repeated_reads);
  g_test_message("Used reads: repeated scan=%" G_GUINT64_FORMAT ", snapshot=%" G_GUINT64_FORMAT
    "; snapshot is followed by an O(n log n) stable sort", repeated.reads, stats.reads);
  assert_freed(&stats, batch); assert_freed(&repeated, batch);
  g_clear_pointer(&cache, g_hash_table_unref); g_clear_pointer(&reference, g_hash_table_unref);
  assert_freed(&stats, size); assert_freed(&repeated, size);
}
static void test_ownership(void)
{
  CacheStats stats = {0};
  g_autoptr(GHashTable) cache = cache_new();
  for (guint i = 0; i < 8; i++) cache_insert(cache, &stats, i, i);
  cache_insert(cache, &stats, 3, 103); assert_freed(&stats, 1);
  CacheValue *replacement = cache_value(cache, 3), *newest = cache_value(cache, 7);
  g_assert_cmpuint(pumpkin_map_memory_cache_trim(cache, 3, cache_used, &stats), ==, 5);
  g_assert_cmpuint(g_hash_table_size(cache), ==, 3); assert_freed(&stats, 6);
  g_assert_true(cache_value(cache, 3) == replacement); g_assert_true(cache_value(cache, 7) == newest);
  g_assert_nonnull(cache_value(cache, 6));
  g_assert_cmpuint(stats.reads, ==, 8);
  g_clear_pointer(&cache, g_hash_table_unref); assert_freed(&stats, 9);
}
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/minimap-memory-cache/empty", test_empty);
  g_test_add_func("/minimap-memory-cache/no-eviction", test_no_eviction);
  g_test_add_func("/minimap-memory-cache/limit-zero", test_limit_zero);
  g_test_add_func("/minimap-memory-cache/oldest-n", test_oldest_n);
  g_test_add_func("/minimap-memory-cache/same-time", test_same_time);
  g_test_add_func("/minimap-memory-cache/used-refresh", test_used_refresh);
  g_test_add_func("/minimap-memory-cache/batch-1024", test_batch_1024);
  g_test_add_func("/minimap-memory-cache/ownership", test_ownership);
  return g_test_run();
}
