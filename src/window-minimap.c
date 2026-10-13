#include "window-minimap.h"
#include "i18n.h"
#include "minimap-store.h"
#include "minimap-bridge-control.h"
#include "minimap-terrain.h"
#include "minimap-cache.h"
#include "minimap-memory-cache.h"
#include "minimap-motion.h"
#include "minimap-render.h"
#include "vendor/cjson/cJSON.h"
#include "platform.h"
#include "window-lifecycle.h"
#include <math.h>
#include <string.h>

typedef struct { char *region; int x, z; cairo_surface_t *surface; gint64 used, loaded; guint64 revision; gboolean cache_probed; } TerrainTile;
typedef struct {
  PumpkinWindow *window;
  GtkStack *map_view;
  GtkWidget *map, *footer, *install, *approve, *stop_tracking;
  AdwBanner *bridge_banner;
  GHashTable *terrain, *monitors;
  GtkDrawingArea *canvas;
  GtkDropDown *world_choice;
  GtkBox *world_tabs;
  GtkScale *refresh_rate;
  GtkLabel *status, *terrain_status, *player_name, *player_info;
  GtkPopover *popover;
  GtkMenuButton *player_menu;
  GtkBox *player_choices;
  AdwSwitchRow *enabled;
  GPtrArray *regions, *choices, *virtual_worlds;
  PumpkinMapSnapshot *live;
  PumpkinMapMotion *motion;
  PumpkinServer *server;
  GCancellable *cancel;
  char *request, *selected, *tracked, *action_json, *action_id, *bridge_error, *scan_error;
  double center_x, center_z, scale, drag_x, drag_z;
  guint timer, frame, generation, inventory_revision;
  gint64 last_scan, last_live, last_terrain, action_started, rescan_at;
  int pid;
  gboolean busy, scan_busy, terrain_busy, changing_choice, user_view, auto_located;
  gboolean consent_open;
  gboolean bridge_updating, bridge_update_attempted;
  PumpkinMapBridgeAccess bridge_access;
} MapPage;
typedef struct {
  PumpkinWindow *window;
  PumpkinServer *server;
  guint generation, inventory_revision;
  char *request, *json;
  gboolean scan, running;
  PumpkinMapSnapshot *live;
  GPtrArray *regions, *directories;
  char *scan_error, *live_error;
  PumpkinMapBridgeAccess bridge_access;
} MapTask;

static MapPage *page_for(PumpkinWindow *self) { return g_object_get_data(G_OBJECT(self), "minimap-page"); }
static gint64 now_ms(void) { return g_get_real_time() / 1000; }
static gboolean poll_map(gpointer data);
static void load_terrain(MapPage *page);
static void sync_world_tabs(MapPage *page);
static void request_consent(MapPage *page);
static gboolean animate_map(GtkWidget *widget, GdkFrameClock *clock, gpointer data);
static gboolean active(MapPage *page)
{
  return !page->window->disposing && page->window->current != NULL &&
    pumpkin_config_get_minimap_consent(page->window->config) &&
    adw_switch_row_get_active(page->enabled) && gtk_widget_get_mapped(page->map);
}
static PumpkinMapWorld *chosen(MapPage *page)
{
  guint selected = gtk_drop_down_get_selected(page->world_choice);
  return selected < page->choices->len ? g_ptr_array_index(page->choices, selected) : NULL;
}
static PumpkinMapWorld *live_world(MapPage *page, PumpkinMapWorld *world)
{
  if (world == NULL || !pumpkin_map_snapshot_fresh(page->live, now_ms())) return NULL;
  guint matches = 0;
  for (guint i = 0; i < page->choices->len; i++) {
    PumpkinMapWorld *candidate = g_ptr_array_index(page->choices, i);
    if (g_strcmp0(candidate->name, world->name) == 0 && g_strcmp0(candidate->dimension, world->dimension) == 0) matches++;
  }
  if (matches != 1) return NULL; /* The SDK does not expose absolute world paths. */
  for (guint i = 0; i < page->live->worlds->len; i++) {
    PumpkinMapWorld *candidate = g_ptr_array_index(page->live->worlds, i);
    if (g_strcmp0(candidate->name, world->name) == 0 && g_strcmp0(candidate->dimension, world->dimension) == 0) return candidate;
  }
  return NULL;
}
static PumpkinMapPlayer *selected_player(MapPage *page)
{
  if (!pumpkin_map_snapshot_fresh(page->live, now_ms())) return NULL;
  return pumpkin_map_find_player(page->live, page->selected);
}
static void stop_tracking(GtkButton *button, MapPage *page)
{
  (void)button;
  g_clear_pointer(&page->tracked, g_free);
  gtk_widget_set_visible(page->stop_tracking, FALSE);
  gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
}
static void choose_changed(GObject *object, GParamSpec *pspec, MapPage *page)
{
  (void)object; (void)pspec;
  if (page->changing_choice) return;
  sync_world_tabs(page);
  if (object != NULL) page->user_view = TRUE;
  stop_tracking(NULL, page);
  gtk_popover_popdown(page->popover);
  PumpkinMapWorld *world = chosen(page);
  if (world != NULL && world->chunks != NULL && world->chunks->len != 0) {
    /* Start near the origin rather than an arbitrary region enumeration edge. */
    PumpkinMapChunk best = g_array_index(world->chunks, PumpkinMapChunk, 0);
    for (guint i = 1; i < world->chunks->len; i++) {
      PumpkinMapChunk chunk = g_array_index(world->chunks, PumpkinMapChunk, i);
      if ((double)chunk.x * chunk.x + (double)chunk.z * chunk.z <
          (double)best.x * best.x + (double)best.z * best.z) best = chunk;
    }
    page->center_x = best.x * 16.0 + 8; page->center_z = best.z * 16.0 + 8;
  }
  load_terrain(page);
  gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
}
static void world_tab_clicked(GtkToggleButton *button, MapPage *page)
{
  if (page->changing_choice || !gtk_toggle_button_get_active(button)) return;
  gtk_drop_down_set_selected(page->world_choice, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "world-index")));
}
static void sync_world_tabs(MapPage *page)
{
  guint selected = gtk_drop_down_get_selected(page->world_choice);
  gboolean changing = page->changing_choice; page->changing_choice = TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(page->world_tabs)); child != NULL;
       child = gtk_widget_get_next_sibling(child))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(child),
      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "world-index")) == selected);
  page->changing_choice = changing;
}
static void update_choices(MapPage *page, const char *previous_id)
{
  g_autofree char *id = g_strdup(previous_id);
  page->changing_choice = TRUE;
  g_ptr_array_set_size(page->choices, 0);
  GtkStringList *labels = gtk_string_list_new(NULL);
  while (gtk_widget_get_first_child(GTK_WIDGET(page->world_tabs)) != NULL)
    gtk_box_remove(page->world_tabs, gtk_widget_get_first_child(GTK_WIDGET(page->world_tabs)));
  guint selected = 0;
  guint available = page->regions->len + page->virtual_worlds->len;
  for (guint i = 0; i < available; i++) {
    PumpkinMapWorld *world = i < page->regions->len ? g_ptr_array_index(page->regions, i) :
      g_ptr_array_index(page->virtual_worlds, i - page->regions->len);
    g_ptr_array_add(page->choices, world);
    const char *dimension = g_strcmp0(world->dimension, "minecraft:overworld") == 0 ? _("Overworld") :
      g_strcmp0(world->dimension, "minecraft:the_nether") == 0 ? _("Nether") :
      g_strcmp0(world->dimension, "minecraft:the_end") == 0 ? _("End") : world->dimension;
    gboolean multiple_roots = FALSE;
    for (guint j = 0; j < available; j++) {
      PumpkinMapWorld *other = j < page->regions->len ? g_ptr_array_index(page->regions, j) :
        g_ptr_array_index(page->virtual_worlds, j - page->regions->len);
      if (g_strcmp0(other->name, world->name) != 0) { multiple_roots = TRUE; break; }
    }
    g_autofree char *label = multiple_roots ? g_strdup_printf("%s / %s", world->name, dimension) : g_strdup(dimension);
    gtk_string_list_append(labels, label);
    if (available <= 4) {
      GtkWidget *tab = gtk_toggle_button_new_with_label(label);
      GtkWidget *first = gtk_widget_get_first_child(GTK_WIDGET(page->world_tabs));
      if (first != NULL) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(tab), GTK_TOGGLE_BUTTON(first));
      gtk_label_set_max_width_chars(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(tab))), 18);
      gtk_label_set_ellipsize(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(tab))), PANGO_ELLIPSIZE_END);
      gtk_widget_set_tooltip_text(tab, label);
      g_object_set_data(G_OBJECT(tab), "world-index", GUINT_TO_POINTER(i));
      gtk_box_append(page->world_tabs, tab);
      g_signal_connect(tab, "toggled", G_CALLBACK(world_tab_clicked), page);
    }
    if (g_strcmp0(world->id, id) == 0) selected = i;
  }
  if (page->choices->len == 0) gtk_string_list_append(labels, _("No saved chunks"));
  gtk_drop_down_set_model(page->world_choice, G_LIST_MODEL(labels));
  g_object_unref(labels);
  gtk_drop_down_set_selected(page->world_choice, selected);
  gtk_widget_set_sensitive(GTK_WIDGET(page->world_choice), page->choices->len != 0);
  gtk_widget_set_visible(GTK_WIDGET(page->world_tabs), page->choices->len > 0 && page->choices->len <= 4);
  gtk_widget_set_visible(GTK_WIDGET(page->world_choice), page->choices->len > 4);
  sync_world_tabs(page);
  page->changing_choice = FALSE;
  if (id == NULL) choose_changed(NULL, NULL, page);
}
static void update_virtual_worlds(MapPage *page, const char *previous_id, gboolean force)
{
  GPtrArray *next = g_ptr_array_new_with_free_func(pumpkin_map_world_free);
  if (pumpkin_map_snapshot_fresh(page->live, now_ms())) for (guint i = 0; i < page->live->worlds->len; i++) {
    PumpkinMapWorld *source = g_ptr_array_index(page->live->worlds, i);
    if (source->chunks->len == 0) continue;
    gboolean saved = FALSE;
    for (guint j = 0; j < page->regions->len; j++) {
      PumpkinMapWorld *region = g_ptr_array_index(page->regions, j);
      if (g_strcmp0(region->name, source->name) == 0 && g_strcmp0(region->dimension, source->dimension) == 0) saved = TRUE;
    }
    if (saved) continue;
    PumpkinMapWorld *world = g_new0(PumpkinMapWorld, 1);
    world->id = g_strconcat("live:", source->id, NULL);
    world->name = g_strdup(source->name); world->dimension = g_strdup(source->dimension);
    world->chunks = g_array_ref(source->chunks); world->tiles = g_ptr_array_new_with_free_func(pumpkin_map_tile_free);
    g_ptr_array_add(next, world);
  }
  gboolean same = next->len == page->virtual_worlds->len;
  for (guint i = 0; same && i < next->len; i++) {
    PumpkinMapWorld *old = g_ptr_array_index(page->virtual_worlds, i), *new = g_ptr_array_index(next, i);
    same = g_strcmp0(old->id, new->id) == 0;
  }
  if (same && !force) {
    for (guint i = 0; i < next->len; i++) {
      PumpkinMapWorld *old = g_ptr_array_index(page->virtual_worlds, i), *new = g_ptr_array_index(next, i);
      g_array_unref(old->chunks); old->chunks = g_array_ref(new->chunks);
    }
    g_ptr_array_unref(next); return;
  }
  g_ptr_array_set_size(page->choices, 0);
  g_ptr_array_unref(page->virtual_worlds); page->virtual_worlds = next;
  update_choices(page, previous_id);
}

static gboolean saved_chunk(PumpkinMapWorld *world, int x, int z)
{
  int rx = (int)floor(x / 32.0), rz = (int)floor(z / 32.0);
  guint index = (guint)(z - rz * 32) * 32 + (guint)(x - rx * 32);
  if (world->tile_index == NULL) {
    world->tile_index = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    for (guint i = 0; i < world->tiles->len; i++) {
      PumpkinMapTile *tile = g_ptr_array_index(world->tiles, i);
      guint64 *key = g_new(guint64, 1); *key = ((guint64)(guint32)tile->x << 32) | (guint32)tile->z;
      g_hash_table_insert(world->tile_index, key, tile);
    }
  }
  guint64 key = ((guint64)(guint32)rx << 32) | (guint32)rz;
  PumpkinMapTile *tile = g_hash_table_lookup(world->tile_index, &key);
  return tile != NULL && tile->revisions[index] != 0;
}
static void map_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  (void)area;
  MapPage *page = data;
  gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
  cairo_set_source_rgb(cr, dark ? .10 : .93, dark ? .12 : .94, dark ? .13 : .95);
  cairo_paint(cr);
  PumpkinMapWorld *world = chosen(page);
  if (world == NULL) return;
  PumpkinMapWorld *border = live_world(page, world);
  gboolean fresh = border != NULL;
  PumpkinMapPlayer *tracked = fresh ? pumpkin_map_find_player(page->live, page->tracked) : NULL;
  gint64 frame_time = g_get_monotonic_time();
  if (tracked != NULL && pumpkin_map_player_matches(tracked, world)) {
    PumpkinMapPose pose = pumpkin_map_motion_pose(page->motion, tracked, frame_time);
    page->center_x = pose.x; page->center_z = pose.z;
  }
  double left = page->center_x - width / (2 * page->scale), top = page->center_z - height / (2 * page->scale);
  double tile = 16 * page->scale;
  /* Fractional zoom must not blend chunk clips into grey seams. */
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  GHashTableIter iter; gpointer value;
  g_hash_table_iter_init(&iter, page->terrain);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    TerrainTile *image = value;
    if (image->surface == NULL || g_strcmp0(image->region, world->region) != 0) continue;
    double x = (image->x * 16.0 - left) * page->scale, z = (image->z * 16.0 - top) * page->scale;
    if (x + tile < 0 || z + tile < 0 || x > width || z > height) continue;
    if (!saved_chunk(world, image->x, image->z)) continue;
    image->used = g_get_monotonic_time();
    pumpkin_map_paint_chunk(cr, image->surface, x, z, page->scale);
  }
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
  if (border != NULL && border->border_known) {
    double diameter = pumpkin_map_border_size(border, now_ms());
    double x = (border->border_x - diameter / 2 - left) * page->scale;
    double z = (border->border_z - diameter / 2 - top) * page->scale;
    double size = diameter * page->scale;
    double l = CLAMP(x, 0, width), r = CLAMP(x + size, 0, width);
    double t = CLAMP(z, 0, height), b = CLAMP(z + size, 0, height);
    cairo_set_source_rgba(cr, .88, .25, .24, .10);
    /* Clip enormous vanilla borders before passing coordinates to Cairo. */
    cairo_rectangle(cr, 0, 0, width, height); cairo_rectangle(cr, l, t, r - l, b - t);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD); cairo_fill(cr); cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_set_source_rgba(cr, 1, .40, .33, .95); cairo_set_line_width(cr, 2);
    if (x >= 0 && x <= width) { cairo_move_to(cr, x, t); cairo_line_to(cr, x, b); }
    if (x + size >= 0 && x + size <= width) { cairo_move_to(cr, x + size, t); cairo_line_to(cr, x + size, b); }
    if (z >= 0 && z <= height) { cairo_move_to(cr, l, z); cairo_line_to(cr, r, z); }
    if (z + size >= 0 && z + size <= height) { cairo_move_to(cr, l, z + size); cairo_line_to(cr, r, z + size); }
    cairo_stroke(cr);
  }
  if (fresh) for (guint i = 0; i < page->live->players->len; i++) {
    PumpkinMapPlayer *player = g_ptr_array_index(page->live->players, i);
    if (!pumpkin_map_player_matches(player, world)) continue;
    PumpkinMapPose pose = pumpkin_map_motion_pose(page->motion, player, frame_time);
    double x = (pose.x - left) * page->scale, z = (pose.z - top) * page->scale;
    if (x < -20 || z < -20 || x > width + 20 || z > height + 20) continue;
    gboolean following = g_strcmp0(player->id, page->tracked) == 0;
    gboolean selected = g_strcmp0(player->id, page->selected) == 0;
    pumpkin_map_paint_player(cr, x, z, pose.yaw, player->yaw_known, player->color, following, selected);
  }
  GdkRGBA color;
  gtk_widget_get_color(GTK_WIDGET(area), &color);
  gdk_cairo_set_source_rgba(cr, &color);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  double cx = width - 52, cy = 58;
  cairo_set_source_rgba(cr, dark ? .12 : 1, dark ? .14 : 1, dark ? .16 : 1, .82);
  cairo_arc(cr, cx, cy, 36, 0, 2 * G_PI); cairo_fill(cr);
  gdk_cairo_set_source_rgba(cr, &color); cairo_set_font_size(cr, 11);
  cairo_move_to(cr, cx - 4, cy - 21); cairo_show_text(cr, "N");
  cairo_move_to(cr, cx + 24, cy + 4); cairo_show_text(cr, "O");
  cairo_move_to(cr, cx - 4, cy + 29); cairo_show_text(cr, "S");
  cairo_move_to(cr, cx - 32, cy + 4); cairo_show_text(cr, "W");
  cairo_move_to(cr, cx, cy - 16); cairo_line_to(cr, cx + 5, cy); cairo_line_to(cr, cx - 5, cy);
  cairo_close_path(cr); cairo_set_source_rgb(cr, .90, .30, .27); cairo_fill(cr);
  cairo_move_to(cr, cx, cy + 16); cairo_line_to(cr, cx + 5, cy); cairo_line_to(cr, cx - 5, cy);
  cairo_close_path(cr); gdk_cairo_set_source_rgba(cr, &color); cairo_fill(cr);
  double blocks = pow(2, floor(log2(90 / page->scale)));
  double length = blocks * page->scale;
  cairo_set_line_width(cr, 2); cairo_move_to(cr, 18, height - 22); cairo_line_to(cr, 18 + length, height - 22); cairo_stroke(cr);
  g_autofree char *scale = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE,
    "%.0f block", "%.0f blocks", (gulong)blocks), blocks);
  cairo_move_to(cr, 18, height - 32); cairo_show_text(cr, scale);
}

static void update_player_card(MapPage *page)
{
  PumpkinMapPlayer *player = selected_player(page);
  PumpkinMapWorld *world = chosen(page);
  if (player == NULL || world == NULL || !pumpkin_map_player_matches(player, world)) {
    gtk_popover_popdown(page->popover); return;
  }
  gtk_label_set_text(page->player_name, player->name);
  g_autofree char *info = g_strdup_printf(_("%u ms ping\nX %.1f   Y %.1f   Z %.1f"), player->ping, player->x, player->y, player->z);
  gtk_label_set_text(page->player_info, info);
}
static void locate_player(GtkButton *button, MapPage *page)
{
  const char *id = g_object_get_data(G_OBJECT(button), "map-player-id");
  PumpkinMapPlayer *player = pumpkin_map_snapshot_fresh(page->live, now_ms()) ?
    pumpkin_map_find_player(page->live, id) : NULL;
  if (player == NULL) return;
  page->user_view = TRUE;
  for (guint i = 0; i < page->choices->len; i++) {
    PumpkinMapWorld *world = g_ptr_array_index(page->choices, i);
    if (!pumpkin_map_player_matches(player, world) || live_world(page, world) == NULL) continue;
    gtk_drop_down_set_selected(page->world_choice, i);
    stop_tracking(NULL, page);
    page->center_x = player->x; page->center_z = player->z;
    g_free(page->selected); page->selected = g_strdup(id);
    gtk_menu_button_popdown(page->player_menu);
    update_player_card(page);
    GdkRectangle anchor = {gtk_widget_get_width(GTK_WIDGET(page->canvas)) / 2,
                           gtk_widget_get_height(GTK_WIDGET(page->canvas)) / 2, 1, 1};
    gtk_popover_set_pointing_to(page->popover, &anchor); gtk_popover_popup(page->popover);
    gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
    return;
  }
  set_details_status(page->window, _("This player's world has no saved chunks or cannot be identified unambiguously"), 5);
}
static void players_menu_opened(GObject *object, GParamSpec *pspec, MapPage *page)
{
  (void)pspec;
  if (!gtk_widget_get_visible(GTK_WIDGET(object))) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(page->player_choices))) != NULL)
    gtk_box_remove(page->player_choices, child);
  gboolean fresh = pumpkin_map_snapshot_fresh(page->live, now_ms());
  guint count = fresh ? page->live->players->len : 0;
  if (count == 0) gtk_box_append(page->player_choices, gtk_label_new(fresh ? _("No players online") : _("Live data unavailable")));
  for (guint i = 0; i < count; i++) {
    PumpkinMapPlayer *player = g_ptr_array_index(page->live->players, i);
    GtkWidget *entry = gtk_button_new_with_label(player->name);
    gtk_widget_add_css_class(entry, "flat");
    g_object_set_data_full(G_OBJECT(entry), "map-player-id", g_strdup(player->id), g_free);
    g_signal_connect(entry, "clicked", G_CALLBACK(locate_player), page);
    gtk_box_append(page->player_choices, entry);
  }
}
static void map_clicked(GtkGestureClick *gesture, int count, double x, double y, MapPage *page)
{
  (void)gesture; (void)count;
  PumpkinMapWorld *world = chosen(page);
  if (live_world(page, world) == NULL) return;
  double width = gtk_widget_get_width(GTK_WIDGET(page->canvas)), height = gtk_widget_get_height(GTK_WIDGET(page->canvas));
  double best = 20 * 20;
  PumpkinMapPlayer *hit = NULL;
  for (guint i = 0; i < page->live->players->len; i++) {
    PumpkinMapPlayer *player = g_ptr_array_index(page->live->players, i);
    if (!pumpkin_map_player_matches(player, world)) continue;
    PumpkinMapPose pose = pumpkin_map_motion_pose(page->motion, player, g_get_monotonic_time());
    double dx = (pose.x - page->center_x) * page->scale + width / 2 - x;
    double dy = (pose.z - page->center_z) * page->scale + height / 2 - y;
    if (dx * dx + dy * dy <= best) { hit = player; best = dx * dx + dy * dy; }
  }
  g_free(page->selected); page->selected = hit != NULL ? g_strdup(hit->id) : NULL;
  if (hit != NULL) {
    update_player_card(page);
    GdkRectangle anchor = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(page->popover, &anchor); gtk_popover_popup(page->popover);
  } else gtk_popover_popdown(page->popover);
  gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
}
static void track_clicked(GtkButton *button, MapPage *page)
{
  (void)button;
  if (selected_player(page) == NULL) return;
  g_free(page->tracked); page->tracked = g_strdup(page->selected);
  gtk_widget_set_visible(page->stop_tracking, TRUE);
  gtk_popover_popdown(page->popover); gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
}
static void drag_begin(GtkGestureDrag *gesture, double x, double y, MapPage *page)
{
  page->user_view = TRUE;
  (void)gesture; (void)x; (void)y;
  page->drag_x = page->center_x; page->drag_z = page->center_z;
}
static void drag_update(GtkGestureDrag *gesture, double x, double y, MapPage *page)
{
  (void)gesture;
  if (hypot(x, y) < 5) return;
  stop_tracking(NULL, page); gtk_popover_popdown(page->popover);
  page->center_x = CLAMP(page->drag_x - x / page->scale, -6e7, 6e7);
  page->center_z = CLAMP(page->drag_z - y / page->scale, -6e7, 6e7);
  gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
}
static gboolean zoom_scroll(GtkEventControllerScroll *controller, double dx, double dy, MapPage *page)
{
  page->user_view = TRUE;
  (void)controller; (void)dx;
  page->scale = CLAMP(page->scale * exp(-dy * .15), .05, 8);
  gtk_widget_queue_draw(GTK_WIDGET(page->canvas)); return TRUE;
}
typedef struct { PumpkinMapChunk chunk; double distance; guint64 revision; } TerrainCandidate;
typedef struct { PumpkinMapChunk chunk; guint64 revision; } TerrainRequest;
typedef struct {
  PumpkinWindow *window;
  guint generation;
  char *region, *error;
  GArray *chunks;
  GPtrArray *tiles;
  gboolean cache_only;
} TerrainTask;
static char *terrain_key(const char *region, int x, int z)
{ return g_strdup_printf("%s/%d,%d", region, x, z); }
static void terrain_tile_free(gpointer data)
{
  TerrainTile *tile = data;
  if (tile == NULL) return;
  if (tile->surface != NULL) cairo_surface_destroy(tile->surface);
  g_free(tile->region); g_free(tile);
}
static gint64 terrain_tile_used(gconstpointer data, gpointer user_data)
{
  (void)user_data;
  const TerrainTile *tile = data;
  return tile->used;
}
static void terrain_task_free(gpointer data)
{
  TerrainTask *task = data;
  g_object_unref(task->window); g_free(task->region); g_free(task->error);
  g_array_unref(task->chunks); g_ptr_array_unref(task->tiles); g_free(task);
}
static void terrain_worker(GTask *result, gpointer source, gpointer data, GCancellable *cancel)
{
  (void)source;
  TerrainTask *task = data;
  gint64 deadline = g_get_monotonic_time() + 40000;
  g_autoptr(GHashTable) readers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
    (GDestroyNotify)pumpkin_terrain_region_free);
  for (guint i = 0; i < task->chunks->len && !g_cancellable_is_cancelled(cancel); i++) {
    TerrainRequest request = g_array_index(task->chunks, TerrainRequest, i);
    PumpkinMapChunk chunk = request.chunk;
    TerrainTile *tile = g_new0(TerrainTile, 1);
    tile->region = g_strdup(task->region); tile->x = chunk.x; tile->z = chunk.z;
    tile->revision = request.revision;
    g_autoptr(GError) error = NULL;
    tile->cache_probed = task->cache_only;
    if (task->cache_only)
      tile->surface = pumpkin_map_cache_read_previous(task->region, chunk.x, chunk.z, &tile->revision);
    else {
      int rx = (int)floor(chunk.x / 32.0), rz = (int)floor(chunk.z / 32.0);
      g_autofree char *key = g_strdup_printf("%d,%d", rx, rz);
      PumpkinTerrainRegion *reader = g_hash_table_lookup(readers, key);
      if (reader == NULL) {
        reader = pumpkin_terrain_region_open(task->region, rx, rz, cancel, &error);
        if (reader != NULL) g_hash_table_insert(readers, g_strdup(key), reader);
      }
      if (reader != NULL) tile->surface = pumpkin_terrain_region_read(reader, chunk.x, chunk.z, cancel, &error);
      if (tile->surface != NULL && !g_cancellable_is_cancelled(cancel))
        pumpkin_map_cache_write(task->region, chunk.x, chunk.z, request.revision, tile->surface);
    }
    tile->loaded = tile->used = g_get_monotonic_time();
    if (error != NULL && task->error == NULL) task->error = g_strdup(error->message);
    g_ptr_array_add(task->tiles, tile);
    if (g_get_monotonic_time() >= deadline) break;
  }
  g_task_return_boolean(result, TRUE);
}
static void terrain_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source; (void)data;
  TerrainTask *task = g_task_get_task_data(G_TASK(result));
  MapPage *page = page_for(task->window);
  if (task->window->disposing || page->generation != task->generation) return;
  page->terrain_busy = FALSE;
  for (guint i = 0; i < task->tiles->len; i++) {
    TerrainTile *tile = g_ptr_array_index(task->tiles, i);
    g_autofree char *key = terrain_key(tile->region, tile->x, tile->z);
    TerrainTile *old = g_hash_table_lookup(page->terrain, key);
    if (old != NULL && old->surface != NULL && tile->surface == NULL) {
      old->loaded = tile->loaded; continue; /* Preserve a tile during a concurrent server save. */
    }
    g_ptr_array_index(task->tiles, i) = NULL;
    g_hash_table_replace(page->terrain, g_steal_pointer(&key), tile);
  }
  /* Hysteresis: a full snapshot + sort costs a few milliseconds and most
   * batches only overshoot by a few tiles. Trim only when the cache exceeds
   * the limit by a whole batch, then bring it back down to the limit. */
  if (g_hash_table_size(page->terrain) > 32768 + 1024) {
    pumpkin_map_memory_cache_trim(page->terrain, 32768, terrain_tile_used, NULL);
  }
  gtk_widget_set_tooltip_text(GTK_WIDGET(page->terrain_status), task->error);
  gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
  page->last_terrain = 0; load_terrain(page);
}
static gint terrain_nearest(gconstpointer a, gconstpointer b)
{
  const TerrainCandidate *left = a, *right = b;
  return left->distance < right->distance ? -1 : left->distance > right->distance ? 1 : 0;
}
static gint region_nearest(gconstpointer a, gconstpointer b, gpointer data)
{
  const PumpkinMapTile *left = *(PumpkinMapTile *const *)a, *right = *(PumpkinMapTile *const *)b;
  const MapPage *page = data;
  double lx = left->x * 512.0 + 256 - page->center_x, lz = left->z * 512.0 + 256 - page->center_z;
  double rx = right->x * 512.0 + 256 - page->center_x, rz = right->z * 512.0 + 256 - page->center_z;
  double l = lx * lx + lz * lz, r = rx * rx + rz * rz;
  return l < r ? -1 : l > r ? 1 : 0;
}
static void load_terrain(MapPage *page)
{
  if (!active(page) || page->terrain_busy || page->changing_choice) return;
  gint64 now = g_get_monotonic_time();
  if (page->last_terrain != 0 && now - page->last_terrain < 200000) return;
  page->last_terrain = now;
  PumpkinMapWorld *world = chosen(page);
  if (world == NULL) {
    gtk_label_set_text(page->terrain_status, page->scan_busy || page->last_scan == 0 ?
      _("Discovering saved worlds...") : _("No saved terrain yet / chunks appear after the server saves them"));
    return;
  }
  if (world->region == NULL) {
    gtk_label_set_text(page->terrain_status, _("Live chunk positions / terrain has not been saved yet"));
    return;
  }
  double half_x = gtk_widget_get_width(GTK_WIDGET(page->canvas)) / (2 * page->scale) + 16;
  double half_z = gtk_widget_get_height(GTK_WIDGET(page->canvas)) / (2 * page->scale) + 16;
  if (half_x <= 16 || half_z <= 16) return;
  g_autoptr(GArray) candidates = g_array_new(FALSE, FALSE, sizeof(TerrainCandidate));
  g_autoptr(GPtrArray) visible_regions = g_ptr_array_new();
  for (guint i = 0; i < world->tiles->len; i++) {
    PumpkinMapTile *tile = g_ptr_array_index(world->tiles, i);
    double rx = tile->x * 512.0 - page->center_x, rz = tile->z * 512.0 - page->center_z;
    if (rx > half_x || rz > half_z || rx + 512 < -half_x || rz + 512 < -half_z) continue;
    g_ptr_array_add(visible_regions, tile);
  }
  g_ptr_array_sort_with_data(visible_regions, region_nearest, page);
  /* Bound main-thread sorting even at the widest zoom on multi-million-chunk worlds. */
  for (guint i = 0; i < visible_regions->len && candidates->len < 32768; i++) {
    PumpkinMapTile *tile = g_ptr_array_index(visible_regions, i);
    double rx = tile->x * 512.0 - page->center_x, rz = tile->z * 512.0 - page->center_z;
    const guint8 *mask = cairo_image_surface_get_data(tile->mask);
    int stride = cairo_image_surface_get_stride(tile->mask);
    for (int z = 0; z < 32; z++) for (int x = 0; x < 32; x++) {
      if (mask[z * stride + x] == 0) continue;
      double dx = rx + x * 16 + 8, dz = rz + z * 16 + 8;
      if (fabs(dx) <= half_x && fabs(dz) <= half_z) {
        TerrainCandidate candidate = {{tile->x * 32 + x, tile->z * 32 + z}, dx * dx + dz * dz,
          tile->revisions[z * 32 + x]};
        g_array_append_val(candidates, candidate);
      }
    }
  }
  g_array_sort(candidates, terrain_nearest);
  guint wanted = MIN(candidates->len, 16384), ready = 0, failed = 0;
  g_autoptr(GArray) chunks = g_array_new(FALSE, FALSE, sizeof(TerrainRequest));
  g_autoptr(GArray) cached = g_array_new(FALSE, FALSE, sizeof(TerrainRequest));
  g_autoptr(GArray) refresh = g_array_new(FALSE, FALSE, sizeof(TerrainRequest));
  for (guint i = 0; i < wanted; i++) {
    TerrainCandidate candidate = g_array_index(candidates, TerrainCandidate, i);
    PumpkinMapChunk chunk = candidate.chunk;
    g_autofree char *key = terrain_key(world->region, chunk.x, chunk.z);
    TerrainTile *tile = g_hash_table_lookup(page->terrain, key);
    if (tile != NULL) { tile->used = now; if (tile->surface != NULL) ++ready; else ++failed; }
    TerrainRequest request = {chunk, candidate.revision};
    if (tile == NULL && cached->len < 1024) g_array_append_val(cached, request);
    else if (tile != NULL && tile->surface == NULL && tile->cache_probed && chunks->len < 64 &&
             (tile->revision == candidate.revision || now - tile->loaded >= 2 * G_USEC_PER_SEC))
      g_array_append_val(chunks, request);
    else if (tile != NULL && refresh->len < 64 &&
             ((tile->revision != candidate.revision && now - tile->loaded >= 2 * G_USEC_PER_SEC) ||
              (tile->surface == NULL && now - tile->loaded >= 2 * G_USEC_PER_SEC)))
      g_array_append_val(refresh, request);
  }
  /* Missing terrain always wins over refreshing an already visible tile. */
  for (guint i = 0; i < refresh->len && chunks->len < 64; i++) {
    TerrainRequest request = g_array_index(refresh, TerrainRequest, i); g_array_append_val(chunks, request);
  }
  const char *terrain_format = failed != 0 ?
    (candidates->len > wanted ?
      g_dngettext(GETTEXT_PACKAGE,
        "Terrain / %u chunk / some saved chunks could not be decoded / zoom in for full detail",
        "Terrain / %u chunks / some saved chunks could not be decoded / zoom in for full detail", ready) :
      g_dngettext(GETTEXT_PACKAGE,
        "Terrain / %u chunk / some saved chunks could not be decoded",
        "Terrain / %u chunks / some saved chunks could not be decoded", ready)) :
    (candidates->len > wanted ?
      g_dngettext(GETTEXT_PACKAGE,
        "Terrain / %u chunk / zoom in for full detail",
        "Terrain / %u chunks / zoom in for full detail", ready) :
      g_dngettext(GETTEXT_PACKAGE, "Terrain / %u chunk", "Terrain / %u chunks", ready));
  g_autofree char *message = chunks->len != 0 ?
    g_strdup_printf(g_dngettext(GETTEXT_PACKAGE,
      "Loading terrain / %u of %u chunk", "Loading terrain / %u of %u chunks", wanted), ready, wanted) :
    g_strdup_printf(terrain_format, ready);
  gtk_label_set_text(page->terrain_status, message);
  gboolean cache_only = cached->len != 0;
  if (cache_only) { g_array_unref(chunks); chunks = g_steal_pointer(&cached); }
  if (chunks->len == 0) return;
  page->terrain_busy = TRUE;
  TerrainTask *task = g_new0(TerrainTask, 1);
  task->window = g_object_ref(page->window); task->generation = page->generation; task->region = g_strdup(world->region);
  task->cache_only = cache_only;
  task->chunks = g_steal_pointer(&chunks); task->tiles = g_ptr_array_new_with_free_func(terrain_tile_free);
  g_autoptr(GTask) operation = g_task_new(NULL, page->cancel, terrain_finished, NULL);
  g_task_set_task_data(operation, task, terrain_task_free); g_task_run_in_thread(operation, terrain_worker);
}

static void task_free(gpointer data)
{
  MapTask *task = data;
  g_clear_pointer(&task->live, pumpkin_map_snapshot_free); g_clear_pointer(&task->regions, g_ptr_array_unref);
  g_clear_pointer(&task->directories, g_ptr_array_unref);
  g_object_unref(task->server); g_object_unref(task->window);
  g_free(task->request); g_free(task->json); g_free(task->scan_error); g_free(task->live_error); g_free(task);
}
static void map_worker(GTask *result, gpointer source, gpointer data, GCancellable *cancel)
{
  (void)source;
  MapTask *task = data;
  g_autofree char *directory = pumpkin_server_get_data_dir(task->server);
  g_autoptr(GError) error = NULL;
  if (!task->scan && task->running && !g_cancellable_is_cancelled(cancel)) {
    if (pumpkin_map_write_request_cancellable(directory, task->json, cancel, &error))
      task->live = pumpkin_map_read_live(directory, task->request, now_ms(), &error);
    if (g_cancellable_is_cancelled(cancel)) pumpkin_map_stop_request(directory, task->request, NULL);
    if (error != NULL) { task->live_error = g_strdup(error->message); g_clear_error(&error); }
  }
  if (task->scan) {
    task->regions = pumpkin_map_scan_regions_watched(directory, cancel, &task->directories, &error);
    if (error != NULL) task->scan_error = g_strdup(error->message);
    g_autoptr(GBytes) bundled = g_resources_lookup_data(PUMPKIN_MAP_BRIDGE_RESOURCE, 0, NULL);
    task->bridge_access = pumpkin_map_bridge_access(directory, bundled, NULL);
  }
  g_task_return_boolean(result, TRUE);
}
static void region_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, MapPage *page)
{
  (void)monitor; (void)file; (void)other; (void)event;
  if (page->window->disposing) return;
  ++page->inventory_revision;
  page->last_scan = 0;
  if (page->rescan_at == 0) page->rescan_at = g_get_monotonic_time() + 150000;
}
static void monitor_free(gpointer data)
{
  GFileMonitor *monitor = data;
  g_signal_handlers_disconnect_by_data(monitor, g_object_get_data(G_OBJECT(monitor), "map-page"));
  g_file_monitor_cancel(monitor); g_object_unref(monitor);
}
static void watch_directory(MapPage *page, const char *path)
{
  if (g_hash_table_contains(page->monitors, path) || g_hash_table_size(page->monitors) >= 1024) return;
  g_autoptr(GFile) directory = g_file_new_for_path(path);
  GFileMonitor *monitor = g_file_monitor_directory(directory, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (monitor == NULL) return;
  g_file_monitor_set_rate_limit(monitor, 150);
  g_object_set_data(G_OBJECT(monitor), "map-page", page);
  g_signal_connect(monitor, "changed", G_CALLBACK(region_changed), page);
  g_hash_table_insert(page->monitors, g_strdup(path), monitor);
}
static void watch_worlds(MapPage *page, GPtrArray *directories)
{
  g_autofree char *base = pumpkin_server_get_data_dir(page->server);
  watch_directory(page, base);
  g_autofree char *plugins = g_build_filename(base, "plugins", NULL);
  watch_directory(page, plugins);
  for (guint i = 0; i < page->regions->len; i++) {
    PumpkinMapWorld *world = g_ptr_array_index(page->regions, i);
    g_autofree char *path = g_strdup(world->region);
    for (guint depth = 0; depth < 9 && g_strcmp0(path, base) != 0; depth++) {
      watch_directory(page, path);
      char *parent = g_path_get_dirname(path); g_free(path); path = parent;
    }
  }
  /* Watch empty dimensions too, without offering them as generated worlds. */
  if (directories != NULL) for (guint i = 0; i < directories->len; i++)
    watch_directory(page, g_ptr_array_index(directories, i));
}
static void map_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source; (void)data;
  MapTask *task = g_task_get_task_data(G_TASK(result));
  PumpkinWindow *self = task->window;
  MapPage *page = page_for(self);
  if (self->disposing || page->generation != task->generation) return;
  if (task->scan) page->scan_busy = FALSE;
  else page->busy = FALSE;
  if (!active(page) || page->server != task->server) return;
  if (task->scan) {
    g_free(page->scan_error); page->scan_error = g_strdup(task->scan_error);
    page->bridge_access = task->bridge_access;
  }
  PumpkinMapWorld *previous = chosen(page);
  g_autofree char *id = previous != NULL ? g_strdup(previous->id) : NULL;
  g_autofree char *name = previous != NULL ? g_strdup(previous->name) : NULL;
  g_autofree char *dimension = previous != NULL ? g_strdup(previous->dimension) : NULL;
  if (task->regions != NULL) {
    // Choice pointers must be dropped before their owning inventory is replaced.
    g_ptr_array_set_size(page->choices, 0);
    for (guint i = 0; i < task->regions->len;) {
      PumpkinMapWorld *next = g_ptr_array_index(task->regions, i);
      if (next->inventory_incomplete) {
        for (guint j = 0; j < page->regions->len; j++) {
          PumpkinMapWorld *old = g_ptr_array_index(page->regions, j);
          if (g_strcmp0(old->id, next->id) != 0) continue;
          g_ptr_array_index(task->regions, i) = g_ptr_array_steal_index_fast(page->regions, j);
          pumpkin_map_world_free(next); next = old; break;
        }
        /* An unreadable new region is not evidence of a generated dimension. */
        if (next->chunks->len == 0) { g_ptr_array_remove_index(task->regions, i); continue; }
      }
      ++i;
    }
    g_ptr_array_unref(page->regions); page->regions = g_steal_pointer(&task->regions);
  }
  if (task->scan) {
    page->last_scan = page->inventory_revision == task->inventory_revision ? g_get_monotonic_time() : 0;
    watch_worlds(page, task->directories);
  }
  if (!task->scan && (task->live != NULL || !pumpkin_map_snapshot_fresh(page->live, now_ms()))) {
    if (task->live != NULL)
      pumpkin_map_motion_update(page->motion, task->live, g_get_monotonic_time(),
        pumpkin_config_get_minimap_refresh_hz(page->window->config));
    else pumpkin_map_motion_clear(page->motion);
    g_clear_pointer(&page->live, pumpkin_map_snapshot_free); page->live = g_steal_pointer(&task->live);
  }
  update_virtual_worlds(page, id, task->scan);
  /* A live-only world becomes a saved world when discovery or an autosave finishes. */
  PumpkinMapWorld *current = chosen(page);
  if (name != NULL && (current == NULL || g_strcmp0(current->id, id) != 0)) {
    guint matches = 0, index = 0;
    for (guint i = 0; i < page->choices->len; i++) {
      PumpkinMapWorld *world = g_ptr_array_index(page->choices, i);
      if (g_strcmp0(world->name, name) == 0 && g_strcmp0(world->dimension, dimension) == 0) { ++matches; index = i; }
    }
    if (matches == 1) {
      page->changing_choice = TRUE; gtk_drop_down_set_selected(page->world_choice, index); page->changing_choice = FALSE;
    }
  }
  if (!page->user_view && !page->auto_located && pumpkin_map_snapshot_fresh(page->live, now_ms())) {
    for (guint p = 0; p < page->live->players->len && !page->auto_located; p++) {
      PumpkinMapPlayer *player = g_ptr_array_index(page->live->players, p);
      for (guint i = 0; i < page->choices->len; i++) {
        PumpkinMapWorld *world = g_ptr_array_index(page->choices, i);
        if (!pumpkin_map_player_matches(player, world) || live_world(page, world) == NULL) continue;
        page->changing_choice = TRUE; gtk_drop_down_set_selected(page->world_choice, i); page->changing_choice = FALSE;
        page->center_x = player->x; page->center_z = player->z; page->auto_located = TRUE; break;
      }
    }
  }
  PumpkinMapPlayer *tracked = pumpkin_map_snapshot_fresh(page->live, now_ms()) ?
    pumpkin_map_find_player(page->live, page->tracked) : NULL;
  if (page->live != NULL && page->tracked != NULL && tracked == NULL) stop_tracking(NULL, page);
  if (tracked != NULL) for (guint i = 0; i < page->choices->len; i++) {
    PumpkinMapWorld *world = g_ptr_array_index(page->choices, i);
    if (pumpkin_map_player_matches(tracked, world) && live_world(page, world) != NULL) {
      page->changing_choice = TRUE;
      gtk_drop_down_set_selected(page->world_choice, i);
      page->changing_choice = FALSE;
      break;
    }
  }
  if (page->action_id != NULL && page->live != NULL && g_strcmp0(page->live->action_id, page->action_id) == 0) {
    set_details_status(self, page->live->action_message != NULL ? page->live->action_message : _("Map action finished"), 5);
    g_clear_pointer(&page->action_id, g_free); g_clear_pointer(&page->action_json, g_free);
  }
  if (page->scan_error != NULL) gtk_label_set_text(page->status, page->scan_error);
  else if (page->live == NULL) gtk_label_set_text(page->status, page->bridge_error != NULL ? page->bridge_error :
    task->running ? _("Player positions unavailable / saved terrain only") : _("Server stopped / saved terrain"));
  else {
    g_clear_pointer(&page->bridge_error, g_free);
    g_autofree char *status = g_strdup_printf(_("Live / %u online"), page->live->players->len);
    gtk_label_set_text(page->status, status);
  }
  gtk_widget_set_tooltip_text(GTK_WIDGET(page->status), task->live_error);
  sync_world_tabs(page);
  pumpkin_window_minimap_update(self);
  load_terrain(page);
  PumpkinMapWorld *border = live_world(page, chosen(page));
  if (page->frame == 0 && (pumpkin_map_motion_active(page->motion, g_get_monotonic_time()) ||
      (border != NULL && border->transition_known && now_ms() < border->transition_at_ms + border->transition_ms)))
    page->frame = gtk_widget_add_tick_callback(GTK_WIDGET(page->canvas), animate_map, page, NULL);
  update_player_card(page);
  gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
}
static char *request_json(MapPage *page)
{
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 1); cJSON_AddStringToObject(root, "request", page->request);
  cJSON_AddNumberToObject(root, "expires_at_ms", (double)(now_ms() + 5000));
  cJSON_AddNumberToObject(root, "refresh_hz", pumpkin_config_get_minimap_refresh_hz(page->window->config));
  if (page->action_json != NULL) cJSON_AddItemToObject(root, "action", cJSON_Parse(page->action_json));
  char *serialized = cJSON_PrintUnformatted(root); char *copy = g_strdup(serialized);
  cJSON_free(serialized); cJSON_Delete(root); return copy;
}
static gboolean poll_map(gpointer data)
{
  MapPage *page = data;
  if (!active(page)) return G_SOURCE_CONTINUE;
  pumpkin_window_minimap_update(page->window);
  if (page->server == NULL) return G_SOURCE_CONTINUE;
  if (page->action_id != NULL && g_get_monotonic_time() - page->action_started > 10 * G_USEC_PER_SEC) {
    set_details_status(page->window, _("Map action was not acknowledged; check server logs before retrying"), 6);
    g_clear_pointer(&page->action_id, g_free); g_clear_pointer(&page->action_json, g_free);
  }
  if (page->live != NULL && !pumpkin_map_snapshot_fresh(page->live, now_ms())) {
    PumpkinMapWorld *world = chosen(page);
    g_autofree char *id = world != NULL ? g_strdup(world->id) : NULL;
    g_clear_pointer(&page->live, pumpkin_map_snapshot_free);
    pumpkin_map_motion_clear(page->motion);
    update_virtual_worlds(page, id, FALSE);
    gtk_popover_popdown(page->popover); stop_tracking(NULL, page);
    gtk_widget_queue_draw(GTK_WIDGET(page->canvas));
  }
  load_terrain(page);
  gint64 now = g_get_monotonic_time();
  for (guint kind = 0; kind < 2; kind++) {
    gboolean scan = kind == 0;
    if (!scan && (!pumpkin_server_get_running(page->server) || pumpkin_server_get_stopping(page->server))) continue;
    if (scan ? page->scan_busy || now < page->rescan_at ||
                 (page->last_scan != 0 && now - page->last_scan < 10 * G_USEC_PER_SEC) :
               page->busy || now - page->last_live < G_USEC_PER_SEC /
                 pumpkin_config_get_minimap_refresh_hz(page->window->config)) continue;
    if (scan) { page->scan_busy = TRUE; page->rescan_at = 0; }
    else { page->busy = TRUE; page->last_live = now; }
    MapTask *task = g_new0(MapTask, 1);
    task->window = g_object_ref(page->window); task->server = g_object_ref(page->server);
    task->generation = page->generation; task->request = g_strdup(page->request);
    task->inventory_revision = page->inventory_revision;
    task->json = request_json(page); task->scan = scan;
    task->running = pumpkin_server_get_running(page->server) && !pumpkin_server_get_stopping(page->server);
    g_autoptr(GTask) operation = g_task_new(NULL, page->cancel, map_finished, NULL);
    g_task_set_task_data(operation, task, task_free); g_task_run_in_thread(operation, map_worker);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean animate_map(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)clock;
  MapPage *page = data;
  PumpkinMapWorld *border = live_world(page, chosen(page));
  gtk_widget_queue_draw(widget);
  if (active(page) && (pumpkin_map_motion_active(page->motion, g_get_monotonic_time()) ||
      (border != NULL && border->transition_known && now_ms() < border->transition_at_ms + border->transition_ms)))
    return G_SOURCE_CONTINUE;
  page->frame = 0;
  return G_SOURCE_REMOVE;
}

typedef struct { PumpkinWindow *window; PumpkinServer *server; char *id, *name; gboolean ban; } ActionContext;
static void action_context_free(ActionContext *context)
{
  g_object_unref(context->window); g_object_unref(context->server); g_free(context->id); g_free(context->name); g_free(context);
}
static void action_confirmed(GObject *source, GAsyncResult *result, gpointer data)
{
  ActionContext *context = data;
  MapPage *page = page_for(context->window);
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  PumpkinMapPlayer *player = !context->window->disposing && active(page) ? pumpkin_map_find_player(page->live, context->id) : NULL;
  if (g_strcmp0(response, "apply") == 0 && page->server == context->server &&
      page->action_id == NULL && pumpkin_map_snapshot_fresh(page->live, now_ms()) &&
      player != NULL && g_strcmp0(player->name, context->name) == 0 && pumpkin_server_get_running(context->server) &&
      !pumpkin_server_get_stopping(context->server)) {
    GtkWidget *entry = g_object_get_data(source, "map-reason");
    g_autofree char *reason = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    g_strstrip(reason);
    cJSON *action = cJSON_CreateObject();
    page->action_id = g_uuid_string_random();
    cJSON_AddStringToObject(action, "id", page->action_id);
    cJSON_AddStringToObject(action, "player", context->id); cJSON_AddStringToObject(action, "name", context->name);
    cJSON_AddStringToObject(action, "type", context->ban ? "ban" : "kick");
    cJSON_AddStringToObject(action, "reason", reason);
    char *json = cJSON_PrintUnformatted(action); page->action_json = g_strdup(json);
    cJSON_free(json); cJSON_Delete(action); page->action_started = g_get_monotonic_time();
    poll_map(page);
  }
  action_context_free(context);
}
static void moderate_clicked(GtkButton *button, MapPage *page)
{
  PumpkinMapPlayer *player = selected_player(page);
  if (player == NULL || page->action_id != NULL || !active(page)) return;
  ActionContext *context = g_new0(ActionContext, 1);
  context->window = g_object_ref(page->window); context->server = g_object_ref(page->server);
  context->id = g_strdup(player->id); context->name = g_strdup(player->name);
  context->ban = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "map-ban"));
  g_autofree char *title = g_strdup_printf(context->ban ? _("Ban %s?") : _("Kick %s?"), player->name);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, context->ban ?
    _("This player will be disconnected and prevented from joining again.") : _("This player can reconnect after being disconnected.")));
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "apply", context->ban ? _("Ban Player") : _("Kick Player"), NULL);
  adw_alert_dialog_set_close_response(dialog, "cancel"); adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_response_appearance(dialog, "apply", ADW_RESPONSE_DESTRUCTIVE);
  GtkWidget *entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("Reason (optional)"));
  gtk_entry_set_max_length(GTK_ENTRY(entry), 120); adw_alert_dialog_set_extra_child(dialog, entry);
  g_object_set_data(G_OBJECT(dialog), "map-reason", entry);
  gtk_popover_popdown(page->popover);
  adw_alert_dialog_choose(dialog, GTK_WIDGET(page->window), NULL, action_confirmed, context);
}

typedef struct { PumpkinWindow *window; PumpkinServer *server; GBytes *bytes; gboolean upgrade, automatic; } InstallTask;
static gboolean can_install_bridge(MapPage *page)
{
  PumpkinWindow *self = page->window;
  PumpkinServer *server = page->server;
  if (server == NULL || self->disposing || self->current != server ||
      !pumpkin_platform_supports_local_hosting() || server_is_running_ui(self, server) ||
      pumpkin_server_get_stopping(server) || pumpkin_server_get_maintenance(server) ||
      self->restart_requested || self->restart_delay_id != 0 || self->start_delay_id != 0) return FALSE;
  DownloadProgressState *download = get_download_progress_state(self, server, FALSE);
  return download == NULL || !download->active;
}
static void install_task_free(gpointer data)
{
  InstallTask *task = data;
  g_object_unref(task->window); g_object_unref(task->server); g_bytes_unref(task->bytes); g_free(task);
}
static void install_worker(GTask *result, gpointer source, gpointer data, GCancellable *cancel)
{
  (void)source;
  InstallTask *task = data;
  g_autoptr(GError) error = NULL;
  if (task->upgrade) {
    g_autofree char *data_dir = pumpkin_server_get_data_dir(task->server);
    if (pumpkin_map_bridge_upgrade(data_dir, task->bytes, &error)) g_task_return_boolean(result, TRUE);
    else g_task_return_error(result, g_steal_pointer(&error));
    return;
  }
  g_autofree char *folder = pumpkin_server_get_plugins_dir(task->server);
  g_autofree char *path = g_build_filename(folder, PUMPKIN_MAP_BRIDGE_FILENAME, NULL);
  g_autofree char *data_dir = pumpkin_server_get_data_dir(task->server);
  if (pumpkin_map_bridge_present(data_dir)) {
    /* Error messages stay in source English; user-facing prefixes translate. */
    g_task_return_new_error(result, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "Map Bridge is already installed; it has not been overwritten");
    return;
  }
  if (g_file_test(folder, G_FILE_TEST_IS_SYMLINK) || !g_file_test(folder, G_FILE_TEST_IS_DIR)) {
    g_task_return_new_error(result, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Plugin folder must be an ordinary local directory");
    return;
  }
  g_autofree char *uuid = g_uuid_string_random();
  g_autofree char *temporary = g_strdup_printf("%s/.map-bridge-%s.tmp", folder, uuid);
  g_autoptr(GFile) staging = g_file_new_for_path(temporary);
  g_autoptr(GFile) file = g_file_new_for_path(path);
  gsize length = 0; const char *contents = g_bytes_get_data(task->bytes, &length);
  g_autoptr(GFileOutputStream) stream = g_file_create(staging, G_FILE_CREATE_PRIVATE, cancel, &error);
  gboolean ok = stream != NULL && g_output_stream_write_all(G_OUTPUT_STREAM(stream), contents, length, NULL, cancel, &error) &&
    g_output_stream_close(G_OUTPUT_STREAM(stream), cancel, &error);
  if (ok) ok = g_file_move(staging, file, G_FILE_COPY_NONE, cancel, NULL, NULL, &error);
  if (!ok) {
    if (stream != NULL) g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, NULL);
    g_file_delete(staging, NULL, NULL);
    g_task_return_error(result, g_steal_pointer(&error));
  } else g_task_return_boolean(result, TRUE);
}
static void install_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source; (void)data;
  InstallTask *task = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;
  gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
  MapPage *page = page_for(task->window);
  /* Keep the reentrancy guard through maintenance's synchronous state signals. */
  pumpkin_server_end_maintenance(task->server);
  if (!task->window->disposing && task->window->current == task->server) {
    page->bridge_updating = FALSE;
    if (ok) {
      page->bridge_access = PUMPKIN_MAP_BRIDGE_UNKNOWN; page->last_scan = 0;
      g_clear_pointer(&page->bridge_error, g_free);
    } else if (task->automatic) {
      g_free(page->bridge_error);
      page->bridge_error = g_strdup_printf(_("Could not update Minimap Bridge: %s. Retry below while the server is stopped."), error->message);
    }
    if (!task->automatic || !ok)
      set_details_status(task->window, ok ? _("Map Bridge installed.") : error->message, 6);
    pumpkin_window_minimap_update(task->window);
  }
}
static void update_bridge_automatically(MapPage *page)
{
  if (page->bridge_updating || page->bridge_update_attempted ||
      page->bridge_access != PUMPKIN_MAP_BRIDGE_OUTDATED || !can_install_bridge(page) ||
      !pumpkin_config_get_minimap_consent(page->window->config) ||
      !pumpkin_config_get_minimap_enabled(page->window->config)) return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_resources_lookup_data(PUMPKIN_MAP_BRIDGE_RESOURCE, 0, &error);
  page->bridge_update_attempted = TRUE;
  page->bridge_updating = TRUE;
  if (bytes == NULL || !pumpkin_server_begin_maintenance(page->server, &error)) {
    page->bridge_updating = FALSE;
    g_free(page->bridge_error); page->bridge_error = g_strdup(error->message);
    return;
  }
  InstallTask *task = g_new0(InstallTask, 1);
  task->window = g_object_ref(page->window); task->server = g_object_ref(page->server);
  task->bytes = g_bytes_ref(bytes); task->upgrade = task->automatic = TRUE;
  g_autoptr(GTask) operation = g_task_new(NULL, NULL, install_finished, NULL);
  g_task_set_task_data(operation, task, install_task_free); g_task_run_in_thread(operation, install_worker);
}
gboolean pumpkin_window_minimap_prepare_start(PumpkinWindow *self, PumpkinServer *server)
{
  gboolean enabled = pumpkin_config_get_minimap_consent(self->config) &&
    pumpkin_config_get_minimap_enabled(self->config);
  pumpkin_minimap_bridge_set_enabled(server, enabled);
  if (!enabled) return TRUE;
  if (server_is_running_ui(self, server) || pumpkin_server_get_maintenance(server)) return FALSE;
  g_autoptr(GBytes) bytes = g_resources_lookup_data(PUMPKIN_MAP_BRIDGE_RESOURCE, 0, NULL);
  g_autofree char *directory = pumpkin_server_get_data_dir(server);
  if (bytes == NULL || pumpkin_map_bridge_access(directory, bytes, NULL) != PUMPKIN_MAP_BRIDGE_OUTDATED) return TRUE;
  MapPage *page = page_for(self);
  gboolean selected = page != NULL && page->server == server;
  if (selected) { page->bridge_updating = TRUE; page->bridge_update_attempted = TRUE; }
  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_begin_maintenance(server, &error)) {
    if (selected) page->bridge_updating = FALSE;
    append_log_for_server(self, server, error->message); return FALSE;
  }
  /* This bounded local preflight also covers starts without opening the map,
   * network actions, automatic launch and restarts. It never installs a missing bridge. */
  gboolean ok = pumpkin_map_bridge_upgrade(directory, bytes, &error);
  pumpkin_server_end_maintenance(server);
  if (selected) {
    page->bridge_updating = FALSE; page->last_scan = 0;
    g_clear_pointer(&page->bridge_error, g_free);
    if (ok) page->bridge_access = PUMPKIN_MAP_BRIDGE_UNKNOWN;
    else page->bridge_error = g_strdup_printf(_("Could not update Minimap Bridge: %s"), error->message);
  }
  g_autofree char *notice = ok ? g_strdup(_("Minimap Bridge updated to the version bundled with this app.")) :
    g_strdup_printf(_("Minimap Bridge update failed; the existing plugin was retained: %s"), error->message);
  g_autofree char *message = g_strdup_printf("[SMPK] %s", notice);
  append_log_for_server(self, server, message);
  /* A map-helper failure must not prevent an otherwise valid server from starting. */
  return TRUE;
}
static void install_confirmed(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(PumpkinWindow) self = data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  PumpkinServer *server = g_object_get_data(source, "map-server");
  if (self->disposing || g_strcmp0(response, "install") != 0 || self->current != server ||
      !can_install_bridge(page_for(self))) return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_resources_lookup_data(PUMPKIN_MAP_BRIDGE_RESOURCE, 0, &error);
  if (bytes == NULL || !pumpkin_server_begin_maintenance(server, &error)) {
    set_details_status(self, error->message, 6); return;
  }
  InstallTask *task = g_new0(InstallTask, 1);
  task->window = g_object_ref(self); task->server = g_object_ref(server); task->bytes = g_bytes_ref(bytes);
  task->upgrade = GPOINTER_TO_INT(g_object_get_data(source, "map-upgrade"));
  g_autoptr(GTask) operation = g_task_new(NULL, NULL, install_finished, NULL);
  g_task_set_task_data(operation, task, install_task_free); g_task_run_in_thread(operation, install_worker);
}
static void install_clicked(GtkButton *button, MapPage *page)
{
  (void)button;
  if (!can_install_bridge(page)) return;
  gboolean upgrade = page->bridge_access == PUMPKIN_MAP_BRIDGE_OUTDATED;
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(upgrade ? _("Update Map Bridge?") : _("Install Map Bridge?"),
    _("This optional plugin reads live player coordinates and ping. It can kick or ban a player only when you confirm an action. "
      "It requires access to its own data folder, not networking. After installation, use Allow Data Access here while the server is stopped. "
      "Existing signature checks are not disabled. Other Pumpkin builds may require a matching Bridge API version.")));
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "install", upgrade ? _("Update") : _("Install"), NULL);
  adw_alert_dialog_set_close_response(dialog, "cancel"); adw_alert_dialog_set_default_response(dialog, "cancel");
  g_object_set_data_full(G_OBJECT(dialog), "map-server", g_object_ref(page->server), g_object_unref);
  g_object_set_data(G_OBJECT(dialog), "map-upgrade", GINT_TO_POINTER(upgrade));
  adw_alert_dialog_choose(dialog, GTK_WIDGET(page->window), NULL, install_confirmed, g_object_ref(page->window));
}

static void approve_worker(GTask *result, gpointer source, gpointer data, GCancellable *cancel)
{
  (void)source; (void)cancel;
  InstallTask *task = data;
  g_autofree char *directory = pumpkin_server_get_data_dir(task->server);
  g_autoptr(GError) error = NULL;
  if (!pumpkin_map_bridge_approve(directory, task->bytes, &error)) g_task_return_error(result, g_steal_pointer(&error));
  else g_task_return_boolean(result, TRUE);
}
static void approve_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source; (void)data;
  InstallTask *task = g_task_get_task_data(G_TASK(result));
  g_autoptr(GError) error = NULL;
  gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
  pumpkin_server_end_maintenance(task->server);
  if (task->window->disposing || task->window->current != task->server) return;
  MapPage *page = page_for(task->window);
  if (ok) {
    page->bridge_access = PUMPKIN_MAP_BRIDGE_APPROVED;
    g_clear_pointer(&page->bridge_error, g_free); page->last_scan = 0;
  }
  set_details_status(task->window, ok ? _("Map Bridge data access allowed.") : error->message, 6);
  pumpkin_window_minimap_update(task->window);
}
static void approve_confirmed(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(PumpkinWindow) self = data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  PumpkinServer *server = g_object_get_data(source, "map-server");
  if (self->disposing || g_strcmp0(response, "allow") != 0 || self->current != server ||
      !can_install_bridge(page_for(self))) return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_resources_lookup_data(PUMPKIN_MAP_BRIDGE_RESOURCE, 0, &error);
  if (bytes == NULL || !pumpkin_server_begin_maintenance(server, &error)) {
    set_details_status(self, error->message, 6); return;
  }
  InstallTask *task = g_new0(InstallTask, 1);
  task->window = g_object_ref(self); task->server = g_object_ref(server); task->bytes = g_bytes_ref(bytes);
  g_autoptr(GTask) operation = g_task_new(NULL, NULL, approve_finished, NULL);
  g_task_set_task_data(operation, task, install_task_free); g_task_run_in_thread(operation, approve_worker);
}
static void approve_clicked(GtkButton *button, MapPage *page)
{
  (void)button;
  if (!can_install_bridge(page)) return;
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
    _("Allow Map Bridge Data Access?"),
    /* Translators: Keep the permission identifier fs.write.data unchanged. */
    _("Allow fs.write.data for the exact Map Bridge bundled with Smashed Pumpkin? "
      "This lets it exchange live player positions, ping and confirmed moderation actions with this app through its own data folder. "
      "No network permission is granted. Only this plugin's saved permission decision is updated; other plugins, blocked permissions "
      "and signature checks stay unchanged.")));
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "allow", _("Allow Data Access"), NULL);
  adw_alert_dialog_set_close_response(dialog, "cancel"); adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_response_appearance(dialog, "allow", ADW_RESPONSE_SUGGESTED);
  g_object_set_data_full(G_OBJECT(dialog), "map-server", g_object_ref(page->server), g_object_unref);
  adw_alert_dialog_choose(dialog, GTK_WIDGET(page->window), NULL, approve_confirmed, g_object_ref(page->window));
}

static void bridge_log_line(PumpkinServer *server, const char *line, MapPage *page)
{
  if (server != page->server || page->window->disposing || line == NULL ||
      strstr(line, "map-bridge") == NULL) return;
  gboolean denied = strstr(line, "Permission denied") != NULL || strstr(line, "approved: false") != NULL;
  if (!denied && strstr(line, "Failed to load plugin") == NULL) return;
  if (denied) page->bridge_access = PUMPKIN_MAP_BRIDGE_DENIED;
  g_free(page->bridge_error);
  page->bridge_error = g_strdup(denied ? _("Pumpkin denied Map Bridge data access. Stop the server, allow access below, then start it again.") :
    strstr(line, "different iteration of the API") != NULL ?
    _("Map Bridge is incompatible with this Pumpkin build. Rebuild it against the matching plugin API.") :
    _("Map Bridge failed to load. Check plugin permissions, signature policy and the server console."));
  if (active(page)) gtk_label_set_text(page->status, page->bridge_error);
}
void pumpkin_window_minimap_update(PumpkinWindow *self)
{
  MapPage *page = page_for(self);
  if (page == NULL || self->disposing) return;
  if (!pumpkin_config_get_minimap_enabled(self->config) || !pumpkin_config_get_minimap_consent(self->config)) return;
  int pid = self->current != NULL ? pumpkin_server_get_pid(self->current) : 0;
  if (page->server != self->current || page->pid != pid) {
    gboolean different_server = page->server != self->current;
    PumpkinMapWorld *previous = chosen(page);
    g_autofree char *previous_id = previous != NULL ? g_strdup(previous->id) : NULL;
    ++page->generation; g_cancellable_cancel(page->cancel); g_clear_object(&page->cancel); page->cancel = g_cancellable_new();
    page->busy = page->scan_busy = page->terrain_busy = FALSE; page->last_scan = page->last_live = 0; page->pid = pid;
    if (different_server) {
      g_hash_table_remove_all(page->monitors);
      page->rescan_at = 0; page->bridge_access = PUMPKIN_MAP_BRIDGE_UNKNOWN;
      page->user_view = page->auto_located = FALSE;
      page->bridge_updating = page->bridge_update_attempted = FALSE;
    }
    if (page->server != NULL) g_signal_handlers_disconnect_by_func(page->server, bridge_log_line, page);
    g_set_object(&page->server, self->current);
    if (page->server != NULL) g_signal_connect(page->server, "log-line", G_CALLBACK(bridge_log_line), page);
    g_clear_pointer(&page->bridge_error, g_free); g_clear_pointer(&page->scan_error, g_free);
    g_free(page->request); page->request = g_uuid_string_random();
    g_clear_pointer(&page->live, pumpkin_map_snapshot_free);
    pumpkin_map_motion_clear(page->motion);
    g_ptr_array_set_size(page->choices, 0);
    if (different_server) g_ptr_array_set_size(page->regions, 0);
    g_ptr_array_set_size(page->virtual_worlds, 0);
    g_clear_pointer(&page->action_id, g_free); g_clear_pointer(&page->action_json, g_free);
    g_clear_pointer(&page->selected, g_free); stop_tracking(NULL, page);
    gtk_popover_popdown(page->popover); update_choices(page, different_server ? NULL : previous_id);
  }
  update_bridge_automatically(page);
  gtk_widget_set_sensitive(page->install, can_install_bridge(page));
  gtk_widget_set_sensitive(page->approve, can_install_bridge(page));
  if (page->server != NULL) {
    g_autofree char *data_dir = pumpkin_server_get_data_dir(page->server);
    gboolean installed = pumpkin_map_bridge_present(data_dir);
    gboolean live = pumpkin_map_snapshot_fresh(page->live, now_ms());
    gboolean running = pumpkin_server_get_running(page->server);
    gboolean outdated = page->bridge_access == PUMPKIN_MAP_BRIDGE_OUTDATED;
    gtk_button_set_label(GTK_BUTTON(page->install), outdated ? _("Update Map Bridge...") : _("Install Map Bridge..."));
    gtk_widget_set_visible(page->install, (outdated && page->bridge_update_attempted && !page->bridge_updating && !running) || (!installed && !live));
    gboolean needs_access = installed && !live && (page->bridge_access == PUMPKIN_MAP_BRIDGE_UNKNOWN ||
      page->bridge_access == PUMPKIN_MAP_BRIDGE_DENIED);
    gtk_widget_set_visible(page->approve, needs_access);
    gtk_widget_set_visible(page->footer, gtk_widget_get_visible(page->install) || needs_access);
    gtk_widget_set_tooltip_text(page->approve, running ? _("Stop the server before allowing data access") :
      _("Allow only the bundled Map Bridge to use its own data folder"));
    gtk_widget_set_tooltip_text(page->install, running ? _("Stop the server before installing Map Bridge") : _("Install the optional live-player bridge"));
    gboolean show_banner = outdated || (!live && running) || needs_access || !installed || page->bridge_error != NULL;
    adw_banner_set_revealed(page->bridge_banner, show_banner);
    gtk_widget_set_margin_bottom(GTK_WIDGET(page->bridge_banner), show_banner ? 10 : 0);
    adw_banner_set_title(page->bridge_banner, page->bridge_error != NULL ? page->bridge_error :
      page->bridge_access == PUMPKIN_MAP_BRIDGE_DENIED ? _("Map Bridge data access was denied. Stop the server and allow access below.") :
      page->bridge_access == PUMPKIN_MAP_BRIDGE_INVALID ? _("Map Bridge permission cache could not be read safely. Check server logs and permissions.") :
      outdated ? (page->bridge_updating ? _("Updating Minimap Bridge...") :
        _("Minimap Bridge will update automatically before the next server start.")) :
      page->bridge_access == PUMPKIN_MAP_BRIDGE_UNTRUSTED ? _("Installed Map Bridge differs from the bundled version. Check plugin compatibility and permissions.") : !installed ?
      (running ? _("Map Bridge is not installed. Stop the server to install it and enable live players.") :
                 _("Map Bridge is not installed. Install it below to enable live players.")) :
      needs_access ? _("Map Bridge needs data access. Stop the server and use Allow Data Access below.") :
      _("Map Bridge has not provided live data. Check the server console for compatibility or blocked permissions."));
  }
}
static void enabled_changed(GObject *object, GParamSpec *pspec, MapPage *page)
{
  (void)object; (void)pspec;
  gboolean enabled = adw_switch_row_get_active(page->enabled);
  if (enabled && !pumpkin_config_get_minimap_consent(page->window->config)) {
    g_signal_handlers_block_by_func(page->enabled, enabled_changed, page);
    adw_switch_row_set_active(page->enabled, FALSE);
    g_signal_handlers_unblock_by_func(page->enabled, enabled_changed, page);
    request_consent(page); return;
  }
  gtk_widget_set_sensitive(GTK_WIDGET(page->refresh_rate), enabled);
  pumpkin_config_set_minimap_enabled(page->window->config, enabled);
  pumpkin_config_save(page->window->config, NULL);
  gtk_stack_set_visible_child_name(page->map_view, enabled ? "content" : "disabled");
  if (!enabled) {
    if (page->timer != 0) { g_source_remove(page->timer); page->timer = 0; }
    if (page->frame != 0) { gtk_widget_remove_tick_callback(GTK_WIDGET(page->canvas), page->frame); page->frame = 0; }
    ++page->generation; g_cancellable_cancel(page->cancel);
    if (page->server != NULL) {
      g_autofree char *directory = pumpkin_server_get_data_dir(page->server);
      pumpkin_map_stop_request(directory, page->request, NULL);
    }
    g_hash_table_remove_all(page->monitors);
    g_hash_table_remove_all(page->terrain);
    pumpkin_map_motion_clear(page->motion);
    g_clear_pointer(&page->live, pumpkin_map_snapshot_free);
    g_ptr_array_set_size(page->choices, 0); g_ptr_array_set_size(page->virtual_worlds, 0); g_ptr_array_set_size(page->regions, 0);
    update_choices(page, NULL);
    g_clear_pointer(&page->selected, g_free);
    g_clear_pointer(&page->action_id, g_free); g_clear_pointer(&page->action_json, g_free);
    gtk_popover_popdown(page->popover); stop_tracking(NULL, page);
    page->busy = page->scan_busy = page->terrain_busy = FALSE;
    page->last_scan = page->last_live = 0;
    page->rescan_at = page->last_terrain = 0; page->auto_located = page->user_view = FALSE;
    g_clear_object(&page->cancel); page->cancel = g_cancellable_new();
    g_free(page->request); page->request = g_uuid_string_random();
  }
  GListModel *servers = pumpkin_server_store_get_model(page->window->store);
  for (guint i = 0; i < g_list_model_get_n_items(servers); ++i) {
    g_autoptr(PumpkinServer) server = g_list_model_get_item(servers, i);
    pumpkin_minimap_bridge_set_enabled(server, enabled);
  }
  if (enabled && active(page) && page->timer == 0) page->timer = g_timeout_add(25, poll_map, page);
}
static void consent_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(PumpkinWindow) self = data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  if (self->disposing) return;
  MapPage *page = page_for(self); page->consent_open = FALSE;
  if (g_strcmp0(response, "enable") == 0) {
    pumpkin_config_set_minimap_consent(self->config, TRUE);
    adw_switch_row_set_active(page->enabled, TRUE);
    pumpkin_config_save(self->config, NULL);
  } else if (g_strcmp0(adw_view_stack_get_visible_child_name(self->details_stack), "minimap") == 0)
    adw_view_stack_set_visible_child_name(self->details_stack, "console");
}
static void request_consent(MapPage *page)
{
  if (page->consent_open || page->window->disposing ||
      pumpkin_config_get_minimap_consent(page->window->config) || !pumpkin_platform_supports_local_hosting()) return;
  page->consent_open = TRUE;
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Enable the Minimap?"),
    _("The minimap increases CPU, memory and disk activity. Higher player update rates use more resources. "
      "It uses the local Map Bridge plugin to send player names, positions, viewing direction, ping and world information "
      "to Smashed Pumpkin through the plugin's own data folder. This data stays on this computer; it is not sent to an external service. "
      "The map reads saved terrain without generating or loading server chunks. "
      "Bridge installation and data access are confirmed separately while the server is stopped. "
      "You can disable the minimap at any time in Settings.")));
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "enable", _("Agree and Enable"), NULL);
  adw_alert_dialog_set_close_response(dialog, "cancel"); adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_response_appearance(dialog, "enable", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_choose(dialog, GTK_WIDGET(page->window), NULL, consent_finished, g_object_ref(page->window));
}
static void map_mapped(GtkWidget *widget, MapPage *page)
{ (void)widget; request_consent(page); }
static void map_content_mapped(GtkWidget *widget, MapPage *page)
{
  (void)widget;
  if (active(page) && page->timer == 0) page->timer = g_timeout_add(25, poll_map, page);
}
static void map_content_unmapped(GtkWidget *widget, MapPage *page)
{
  (void)widget;
  if (page->timer != 0) { g_source_remove(page->timer); page->timer = 0; }
  if (page->frame != 0) { gtk_widget_remove_tick_callback(GTK_WIDGET(page->canvas), page->frame); page->frame = 0; }
}
static void refresh_changed(GtkRange *range, MapPage *page)
{
  pumpkin_config_set_minimap_refresh_hz(page->window->config, (guint)gtk_range_get_value(range));
  pumpkin_config_save(page->window->config, NULL); page->last_live = 0;
}
static void enable_clicked(GtkButton *button, MapPage *page)
{
  (void)button;
  if (!pumpkin_platform_supports_local_hosting()) return;
  adw_switch_row_set_active(page->enabled, TRUE);
  adw_view_stack_set_visible_child_name(page->window->details_stack, "minimap");
}
static void page_free(gpointer data)
{
  MapPage *page = data;
  g_clear_object(&page->server); g_clear_object(&page->cancel);
  g_hash_table_unref(page->terrain);
  pumpkin_map_motion_free(page->motion);
  g_hash_table_unref(page->monitors);
  g_ptr_array_unref(page->regions); g_ptr_array_unref(page->choices); g_ptr_array_unref(page->virtual_worlds); pumpkin_map_snapshot_free(page->live);
  g_free(page->request); g_free(page->selected); g_free(page->tracked); g_free(page->action_id); g_free(page->action_json);
  g_free(page->bridge_error); g_free(page->scan_error); g_free(page);
}
void pumpkin_window_minimap_shutdown(PumpkinWindow *self)
{
  MapPage *page = page_for(self); if (page == NULL) return;
  ++page->generation; g_cancellable_cancel(page->cancel);
  g_hash_table_remove_all(page->monitors);
  if (page->server != NULL) g_signal_handlers_disconnect_by_func(page->server, bridge_log_line, page);
  if (page->timer != 0) { g_source_remove(page->timer); page->timer = 0; }
  if (page->frame != 0) { gtk_widget_remove_tick_callback(GTK_WIDGET(page->canvas), page->frame); page->frame = 0; }
  if (page->popover != NULL) {
    gtk_widget_unparent(GTK_WIDGET(page->popover)); page->popover = NULL;
  }
}
void pumpkin_window_minimap_set_enabled(PumpkinWindow *self, gboolean enabled)
{
  MapPage *page = page_for(self);
  if (page != NULL) adw_switch_row_set_active(page->enabled, enabled);
}
static GtkWidget *button(const char *icon, const char *tooltip)
{
  GtkWidget *widget = gtk_button_new_from_icon_name(icon); gtk_widget_set_tooltip_text(widget, tooltip); return widget;
}
static void world_label_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory; (void)data;
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_list_item_set_child(item, label);
}
static void world_label_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory; (void)data;
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
    gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item))));
  gtk_widget_set_tooltip_text(gtk_list_item_get_child(item),
    gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item))));
}
void pumpkin_window_minimap_setup(PumpkinWindow *self)
{
  MapPage *page = g_new0(MapPage, 1); page->window = self; page->scale = 2;
  page->motion = pumpkin_map_motion_new();
  page->terrain = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, terrain_tile_free);
  page->monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, monitor_free);
  page->bridge_access = PUMPKIN_MAP_BRIDGE_UNKNOWN;
  page->cancel = g_cancellable_new(); page->regions = g_ptr_array_new_with_free_func(pumpkin_map_world_free);
  page->virtual_worlds = g_ptr_array_new_with_free_func(pumpkin_map_world_free);
  page->choices = g_ptr_array_new(); page->request = g_uuid_string_random();
  g_object_set_data_full(G_OBJECT(self), "minimap-page", page, page_free);
  page->map = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  page->bridge_banner = ADW_BANNER(adw_banner_new(_("Live players require Map Bridge")));
  gtk_box_append(GTK_BOX(page->map), GTK_WIDGET(page->bridge_banner));
  GtkWidget *overlay = gtk_overlay_new();
  gtk_widget_set_vexpand(overlay, TRUE); gtk_widget_set_hexpand(overlay, TRUE);
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_HIDDEN); gtk_widget_add_css_class(overlay, "minimap-canvas");
  gtk_box_append(GTK_BOX(page->map), overlay);
  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign(toolbar, GTK_ALIGN_START); gtk_widget_set_valign(toolbar, GTK_ALIGN_START);
  gtk_widget_set_margin_start(toolbar, 12); gtk_widget_set_margin_top(toolbar, 12);
  gtk_widget_add_css_class(toolbar, "minimap-controls"); gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toolbar);
  page->world_tabs = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_add_css_class(GTK_WIDGET(page->world_tabs), "linked"); gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(page->world_tabs));
  page->world_choice = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(world_label_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(world_label_bind), NULL);
  gtk_drop_down_set_factory(page->world_choice, factory); g_object_unref(factory);
  gtk_accessible_update_property(GTK_ACCESSIBLE(page->world_choice), GTK_ACCESSIBLE_PROPERTY_LABEL, _("World and dimension"), -1);
  gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(page->world_choice));
  page->stop_tracking = button("media-playback-stop-symbolic", _("Stop following player"));
  gtk_widget_set_visible(page->stop_tracking, FALSE); gtk_box_append(GTK_BOX(toolbar), page->stop_tracking);
  page->player_menu = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(page->player_menu, "avatar-default-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(page->player_menu), _("Locate online player"));
  gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(page->player_menu));
  GtkWidget *players_popover = gtk_popover_new(), *players_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(players_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(players_scroll), 280);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(players_scroll), TRUE);
  gtk_widget_set_size_request(players_scroll, 200, -1);
  page->player_choices = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(players_scroll), GTK_WIDGET(page->player_choices));
  gtk_popover_set_child(GTK_POPOVER(players_popover), players_scroll); gtk_menu_button_set_popover(page->player_menu, players_popover);
  g_signal_connect(players_popover, "notify::visible", G_CALLBACK(players_menu_opened), page);
  page->canvas = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_widget_set_size_request(GTK_WIDGET(page->canvas), 240, 180);
  gtk_widget_set_vexpand(GTK_WIDGET(page->canvas), TRUE); gtk_widget_set_hexpand(GTK_WIDGET(page->canvas), TRUE);
  gtk_widget_set_focusable(GTK_WIDGET(page->canvas), TRUE); gtk_widget_add_css_class(GTK_WIDGET(page->canvas), "minimap-canvas");
  gtk_widget_set_overflow(GTK_WIDGET(page->canvas), GTK_OVERFLOW_HIDDEN);
  gtk_accessible_update_property(GTK_ACCESSIBLE(page->canvas), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Saved terrain and online player map"), -1);
  gtk_drawing_area_set_draw_func(page->canvas, map_draw, page, NULL); gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(page->canvas));
  page->footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(page->footer, 10);
  gtk_widget_set_visible(page->footer, FALSE);
  page->status = GTK_LABEL(gtk_label_new(_("Saved terrain / Map Bridge is required for live players")));
  gtk_label_set_wrap(page->status, TRUE); gtk_label_set_xalign(page->status, 0); gtk_widget_add_css_class(GTK_WIDGET(page->status), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(page->status), "dim-label"); gtk_widget_set_hexpand(GTK_WIDGET(page->status), TRUE);
  gtk_box_append(GTK_BOX(page->footer), GTK_WIDGET(page->status));
  gtk_widget_set_visible(GTK_WIDGET(page->status), FALSE);
  page->install = gtk_button_new_with_label(_("Install Map Bridge..."));
  gtk_box_append(GTK_BOX(page->footer), page->install);
  page->approve = gtk_button_new_with_label(_("Allow Data Access..."));
  gtk_box_append(GTK_BOX(page->footer), page->approve);
  gtk_box_append(GTK_BOX(page->map), page->footer);
  page->terrain_status = GTK_LABEL(gtk_label_new(_("Loading saved terrain...")));
  gtk_label_set_xalign(page->terrain_status, 0); gtk_label_set_wrap(page->terrain_status, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(page->terrain_status), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(page->terrain_status), "dim-label");
  gtk_box_append(GTK_BOX(page->map), GTK_WIDGET(page->terrain_status));
  gtk_widget_set_visible(GTK_WIDGET(page->terrain_status), FALSE);
  page->map_view = GTK_STACK(gtk_stack_new());
  gtk_stack_set_vhomogeneous(page->map_view, FALSE);
  gtk_stack_set_hhomogeneous(page->map_view, FALSE);
  gtk_stack_add_named(page->map_view, page->map, "content");
  GtkWidget *disabled = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(disabled), "find-location-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(disabled), _("Minimap is Disabled"));
  adw_status_page_set_description(ADW_STATUS_PAGE(disabled), pumpkin_platform_supports_local_hosting() ?
    _("Explore saved chunks without generating terrain. Enable the minimap here or in Settings. Live players require the optional Map Bridge plugin.") :
    _("The minimap requires a locally hosted server, which is not supported on this platform."));
  GtkWidget *enable = gtk_button_new_with_label(_("Enable Minimap"));
  gtk_widget_set_halign(enable, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(enable, "suggested-action");
  gtk_widget_set_sensitive(enable, pumpkin_platform_supports_local_hosting());
  adw_status_page_set_child(ADW_STATUS_PAGE(disabled), enable);
  g_signal_connect(enable, "clicked", G_CALLBACK(enable_clicked), page);
  gtk_stack_add_named(page->map_view, disabled, "disabled");
  AdwViewStackPage *map_page = adw_view_stack_add_titled(self->details_stack, GTK_WIDGET(page->map_view), "minimap", _("Minimap"));
  adw_view_stack_page_set_icon_name(map_page, "find-location-symbolic");
  g_signal_connect(page->map_view, "map", G_CALLBACK(map_mapped), page);

  page->popover = GTK_POPOVER(gtk_popover_new()); gtk_widget_set_parent(GTK_WIDGET(page->popover), GTK_WIDGET(page->canvas));
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(card, 12); gtk_widget_set_margin_end(card, 12); gtk_widget_set_margin_top(card, 12); gtk_widget_set_margin_bottom(card, 12);
  page->player_name = GTK_LABEL(gtk_label_new(NULL)); gtk_widget_add_css_class(GTK_WIDGET(page->player_name), "title-3");
  page->player_info = GTK_LABEL(gtk_label_new(NULL)); gtk_label_set_selectable(page->player_info, TRUE);
  gtk_box_append(GTK_BOX(card), GTK_WIDGET(page->player_name)); gtk_box_append(GTK_BOX(card), GTK_WIDGET(page->player_info));
  GtkWidget *track = gtk_button_new_with_label(_("Follow Player")); gtk_box_append(GTK_BOX(card), track);
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *kick = gtk_button_new_with_label(_("Kick...")), *ban = gtk_button_new_with_label(_("Ban..."));
  g_object_set_data(G_OBJECT(ban), "map-ban", GINT_TO_POINTER(1)); gtk_widget_add_css_class(ban, "destructive-action");
  gtk_box_append(GTK_BOX(actions), kick); gtk_box_append(GTK_BOX(actions), ban); gtk_box_append(GTK_BOX(card), actions); gtk_popover_set_child(page->popover, card);

  page->enabled = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->enabled), _("Enable minimap"));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(page->enabled), _("Turn off to release map memory and unload the local bridge from all servers."));
  adw_preferences_group_add(self->minimap_settings_group, GTK_WIDGET(page->enabled));
  gtk_widget_set_sensitive(GTK_WIDGET(page->enabled), pumpkin_platform_supports_local_hosting());
  adw_switch_row_set_active(page->enabled, pumpkin_config_get_minimap_enabled(self->config) && pumpkin_config_get_minimap_consent(self->config));
  AdwActionRow *rate_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rate_row), _("Player update rate"));
  adw_action_row_set_subtitle(rate_row, _("Updates per second. Higher rates use more resources."));
  page->refresh_rate = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 2, 20, 1));
  gtk_scale_set_draw_value(page->refresh_rate, TRUE); gtk_scale_set_digits(page->refresh_rate, 0);
  gtk_scale_set_value_pos(page->refresh_rate, GTK_POS_RIGHT); gtk_widget_set_size_request(GTK_WIDGET(page->refresh_rate), 200, -1);
  gtk_range_set_value(GTK_RANGE(page->refresh_rate), pumpkin_config_get_minimap_refresh_hz(self->config));
  gtk_widget_set_sensitive(GTK_WIDGET(page->refresh_rate), adw_switch_row_get_active(page->enabled));
  gtk_widget_set_valign(GTK_WIDGET(page->refresh_rate), GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(page->refresh_rate), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Minimap player updates per second"), -1);
  adw_action_row_add_suffix(rate_row, GTK_WIDGET(page->refresh_rate));
  adw_preferences_group_add(self->minimap_settings_group, GTK_WIDGET(rate_row));
  g_signal_connect(page->refresh_rate, "value-changed", G_CALLBACK(refresh_changed), page);
  gtk_stack_set_visible_child_name(page->map_view, adw_switch_row_get_active(page->enabled) ? "content" : "disabled");
  g_signal_connect(page->enabled, "notify::active", G_CALLBACK(enabled_changed), page);
  g_signal_connect(page->world_choice, "notify::selected", G_CALLBACK(choose_changed), page);
  g_signal_connect(track, "clicked", G_CALLBACK(track_clicked), page); g_signal_connect(page->stop_tracking, "clicked", G_CALLBACK(stop_tracking), page);
  g_signal_connect(kick, "clicked", G_CALLBACK(moderate_clicked), page); g_signal_connect(ban, "clicked", G_CALLBACK(moderate_clicked), page);
  g_signal_connect(page->install, "clicked", G_CALLBACK(install_clicked), page);
  g_signal_connect(page->approve, "clicked", G_CALLBACK(approve_clicked), page);
  GtkGesture *click = gtk_gesture_click_new(); g_signal_connect(click, "released", G_CALLBACK(map_clicked), page);
  gtk_widget_add_controller(GTK_WIDGET(page->canvas), GTK_EVENT_CONTROLLER(click));
  GtkGesture *drag = gtk_gesture_drag_new(); g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), page);
  g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), page); gtk_widget_add_controller(GTK_WIDGET(page->canvas), GTK_EVENT_CONTROLLER(drag));
  GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll, "scroll", G_CALLBACK(zoom_scroll), page); gtk_widget_add_controller(GTK_WIDGET(page->canvas), scroll);
  g_signal_connect(page->map, "map", G_CALLBACK(map_content_mapped), page);
  g_signal_connect(page->map, "unmap", G_CALLBACK(map_content_unmapped), page);
  update_choices(page, NULL); pumpkin_window_minimap_update(self);
}
