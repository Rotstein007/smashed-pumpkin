#include "window-console.h"

#include "window-protocol.h"

static char *sanitize_console_text(const char *line);

static const char *
console_level_token_tag_name(ConsoleLevel level)
{
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      return "console-token-trace";
    case CONSOLE_LEVEL_DEBUG:
      return "console-token-debug";
    case CONSOLE_LEVEL_INFO:
      return "console-token-info";
    case CONSOLE_LEVEL_WARN:
      return "console-token-warn";
    case CONSOLE_LEVEL_ERROR:
      return "console-token-error";
    case CONSOLE_LEVEL_SMPK:
      return "console-token-smpk";
    case CONSOLE_LEVEL_OTHER:
    default:
      return NULL;
  }
}

static void
apply_console_tag_offsets(GtkTextBuffer *buffer, int line_start_offset, int start, int end, const char *tag_name)
{
  if (buffer == NULL || tag_name == NULL || *tag_name == '\0' || start < 0 || end <= start) {
    return;
  }
  GtkTextIter iter_start;
  GtkTextIter iter_end;
  gtk_text_buffer_get_iter_at_offset(buffer, &iter_start, line_start_offset + start);
  gtk_text_buffer_get_iter_at_offset(buffer, &iter_end, line_start_offset + end);
  gtk_text_buffer_apply_tag_by_name(buffer, tag_name, &iter_start, &iter_end);
}

static void
apply_console_inline_tags(GtkTextBuffer *buffer, int line_start_offset, const char *display, ConsoleLevel level)
{
  if (buffer == NULL || display == NULL || *display == '\0') {
    return;
  }

  const char *level_word = NULL;
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      level_word = "TRACE";
      break;
    case CONSOLE_LEVEL_DEBUG:
      level_word = "DEBUG";
      break;
    case CONSOLE_LEVEL_INFO:
      level_word = "INFO";
      break;
    case CONSOLE_LEVEL_WARN:
      level_word = "WARN";
      break;
    case CONSOLE_LEVEL_ERROR:
      level_word = "ERROR";
      break;
    case CONSOLE_LEVEL_SMPK:
      level_word = "SMPK";
      break;
    case CONSOLE_LEVEL_OTHER:
    default:
      break;
  }

  const char *level_tag_name = console_level_token_tag_name(level);
  if (level_word != NULL && level_tag_name != NULL) {
    g_autofree char *level_token = g_strdup_printf("[%s]", level_word);
    const char *token_pos = level_token != NULL ? strstr(display, level_token) : NULL;
    if (token_pos != NULL) {
      int token_start = (int)(token_pos - display) + 1;
      int token_end = token_start + (int)strlen(level_word);
      apply_console_tag_offsets(buffer, line_start_offset, token_start, token_end, level_tag_name);
    }
  }

  static GRegex *startup_ms_re = NULL;
  if (startup_ms_re == NULL) {
    startup_ms_re = g_regex_new("\\b[0-9]+ms\\b", G_REGEX_OPTIMIZE, 0, NULL);
  }
  if (startup_ms_re == NULL) {
    return;
  }

  g_autoptr(GMatchInfo) match = NULL;
  g_regex_match(startup_ms_re, display, 0, &match);
  while (match != NULL && g_match_info_matches(match)) {
    int start = -1;
    int end = -1;
    if (g_match_info_fetch_pos(match, 0, &start, &end) && start >= 0 && end > start) {
      apply_console_tag_offsets(buffer, line_start_offset, start, end, "console-token-startup-ms");
    }
    if (!g_match_info_next(match, NULL)) {
      break;
    }
  }
}

static const char *
console_timestamp_pattern_for_config(PumpkinWindow *self)
{
  PumpkinDateFormat format = PUMPKIN_DATE_FORMAT_DMY;
  PumpkinTimeFormat time_format = PUMPKIN_TIME_FORMAT_24H;
  if (self != NULL && self->config != NULL) {
    format = pumpkin_config_get_date_format(self->config);
    time_format = pumpkin_config_get_time_format(self->config);
  }
  if (format == PUMPKIN_DATE_FORMAT_YMD) {
    return time_format == PUMPKIN_TIME_FORMAT_12H
             ? "%Y-%m-%d %I:%M:%S %p"
             : "%Y-%m-%d %H:%M:%S";
  }
  if (format == PUMPKIN_DATE_FORMAT_MDY) {
    return time_format == PUMPKIN_TIME_FORMAT_12H
             ? "%m/%d/%Y %I:%M:%S %p"
             : "%m/%d/%Y %H:%M:%S";
  }
  return time_format == PUMPKIN_TIME_FORMAT_12H
           ? "%d.%m.%Y %I:%M:%S %p"
           : "%d.%m.%Y %H:%M:%S";
}

static ConsoleLevel
console_level_from_text(const char *level_text)
{
  if (level_text == NULL || *level_text == '\0') {
    return CONSOLE_LEVEL_OTHER;
  }
  g_autofree char *lower = g_ascii_strdown(level_text, -1);
  if (g_str_has_prefix(lower, "trace")) {
    return CONSOLE_LEVEL_TRACE;
  }
  if (g_str_has_prefix(lower, "debug")) {
    return CONSOLE_LEVEL_DEBUG;
  }
  if (g_str_has_prefix(lower, "info")) {
    return CONSOLE_LEVEL_INFO;
  }
  if (g_str_has_prefix(lower, "warn")) {
    return CONSOLE_LEVEL_WARN;
  }
  if (g_str_has_prefix(lower, "error")) {
    return CONSOLE_LEVEL_ERROR;
  }
  if (g_str_has_prefix(lower, "smpk")) {
    return CONSOLE_LEVEL_SMPK;
  }
  return CONSOLE_LEVEL_OTHER;
}

static const char *
console_level_tag_name(ConsoleLevel level)
{
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      return "console-level-trace";
    case CONSOLE_LEVEL_DEBUG:
      return "console-level-debug";
    case CONSOLE_LEVEL_INFO:
      return "console-level-info";
    case CONSOLE_LEVEL_WARN:
      return "console-level-warn";
    case CONSOLE_LEVEL_ERROR:
      return "console-level-error";
    case CONSOLE_LEVEL_SMPK:
      return "console-level-smpk";
    case CONSOLE_LEVEL_OTHER:
    default:
      return "console-level-other";
  }
}

static gboolean
console_level_enabled(PumpkinWindow *self, ConsoleLevel level)
{
  if (self == NULL) {
    return TRUE;
  }
  switch (level) {
    case CONSOLE_LEVEL_TRACE:
      return self->check_console_trace == NULL ||
             gtk_check_button_get_active(self->check_console_trace);
    case CONSOLE_LEVEL_DEBUG:
      return self->check_console_debug == NULL ||
             gtk_check_button_get_active(self->check_console_debug);
    case CONSOLE_LEVEL_INFO:
      return self->check_console_info == NULL ||
             gtk_check_button_get_active(self->check_console_info);
    case CONSOLE_LEVEL_WARN:
      return self->check_console_warn == NULL ||
             gtk_check_button_get_active(self->check_console_warn);
    case CONSOLE_LEVEL_ERROR:
      return self->check_console_error == NULL ||
             gtk_check_button_get_active(self->check_console_error);
    case CONSOLE_LEVEL_SMPK:
      return self->check_console_smpk == NULL ||
             gtk_check_button_get_active(self->check_console_smpk);
    case CONSOLE_LEVEL_OTHER:
    default:
      return self->check_console_other == NULL ||
             gtk_check_button_get_active(self->check_console_other);
  }
}

static void
apply_console_filters_to_buffer(PumpkinWindow *self, GtkTextBuffer *buffer)
{
  if (self == NULL || buffer == NULL) {
    return;
  }
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  if (table == NULL) {
    return;
  }
  ConsoleLevel levels[] = {
    CONSOLE_LEVEL_TRACE,
    CONSOLE_LEVEL_DEBUG,
    CONSOLE_LEVEL_INFO,
    CONSOLE_LEVEL_WARN,
    CONSOLE_LEVEL_ERROR,
    CONSOLE_LEVEL_SMPK,
    CONSOLE_LEVEL_OTHER
  };
  for (guint i = 0; i < G_N_ELEMENTS(levels); i++) {
    const char *tag_name = console_level_tag_name(levels[i]);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, tag_name);
    if (tag != NULL) {
      gboolean visible = console_level_enabled(self, levels[i]);
      g_object_set(tag, "invisible", visible ? FALSE : TRUE, NULL);
    }
  }
}

void
ensure_console_buffer_tags(PumpkinWindow *self, GtkTextBuffer *buffer)
{
  if (buffer == NULL) {
    return;
  }
  ConsoleLevel levels[] = {
    CONSOLE_LEVEL_TRACE,
    CONSOLE_LEVEL_DEBUG,
    CONSOLE_LEVEL_INFO,
    CONSOLE_LEVEL_WARN,
    CONSOLE_LEVEL_ERROR,
    CONSOLE_LEVEL_SMPK,
    CONSOLE_LEVEL_OTHER
  };
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  if (table == NULL) {
    return;
  }
  for (guint i = 0; i < G_N_ELEMENTS(levels); i++) {
    const char *tag_name = console_level_tag_name(levels[i]);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, tag_name);
    if (tag == NULL) {
      tag = gtk_text_buffer_create_tag(buffer, tag_name, NULL);
    }
    if (tag == NULL) {
      continue;
    }
    g_object_set(tag, "foreground-set", FALSE, "weight-set", FALSE, NULL);
  }

  struct {
    const char *name;
    const char *color;
    int weight;
  } token_tags[] = {
    {"console-token-trace", "#7a828a", PANGO_WEIGHT_BOLD},
    {"console-token-debug", "#6f767e", PANGO_WEIGHT_BOLD},
    {"console-token-info", "#2f8f46", PANGO_WEIGHT_BOLD},
    {"console-token-warn", "#b7791f", PANGO_WEIGHT_BOLD},
    {"console-token-error", "#c93434", PANGO_WEIGHT_BOLD},
    {"console-token-smpk", "#4b6cb7", PANGO_WEIGHT_BOLD},
    {"console-token-startup-ms", NULL, PANGO_WEIGHT_BOLD}
  };
  for (guint i = 0; i < G_N_ELEMENTS(token_tags); i++) {
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, token_tags[i].name);
    if (tag == NULL) {
      tag = gtk_text_buffer_create_tag(buffer, token_tags[i].name, NULL);
    }
    if (tag == NULL) {
      continue;
    }
    if (token_tags[i].color != NULL) {
      g_object_set(tag, "foreground", token_tags[i].color, "weight", token_tags[i].weight, NULL);
    } else {
      g_object_set(tag, "weight", token_tags[i].weight, NULL);
    }
  }
  apply_console_filters_to_buffer(self, buffer);
}

void
apply_console_filters(PumpkinWindow *self)
{
  if (self == NULL || self->console_buffers == NULL) {
    return;
  }
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->console_buffers);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(value);
    ensure_console_buffer_tags(self, buffer);
    apply_console_filters_to_buffer(self, buffer);
  }
}

char *
format_console_line(PumpkinWindow *self, const char *line, ConsoleLevel *out_level)
{
  if (out_level != NULL) {
    *out_level = CONSOLE_LEVEL_OTHER;
  }
  g_autofree char *clean = sanitize_console_text(line);
  if (clean == NULL || *clean == '\0') {
    return NULL;
  }

  static GRegex *prefix_re = NULL;
  if (prefix_re == NULL) {
    prefix_re = g_regex_new(
      "^([0-9]{4})-([0-9]{2})-([0-9]{2})[ T]([0-9]{2}):([0-9]{2}):([0-9]{2})\\s+([A-Za-z]+)\\s+(.+)$",
      G_REGEX_OPTIMIZE, 0, NULL);
  }

  g_autofree char *timestamp_text = NULL;
  g_autofree char *message = NULL;
  g_autofree char *level = NULL;
  g_autofree char *target = NULL;

  if (g_str_has_prefix(clean, "[SMPK]")) {
    const char *rest = clean + strlen("[SMPK]");
    while (*rest == ' ') {
      rest++;
    }
    level = g_strdup("SMPK");
    message = g_strdup(rest);
    if (out_level != NULL) {
      *out_level = CONSOLE_LEVEL_SMPK;
    }
  } else if (g_str_has_prefix(clean, "SMPK:")) {
    const char *rest = clean + strlen("SMPK:");
    while (*rest == ' ') {
      rest++;
    }
    level = g_strdup("SMPK");
    message = g_strdup(rest);
    if (out_level != NULL) {
      *out_level = CONSOLE_LEVEL_SMPK;
    }
  }

  if (prefix_re != NULL && level == NULL) {
    g_autoptr(GMatchInfo) match = NULL;
    if (g_regex_match(prefix_re, clean, 0, &match) && g_match_info_matches(match)) {
      g_autofree char *year_txt = g_match_info_fetch(match, 1);
      g_autofree char *month_txt = g_match_info_fetch(match, 2);
      g_autofree char *day_txt = g_match_info_fetch(match, 3);
      g_autofree char *hour_txt = g_match_info_fetch(match, 4);
      g_autofree char *minute_txt = g_match_info_fetch(match, 5);
      g_autofree char *second_txt = g_match_info_fetch(match, 6);
      level = g_match_info_fetch(match, 7);
      g_autofree char *rest = g_match_info_fetch(match, 8);

      int year = year_txt != NULL ? (int)strtol(year_txt, NULL, 10) : 0;
      int month = month_txt != NULL ? (int)strtol(month_txt, NULL, 10) : 0;
      int day = day_txt != NULL ? (int)strtol(day_txt, NULL, 10) : 0;
      int hour = hour_txt != NULL ? (int)strtol(hour_txt, NULL, 10) : 0;
      int minute = minute_txt != NULL ? (int)strtol(minute_txt, NULL, 10) : 0;
      int second = second_txt != NULL ? (int)strtol(second_txt, NULL, 10) : 0;
      g_autoptr(GDateTime) dt_utc = g_date_time_new_utc(year, month, day, hour, minute, (gdouble)second);
      if (dt_utc != NULL) {
        g_autoptr(GDateTime) dt_local = g_date_time_to_local(dt_utc);
        if (dt_local != NULL) {
          timestamp_text = g_date_time_format(dt_local, console_timestamp_pattern_for_config(self));
        }
      }
      if (out_level != NULL) {
        *out_level = console_level_from_text(level);
      }

      if (rest != NULL) {
        const char *split = strstr(rest, ": ");
        if (split != NULL) {
          g_autofree char *prefix = g_strndup(rest, (gsize)(split - rest));
          message = g_strdup(split + 2);
          g_strstrip(prefix);
          if (prefix[0] != '\0') {
            const char *last_space = strrchr(prefix, ' ');
            target = (last_space != NULL && last_space[1] != '\0')
                       ? g_strdup(last_space + 1)
                       : g_strdup(prefix);
          }
        } else {
          message = g_strdup(rest);
        }
      }
    }
  }

  if (timestamp_text == NULL) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    if (now != NULL) {
      timestamp_text = g_date_time_format(now, console_timestamp_pattern_for_config(self));
    }
  }
  if (timestamp_text == NULL) {
    timestamp_text = g_strdup("--");
  }

  if (message == NULL || *message == '\0') {
    message = g_strdup(clean);
  }
  g_strstrip(message);

  if (level != NULL && *level != '\0') {
    g_autofree char *level_upper = g_ascii_strup(level, -1);
    if (target != NULL && *target != '\0') {
      return g_strdup_printf("[%s] [%s] [%s] %s", timestamp_text, level_upper, target, message);
    }
    return g_strdup_printf("[%s] [%s] %s", timestamp_text, level_upper, message);
  }

  return g_strdup_printf("[%s] %s", timestamp_text, message);
}

void
append_console_line(PumpkinWindow *self, PumpkinServer *server, const char *line)
{
  if (self->log_view == NULL || server == NULL || line == NULL) {
    return;
  }
  ConsoleLevel level = CONSOLE_LEVEL_OTHER;
  g_autofree char *display = format_console_line(self, line, &level);
  if (display == NULL || *display == '\0') {
    return;
  }

  GtkTextBuffer *buffer = g_hash_table_lookup(self->console_buffers, server);
  if (buffer == NULL) {
    buffer = gtk_text_buffer_new(NULL);
    g_hash_table_insert(self->console_buffers, g_object_ref(server), buffer);
  }
  ensure_console_buffer_tags(self, buffer);

  if (self->current == server && gtk_text_view_get_buffer(self->log_view) != buffer) {
    gtk_text_view_set_buffer(self->log_view, buffer);
  }

  g_autofree char *with_newline = g_strdup_printf("%s\n", display);
  int line_start_offset = gtk_text_buffer_get_char_count(buffer);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  const char *tag_name = console_level_tag_name(level);
  if (tag_name != NULL && *tag_name != '\0') {
    gtk_text_buffer_insert_with_tags_by_name(buffer, &end, with_newline, -1, tag_name, NULL);
  } else {
    gtk_text_buffer_insert(buffer, &end, with_newline, -1);
  }
  apply_console_inline_tags(buffer, line_start_offset, display, level);

  int line_count = gtk_text_buffer_get_line_count(buffer);
  if (line_count > CONSOLE_MAX_LINES) {
    int cut_line = line_count - CONSOLE_MAX_LINES;
    GtkTextIter trim_start;
    GtkTextIter trim_end;
    gtk_text_buffer_get_start_iter(buffer, &trim_start);
    gtk_text_buffer_get_iter_at_line(buffer, &trim_end, cut_line);
    gtk_text_buffer_delete(buffer, &trim_start, &trim_end);
  }

  gtk_text_buffer_get_end_iter(buffer, &end);

  GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "log-end");
  if (mark == NULL) {
    mark = gtk_text_buffer_create_mark(buffer, "log-end", &end, FALSE);
  } else {
    gtk_text_buffer_move_mark(buffer, mark, &end);
  }
  if (gtk_text_view_get_buffer(self->log_view) == buffer) {
    queue_console_scroll_to_end(self);
  }
}

static gboolean
scroll_console_to_end_idle(gpointer user_data)
{
  PumpkinWindow *self = PUMPKIN_WINDOW(user_data);
  self->console_scroll_idle_id = 0;
  if (self == NULL || self->log_view == NULL) {
    return G_SOURCE_REMOVE;
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_view);
  if (buffer == NULL) {
    return G_SOURCE_REMOVE;
  }
  GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "log-end");
  if (mark != NULL) {
    gtk_text_view_scroll_to_mark(self->log_view, mark, 0.0, TRUE, 0.0, 1.0);
  }
  return G_SOURCE_REMOVE;
}

void
queue_console_scroll_to_end(PumpkinWindow *self)
{
  if (self == NULL || self->log_view == NULL || self->console_scroll_idle_id != 0) {
    return;
  }
  self->console_scroll_idle_id =
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scroll_console_to_end_idle, g_object_ref(self), g_object_unref);
}

void
append_log(PumpkinWindow *self, const char *line)
{
  if (self == NULL || line == NULL || *line == '\0') {
    return;
  }
  if (self->current == NULL) {
    return;
  }
  g_autofree char *prefixed = g_strdup_printf("[SMPK] %s", line);
  append_console_line(self, self->current, prefixed);
}

void
append_log_for_server(PumpkinWindow *self, PumpkinServer *server, const char *line)
{
  if (self == NULL || line == NULL || *line == '\0') {
    return;
  }
  PumpkinServer *target = server != NULL ? server : self->current;
  if (target == NULL) {
    return;
  }
  g_autofree char *prefixed = g_strdup_printf("[SMPK] %s", line);
  append_console_line(self, target, prefixed);
}

void
set_console_warning(PumpkinWindow *self, const char *message, gboolean visible)
{
  if (self == NULL || self->console_warning_revealer == NULL ||
      self->console_warning_label == NULL) {
    return;
  }
  gtk_label_set_text(self->console_warning_label, message != NULL ? message : "");
  gtk_revealer_set_reveal_child(self->console_warning_revealer, visible);
  if (visible) {
    gint64 now = g_get_monotonic_time();
    if (self->suppress_warning_beep_until > now) {
      return;
    }
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
      gdk_display_beep(display);
    }
  }
}

void
on_console_copy(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->log_view == NULL) {
    return;
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_view);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (text == NULL) {
    return;
  }
  GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(self)));
  gdk_clipboard_set_text(clipboard, text);
}

void
on_console_clear(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self->log_view == NULL) {
    return;
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->log_view);
  gtk_text_buffer_set_text(buffer, "", -1);
}

void
on_console_filter_toggled(GtkCheckButton *button, PumpkinWindow *self)
{
  (void)button;
  apply_console_filters(self);
}

void
on_console_filter_all_clicked(GtkButton *button, PumpkinWindow *self)
{
  (void)button;
  if (self == NULL) {
    return;
  }

  GtkCheckButton *checks[] = {
    self->check_console_trace,
    self->check_console_debug,
    self->check_console_info,
    self->check_console_warn,
    self->check_console_error,
    self->check_console_smpk,
    self->check_console_other
  };

  gboolean all_enabled = TRUE;
  for (guint i = 0; i < G_N_ELEMENTS(checks); i++) {
    if (checks[i] == NULL) {
      continue;
    }
    if (!gtk_check_button_get_active(checks[i])) {
      all_enabled = FALSE;
      break;
    }
  }

  gboolean next_state = !all_enabled;
  for (guint i = 0; i < G_N_ELEMENTS(checks); i++) {
    if (checks[i] == NULL) {
      continue;
    }
    gtk_check_button_set_active(checks[i], next_state);
  }
  apply_console_filters(self);
}

static char *
sanitize_console_text(const char *line)
{
  if (line == NULL) {
    return NULL;
  }
  g_autofree char *without_ansi = pumpkin_strip_ansi(line);
  const char *src = without_ansi != NULL ? without_ansi : line;
  GString *out = g_string_sized_new(strlen(src) + 8);
  for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
    unsigned char c = *p;
    if (c == '\r' || c == '\n') {
      continue;
    }
    if (c == '\t') {
      g_string_append_c(out, ' ');
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      continue;
    }
    g_string_append_c(out, (char)c);
  }
  char *text = g_string_free(out, FALSE);
  g_strstrip(text);
  return text;
}

void
on_send_command(GtkWidget *widget, PumpkinWindow *self)
{
  (void)widget;
  if (self->current == NULL) {
    return;
  }

  const char *cmd = gtk_editable_get_text(GTK_EDITABLE(self->entry_command));
  g_autofree char *command = g_strdup(cmd != NULL ? cmd : "");
  g_strstrip(command);
  if (command[0] == '\0') {
    return;
  }

  g_autoptr(GError) error = NULL;
  if (!pumpkin_server_send_command(self->current, command, &error)) {
    append_log(self, error->message);
    return;
  }

  if (self->command_history != NULL) {
    g_ptr_array_add(self->command_history, g_strdup(command));
    if (self->current != NULL) {
      g_autofree char *path = g_build_filename(pumpkin_server_get_data_dir(self->current), "command-history.log", NULL);
      if (path != NULL) {
        g_autofree char *clean = g_strdup(command);
        if (clean != NULL) {
          for (char *p = clean; *p != '\0'; p++) {
            if (*p == '\r' || *p == '\n') {
              *p = ' ';
            }
          }
        }
        FILE *fp = fopen(path, "a");
        if (fp != NULL) {
          fputs(clean != NULL ? clean : command, fp);
          fputc('\n', fp);
          fclose(fp);
        }
      }
    }
    self->command_history_index = -1;
    g_clear_pointer(&self->command_history_draft, g_free);
  }
  gtk_editable_set_text(GTK_EDITABLE(self->entry_command), "");
}

gboolean
on_command_entry_key_pressed(GtkEventControllerKey *controller,
                             guint keyval,
                             guint keycode,
                             GdkModifierType state,
                             PumpkinWindow *self)
{
  (void)controller;
  (void)keycode;
  if (self == NULL || self->entry_command == NULL || self->command_history == NULL ||
      self->command_history->len == 0) {
    return FALSE;
  }

  if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) != 0) {
    return FALSE;
  }

  gboolean up = (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up);
  gboolean down = (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down);
  if (!up && !down) {
    return FALSE;
  }

  const char *current_text = gtk_editable_get_text(GTK_EDITABLE(self->entry_command));
  if (up) {
    if (self->command_history_index < 0) {
      self->command_history_index = (int)self->command_history->len - 1;
      g_clear_pointer(&self->command_history_draft, g_free);
      self->command_history_draft = g_strdup(current_text != NULL ? current_text : "");
    } else if (self->command_history_index > 0) {
      self->command_history_index--;
    }
  } else {
    if (self->command_history_index < 0) {
      return FALSE;
    }
    if (self->command_history_index < (int)self->command_history->len - 1) {
      self->command_history_index++;
    } else {
      self->command_history_index = -1;
    }
  }

  const char *replacement = "";
  if (self->command_history_index >= 0 &&
      self->command_history_index < (int)self->command_history->len) {
    replacement = g_ptr_array_index(self->command_history, self->command_history_index);
  } else if (self->command_history_draft != NULL) {
    replacement = self->command_history_draft;
  }
  gtk_editable_set_text(GTK_EDITABLE(self->entry_command), replacement);
  gtk_editable_set_position(GTK_EDITABLE(self->entry_command), -1);
  if (self->command_history_index < 0) {
    g_clear_pointer(&self->command_history_draft, g_free);
  }
  return TRUE;
}

gboolean
console_level_matches_log_filter(ConsoleLevel level, int level_index)
{
  if (level_index <= 0) {
    return TRUE;
  }
  if (level_index == 1) {
    return level == CONSOLE_LEVEL_INFO;
  }
  if (level_index == 2) {
    return level == CONSOLE_LEVEL_WARN;
  }
  if (level_index == 3) {
    return level == CONSOLE_LEVEL_ERROR;
  }
  return TRUE;
}

gboolean
is_auto_poll_noise_line(const char *line)
{
  if (line == NULL) {
    return FALSE;
  }
  g_autofree char *clean = pumpkin_strip_ansi(line);
  const char *check = clean != NULL ? clean : line;
  if (pumpkin_is_player_list_snapshot_line(check)) {
    return TRUE;
  }
  double tps = 0.0;
  if (pumpkin_parse_tps_from_line(check, &tps)) {
    return TRUE;
  }
  return FALSE;
}
