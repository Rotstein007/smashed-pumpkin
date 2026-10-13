#include "minimap-memory-cache.h"

typedef struct { gpointer key; gint64 used; } MemoryCandidate;

static gint memory_oldest(gconstpointer a, gconstpointer b)
{
  const MemoryCandidate *left = a, *right = b;
  return left->used < right->used ? -1 : left->used > right->used ? 1 : 0;
}
guint pumpkin_map_memory_cache_trim(GHashTable *cache, guint limit,
                                   PumpkinMapMemoryCacheUsedFunc get_used, gpointer user_data)
{
  g_return_val_if_fail(cache != NULL, 0);
  g_return_val_if_fail(get_used != NULL, 0);
  guint size = g_hash_table_size(cache);
  if (size <= limit) return 0;
  const guint excess = size - limit;
  g_autoptr(GArray) candidates = g_array_sized_new(FALSE, FALSE, sizeof(MemoryCandidate), size);
  GHashTableIter iter; gpointer key, value;
  g_hash_table_iter_init(&iter, cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    MemoryCandidate candidate = {key, get_used(value, user_data)};
    g_array_append_val(candidates, candidate);
  }
  /* GLib's stable sort keeps the first encountered tile first for equal times. */
  g_array_sort(candidates, memory_oldest);
  guint removed = 0;
  for (guint i = 0; i < excess; i++)
    removed += g_hash_table_remove(cache, g_array_index(candidates, MemoryCandidate, i).key);
  return removed;
}
