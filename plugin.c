/* radicalesync - J-Pilot plugin entry points and GTK user interface */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include "libplugin.h"
#include "rs.h"

#define PLUGIN_MAJOR 1
#define PLUGIN_MINOR 0
#define PLUGIN_NAME "Radicale Sync"

static GtkWidget *log_view;       /* NULL while the plugin page is not shown */
static GtkWidget *sync_button;
static GtkWidget *settings_button;
static GtkWidget *info_label;
static gboolean syncing;

/* ---------- logging ---------- */

static void log_to_jpilot(const char *msg, void *data)
{
   jp_logf(JP_LOG_INFO, "%s: %s\n", PLUGIN_NAME, msg);
}

static void log_to_gui(const char *msg, void *data)
{
   log_to_jpilot(msg, data);
   if (log_view) {
      GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
      GtkTextIter end;
      GtkTextMark *mark;
      gtk_text_buffer_get_end_iter(b, &end);
      gtk_text_buffer_insert(b, &end, msg, -1);
      gtk_text_buffer_insert(b, &end, "\n", -1);
      mark = gtk_text_buffer_create_mark(b, NULL, &end, FALSE);
      gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_view), mark);
      gtk_text_buffer_delete_mark(b, mark);
   }
   /* keep the window responsive during a long sync */
   while (gtk_events_pending())
      gtk_main_iteration();
}

/* ---------- settings dialog ---------- */

typedef struct {
   GtkWidget *dialog;
   GtkWidget *url, *user, *pass;
   GtkWidget *cal, *todo, *card;
   GtkWidget *use_cal, *use_todo, *use_card;
   GtkWidget *conflict, *hotsync, *insecure, *past_days;
   GtkWidget *status;
} SettingsUI;

static GtkWidget *collection_combo(const char *current)
{
   GtkWidget *combo = gtk_combo_box_text_new_with_entry();
   gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), current ? current : "");
   gtk_entry_set_placeholder_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))),
                                  "e.g. /tom/calendar/  (use Find collections)");
   gtk_widget_set_hexpand(combo, TRUE);
   return combo;
}

/* Collection path from a combo: selected id (href) or typed text */
static char *combo_path(GtkWidget *combo)
{
   const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
   if (id)
      return g_strdup(id);
   return g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))))));
}

static void read_form(SettingsUI *ui, RsSettings *s)
{
   rs_settings_clear(s);
   s->url = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(ui->url))));
   s->username = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(ui->user))));
   s->password = g_strdup(gtk_entry_get_text(GTK_ENTRY(ui->pass)));
   s->cal_path = combo_path(ui->cal);
   s->todo_path = combo_path(ui->todo);
   s->card_path = combo_path(ui->card);
   s->sync_cal = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->use_cal));
   s->sync_todo = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->use_todo));
   s->sync_card = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->use_card));
   s->after_hotsync = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->hotsync));
   s->insecure_tls = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->insecure));
   s->conflict = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->conflict)) == 1
                 ? RS_PREFER_PALM : RS_PREFER_SERVER;
   s->past_days = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->past_days));
}

static void fill_combo(GtkWidget *combo, GList *items, const char *want)
{
   GtkEntry *entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo)));
   char *typed = g_strdup(gtk_entry_get_text(entry));
   GList *l;
   int n = 0, chosen = -1;

   gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
   for (l = items; l; l = l->next) {
      RsDavItem *it = l->data;
      gboolean fits;
      char *label;
      if (!strcmp(want, "addressbook"))
         fits = !g_strcmp0(it->kind, "addressbook");
      else
         fits = !g_strcmp0(it->kind, "calendar") &&
                (!it->components || strstr(it->components, want));
      if (!fits)
         continue;
      label = g_strdup_printf("%s  (%s)", it->displayname ? it->displayname : it->href, it->href);
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), it->href, label);
      if (chosen < 0 && (!*typed || !strcmp(typed, it->href)))
         chosen = n;
      n++;
      g_free(label);
   }
   if (chosen >= 0)
      gtk_combo_box_set_active(GTK_COMBO_BOX(combo), chosen);
   g_free(typed);
}

static void cb_find(GtkWidget *w, SettingsUI *ui)
{
   RsSettings s = { 0 };
   GList *items;
   char *err = NULL;

   read_form(ui, &s);
   gtk_label_set_text(GTK_LABEL(ui->status), "Contacting server…");
   while (gtk_events_pending())
      gtk_main_iteration();
   items = rs_discover(&s, &err);
   if (items) {
      char *msg = g_strdup_printf("Connected. Found %u collection(s).", g_list_length(items));
      fill_combo(ui->cal, items, "VEVENT");
      fill_combo(ui->todo, items, "VTODO");
      fill_combo(ui->card, items, "addressbook");
      gtk_label_set_text(GTK_LABEL(ui->status), msg);
      g_free(msg);
   } else {
      gtk_label_set_text(GTK_LABEL(ui->status), err ? err : "Nothing found.");
   }
   g_list_free_full(items, (GDestroyNotify)rs_dav_item_free);
   g_free(err);
   rs_settings_clear(&s);
}

static void attach_row(GtkWidget *grid, int row, const char *label, GtkWidget *w)
{
   GtkWidget *l = gtk_label_new_with_mnemonic(label);
   gtk_label_set_xalign(GTK_LABEL(l), 1.0);
   gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
   gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
   gtk_grid_attach(GTK_GRID(grid), w, 1, row, 2, 1);
}

static void open_settings(GtkWidget *parent)
{
   SettingsUI ui;
   RsSettings s = { 0 };
   GtkWidget *grid, *area, *find, *frame;
   int row = 0;

   rs_settings_load(&s);
   ui.dialog = gtk_dialog_new_with_buttons(
      "Radicale Sync Settings",
      parent ? GTK_WINDOW(gtk_widget_get_toplevel(parent)) : NULL,
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, NULL);
   gtk_dialog_set_default_response(GTK_DIALOG(ui.dialog), GTK_RESPONSE_OK);
   gtk_window_set_default_size(GTK_WINDOW(ui.dialog), 560, -1);

   grid = gtk_grid_new();
   gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
   gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
   gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

   ui.url = gtk_entry_new();
   gtk_entry_set_text(GTK_ENTRY(ui.url), s.url);
   gtk_entry_set_placeholder_text(GTK_ENTRY(ui.url), "http://server:5232/");
   gtk_widget_set_hexpand(ui.url, TRUE);
   attach_row(grid, row++, "Radicale _address:", ui.url);

   ui.user = gtk_entry_new();
   gtk_entry_set_text(GTK_ENTRY(ui.user), s.username);
   attach_row(grid, row++, "_Username:", ui.user);

   ui.pass = gtk_entry_new();
   gtk_entry_set_visibility(GTK_ENTRY(ui.pass), FALSE);
   gtk_entry_set_input_purpose(GTK_ENTRY(ui.pass), GTK_INPUT_PURPOSE_PASSWORD);
   gtk_entry_set_text(GTK_ENTRY(ui.pass), s.password);
   gtk_entry_set_activates_default(GTK_ENTRY(ui.pass), TRUE);
   attach_row(grid, row++, "_Password:", ui.pass);

   ui.insecure = gtk_check_button_new_with_mnemonic("Accept self-signed _TLS certificates");
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.insecure), s.insecure_tls);
   gtk_grid_attach(GTK_GRID(grid), ui.insecure, 1, row++, 2, 1);

   find = gtk_button_new_with_mnemonic("_Find collections");
   ui.status = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(ui.status), 0.0);
   gtk_label_set_line_wrap(GTK_LABEL(ui.status), TRUE);
   gtk_grid_attach(GTK_GRID(grid), find, 1, row, 1, 1);
   gtk_grid_attach(GTK_GRID(grid), ui.status, 2, row++, 1, 1);
   g_signal_connect(find, "clicked", G_CALLBACK(cb_find), &ui);

   frame = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
   gtk_grid_attach(GTK_GRID(grid), frame, 0, row++, 3, 1);

   ui.use_cal = gtk_check_button_new_with_mnemonic("_Calendar:");
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.use_cal), s.sync_cal);
   ui.cal = collection_combo(s.cal_path);
   gtk_grid_attach(GTK_GRID(grid), ui.use_cal, 0, row, 1, 1);
   gtk_grid_attach(GTK_GRID(grid), ui.cal, 1, row++, 2, 1);

   ui.use_todo = gtk_check_button_new_with_mnemonic("_To-dos:");
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.use_todo), s.sync_todo);
   ui.todo = collection_combo(s.todo_path);
   gtk_grid_attach(GTK_GRID(grid), ui.use_todo, 0, row, 1, 1);
   gtk_grid_attach(GTK_GRID(grid), ui.todo, 1, row++, 2, 1);

   ui.use_card = gtk_check_button_new_with_mnemonic("C_ontacts:");
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.use_card), s.sync_card);
   ui.card = collection_combo(s.card_path);
   gtk_grid_attach(GTK_GRID(grid), ui.use_card, 0, row, 1, 1);
   gtk_grid_attach(GTK_GRID(grid), ui.card, 1, row++, 2, 1);

   {
      GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *l = gtk_label_new_with_mnemonic("Sync _past events from the last");
      ui.past_days = gtk_spin_button_new_with_range(0, 36500, 1);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.past_days), s.past_days);
      gtk_label_set_mnemonic_widget(GTK_LABEL(l), ui.past_days);
      gtk_box_pack_start(GTK_BOX(hb), l, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(hb), ui.past_days, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(hb), gtk_label_new("days"), FALSE, FALSE, 0);
      gtk_widget_set_tooltip_text(hb,
         "Calendar events that ended longer ago are neither sent nor received, "
         "and are never deleted because of this limit. Current and future "
         "events, and repeating events without an end, are always synced.");
      gtk_grid_attach(GTK_GRID(grid), hb, 1, row++, 2, 1);
   }

   frame = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
   gtk_grid_attach(GTK_GRID(grid), frame, 0, row++, 3, 1);

   ui.conflict = gtk_combo_box_text_new();
   gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.conflict), "Keep the server version");
   gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.conflict), "Keep the J-Pilot version");
   gtk_combo_box_set_active(GTK_COMBO_BOX(ui.conflict), s.conflict == RS_PREFER_PALM);
   attach_row(grid, row++, "When both _changed:", ui.conflict);

   ui.hotsync = gtk_check_button_new_with_mnemonic(
      "Sync with Radicale automatically during each _HotSync");
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.hotsync), s.after_hotsync);
   gtk_grid_attach(GTK_GRID(grid), ui.hotsync, 1, row++, 2, 1);

   area = gtk_dialog_get_content_area(GTK_DIALOG(ui.dialog));
   gtk_container_add(GTK_CONTAINER(area), grid);
   gtk_widget_show_all(ui.dialog);

   while (gtk_dialog_run(GTK_DIALOG(ui.dialog)) == GTK_RESPONSE_OK) {
      read_form(&ui, &s);
      if (!*s.url || !strstr(s.url, "://")) {
         gtk_label_set_text(GTK_LABEL(ui.status),
                            "Enter the full address, e.g. http://server:5232/");
         continue;
      }
      if (rs_settings_save(&s) == 0)
         break;
      gtk_label_set_text(GTK_LABEL(ui.status), "Could not save the settings.");
   }
   gtk_widget_destroy(ui.dialog);
   rs_settings_clear(&s);
}

/* ---------- main plugin page ---------- */

static char *info_text(const RsSettings *s)
{
   return *s->username
      ? g_strdup_printf("Server: %s   User: %s   Past events: %d days",
                        s->url, s->username, s->past_days)
      : g_strdup("Not configured yet - click Settings.");
}

static void cb_settings(GtkWidget *w, gpointer data)
{
   RsSettings s = { 0 };
   char *t;

   open_settings(w);
   if (!info_label)
      return;
   rs_settings_load(&s);
   t = info_text(&s);
   gtk_label_set_text(GTK_LABEL(info_label), t);
   g_free(t);
   rs_settings_clear(&s);
}

static void cb_sync(GtkWidget *w, gpointer data)
{
   RsSettings s = { 0 };

   if (syncing)
      return;
   syncing = TRUE;
   gtk_widget_set_sensitive(sync_button, FALSE);
   gtk_widget_set_sensitive(settings_button, FALSE);
   rs_set_log_func(log_to_gui, NULL);
   rs_settings_load(&s);
   if (!*s.username && !*s.cal_path && !*s.todo_path && !*s.card_path) {
      rs_log("Nothing is configured yet - click Settings first.");
   } else {
      rs_sync_all(&s);
      rs_log("Switch to the Datebook, Address or To Do view to see the changes.");
   }
   rs_settings_clear(&s);
   rs_set_log_func(log_to_jpilot, NULL);
   if (sync_button) {
      gtk_widget_set_sensitive(sync_button, TRUE);
      gtk_widget_set_sensitive(settings_button, TRUE);
   }
   syncing = FALSE;
}

/* ---------- J-Pilot plugin API ---------- */

void plugin_version(int *major_version, int *minor_version)
{
   *major_version = PLUGIN_MAJOR;
   *minor_version = PLUGIN_MINOR;
}

int plugin_get_name(char *name, int len)
{
   g_snprintf(name, len, "%s %d.%d", PLUGIN_NAME, PLUGIN_MAJOR, PLUGIN_MINOR);
   return EXIT_SUCCESS;
}

int plugin_get_menu_name(char *name, int len)
{
   g_strlcpy(name, PLUGIN_NAME, len);
   return EXIT_SUCCESS;
}

int plugin_get_help_name(char *name, int len)
{
   g_snprintf(name, len, "About %s", PLUGIN_NAME);
   return EXIT_SUCCESS;
}

int plugin_help(char **text, int *width, int *height)
{
   *text = g_strdup(
      PLUGIN_NAME "\n\n"
      "Two-way sync of the J-Pilot calendar, contacts and to-dos with a\n"
      "Radicale CalDAV/CardDAV server.\n\n"
      "Open Plugins > Radicale Sync, click Settings, enter the server\n"
      "address, username and password, then Find collections.\n"
      "Click Sync now, or enable syncing during each HotSync.\n\n"
      "Settings and sync state: ~/.jpilot/radicalesync/\n");
   *width = *height = 0;
   return EXIT_SUCCESS;
}

int plugin_startup(jp_startup_info *info)
{
   jp_init();
   rs_set_log_func(log_to_jpilot, NULL);
   return EXIT_SUCCESS;
}

int plugin_gui(GtkWidget *vbox, GtkWidget *hbox, unsigned int unique_id)
{
   GtkWidget *box, *title, *buttons, *scroll, *hint;
   RsSettings s = { 0 };
   char *where;

   rs_settings_load(&s);
   box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
   gtk_container_set_border_width(GTK_CONTAINER(box), 8);

   title = gtk_label_new(NULL);
   gtk_label_set_markup(GTK_LABEL(title), "<big><b>Radicale Sync</b></big>");
   gtk_label_set_xalign(GTK_LABEL(title), 0.0);
   gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

   where = info_text(&s);
   hint = info_label = gtk_label_new(where);
   gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
   gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
   g_free(where);

   buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   sync_button = gtk_button_new_with_mnemonic("_Sync now");
   settings_button = gtk_button_new_with_mnemonic("S_ettings…");
   g_signal_connect(sync_button, "clicked", G_CALLBACK(cb_sync), NULL);
   g_signal_connect(settings_button, "clicked", G_CALLBACK(cb_settings), NULL);
   gtk_box_pack_start(GTK_BOX(buttons), sync_button, FALSE, FALSE, 0);
   gtk_box_pack_start(GTK_BOX(buttons), settings_button, FALSE, FALSE, 0);
   gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

   log_view = gtk_text_view_new();
   gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
   gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
   gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
   scroll = gtk_scrolled_window_new(NULL, NULL);
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_container_add(GTK_CONTAINER(scroll), log_view);
   gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

   /* J-Pilot passes the left-hand list pane as vbox and the main area as
    * hbox; the whole page belongs in the main area */
   gtk_box_pack_start(GTK_BOX(hbox), box, TRUE, TRUE, 0);
   gtk_widget_show_all(hbox);
   rs_settings_clear(&s);
   return EXIT_SUCCESS;
}

int plugin_gui_cleanup(void)
{
   log_view = NULL;
   info_label = NULL;
   sync_button = NULL;
   settings_button = NULL;
   return EXIT_SUCCESS;
}

/* HotSync hooks.  These run in J-Pilot's sync process, so no GTK here.
 * Before connecting: bring server changes into J-Pilot so they reach the
 * Palm in this HotSync.  After: send changes made on the Palm. */
static void hotsync_run(const char *when)
{
   RsSettings s = { 0 };

   rs_settings_load(&s);
   if (s.after_hotsync) {
      rs_set_log_func(log_to_jpilot, NULL);
      jp_logf(JP_LOG_GUI, "%s: syncing with Radicale (%s HotSync)\n", PLUGIN_NAME, when);
      rs_sync_all(&s);
   }
   rs_settings_clear(&s);
}

int plugin_pre_sync_pre_connect(void)
{
   hotsync_run("before");
   return EXIT_SUCCESS;
}

int plugin_post_sync(void)
{
   hotsync_run("after");
   return EXIT_SUCCESS;
}

int plugin_exit_cleanup(void)
{
   return EXIT_SUCCESS;
}
