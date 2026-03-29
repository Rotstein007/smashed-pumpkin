#pragma once

#include "window-internal.h"

gboolean console_level_matches_log_filter(ConsoleLevel level, int level_index);
gboolean is_auto_poll_noise_line(const char *line);
void append_console_line(PumpkinWindow *self, PumpkinServer *server, const char *line);
void append_log(PumpkinWindow *self, const char *line);
void append_log_for_server(PumpkinWindow *self, PumpkinServer *server, const char *line);
void queue_console_scroll_to_end(PumpkinWindow *self);
void set_console_warning(PumpkinWindow *self, const char *message, gboolean visible);
void ensure_console_buffer_tags(PumpkinWindow *self, GtkTextBuffer *buffer);
void apply_console_filters(PumpkinWindow *self);
char *format_console_line(PumpkinWindow *self, const char *line, ConsoleLevel *out_level);
void on_console_copy(GtkButton *button, PumpkinWindow *self);
void on_console_clear(GtkButton *button, PumpkinWindow *self);
void on_console_filter_toggled(GtkCheckButton *button, PumpkinWindow *self);
void on_console_filter_all_clicked(GtkButton *button, PumpkinWindow *self);
gboolean on_command_entry_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval,
                                      guint keycode,
                                      GdkModifierType state,
                                      PumpkinWindow *self);
void on_send_command(GtkWidget *widget, PumpkinWindow *self);
