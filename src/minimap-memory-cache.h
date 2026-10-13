#pragma once
#include <glib.h>

typedef gint64 (*PumpkinMapMemoryCacheUsedFunc)(gconstpointer value, gpointer user_data);

/* Trim to limit by ascending used time; equal times keep the snapshot order.
 * Snapshot keys are borrowed; removal uses the table's destroy callbacks.
 * The caller must have exclusive access and get_used must not mutate the table;
 * run it on the same thread that writes used values (no locking is provided).
 * Over-limit work is one O(n) snapshot, O(n log n) sort and n - limit removals. */
guint pumpkin_map_memory_cache_trim(GHashTable *cache, guint limit,
                                   PumpkinMapMemoryCacheUsedFunc get_used, gpointer user_data);
