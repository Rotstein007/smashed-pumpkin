#include <gtk/gtk.h>

static void
spin(int milliseconds)
{
  gint64 end = g_get_monotonic_time() + milliseconds * 1000;
  do {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  } while (g_get_monotonic_time() < end);
}

/* A toolkit-only preflight: a window and popover share a frame clock. GTK
 * 4.24.1 can submit that frame twice and abort under fatal-warnings, even
 * without application code, screenshots or a desktop compositor. */
int
main(void)
{
  gtk_init();
  g_print("GTK runtime %u.%u.%u; backend %s\n", gtk_get_major_version(),
    gtk_get_minor_version(), gtk_get_micro_version(),
    G_OBJECT_TYPE_NAME(gdk_display_get_default()));
  GtkWidget *window = gtk_window_new();
  GtkWidget *button = gtk_menu_button_new();
  GtkWidget *popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(popover), gtk_label_new("Frame clock preflight"));
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
  gtk_window_set_child(GTK_WINDOW(window), button);
  gtk_window_set_default_size(GTK_WINDOW(window), 300, 200);
  gtk_window_present(GTK_WINDOW(window));
  spin(200);
  gtk_menu_button_popup(GTK_MENU_BUTTON(button));
  spin(300);
  g_assert_true(gtk_widget_get_mapped(popover));
  gtk_widget_queue_draw(window);
  gtk_widget_queue_draw(popover);
  spin(300);
  gtk_window_destroy(GTK_WINDOW(window));
  spin(200);
  g_print("PASS: GTK window/popover frame clock preflight\n");
  return 0;
}
