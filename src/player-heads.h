#pragma once

#include <gtk/gtk.h>

/* Byte cap for every PNG accepted as a head, skin or cached avatar. */
#define HEAD_MAX_PNG_BYTES (128 * 1024)

void pumpkin_player_head_set(GtkImage *image, const char *uuid, const char *name,
                             gboolean java, gboolean bedrock);
gboolean pumpkin_player_head_is_java(GtkImage *image);
char *pumpkin_player_skin_url(const char *profile_json, const char *identifier);
GdkTexture *pumpkin_player_head_from_skin(GBytes *bytes);
/* PNG-only 64x64 avatars, sharing the skin decoder's pre-allocation limits. */
GdkTexture *pumpkin_player_head_from_png(GBytes *bytes);
/* Reads a cached avatar. known_size is the caller's stat result (a size hint,
 * never trusted as a bound); errors are reported through error when non-NULL. */
GdkTexture *pumpkin_player_head_from_file(const char *path, goffset known_size, GError **error);
