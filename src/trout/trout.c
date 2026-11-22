#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <inttypes.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <spawn.h>
#include <string.h>
#include "file_transfer.h"
#include "thumbnailer.h"

#define trout_window_width 1100
#define trout_window_height 760
#define trout_window_single_tab_count 1
#define trout_zoom_level_count 7
#define trout_zoom_level_last (trout_zoom_level_count - 1)
#define trout_zoom_level_initial 0
#define trout_zoom_level_step 1
#define trout_zoom_save_delay_milliseconds 5000
#define trout_action_button_update_delay_milliseconds 500
#define trout_directory_refresh_interval_milliseconds 1000
#define trout_action_appearance_context 1
#define trout_action_appearance_button 2
#define trout_action_type_directories 1
#define trout_action_type_files 2
#define trout_filename_max_width_chars 48
#define trout_icon_label_spacing 2
#define trout_icon_size_minimum 16
#define trout_compact_max_rows 128
#define trout_typeahead_timeout_milliseconds 5000
#define trout_sort_name_descending 0
#define trout_sort_name_ascending 1
#define trout_sort_modified_descending 2
#define trout_sort_modified_ascending 3
#define trout_sort_size_descending 4
#define trout_sort_size_ascending 5
#define trout_sort_order_count 6
#define trout_application_id "org.trout.file-manager"
#define trout_compact_css \
  "gridview.trout_compact { padding: 3px; border-spacing: 6px 0; }" \
  "gridview.trout_compact > child, gridview.trout_compact > child box { min-height: 0; margin: 0; padding: 0; border-width: 0; border-spacing: 0; }" \
  "gridview.trout_compact label { min-height: 0; margin: 0; padding: 0; }"
#define trout_directory_attributes "standard::name,standard::display-name,standard::type,standard::icon,standard::fast-content-type,standard::is-hidden,standard::size,time::modified,time::modified-usec"

typedef struct trout_window trout_window;
typedef struct trout_tab trout_tab;
typedef struct trout_delete_job trout_delete_job;
typedef struct trout_clipboard_read trout_clipboard_read;
typedef struct trout_rename_dialog trout_rename_dialog;
typedef struct trout_preferences trout_preferences;
typedef struct trout_action trout_action;
typedef struct trout_transfer_request trout_transfer_request;
typedef struct trout_transfer_prompt trout_transfer_prompt;
typedef struct trout_file_progress_update trout_file_progress_update;
typedef struct trout_directory_recovery trout_directory_recovery;
typedef struct trout_properties_result trout_properties_result;
typedef struct trout_properties_frame trout_properties_frame;

struct trout_preferences {
  guint zoom_level;
  guint save_source_id;
  char *path;
  gboolean dirty;
};

struct trout_action {
  char *label;
  guint appearance;
  guint file_types;
  char **extensions;
  char **command;
  gboolean any_extension;
};

struct trout_window {
  GtkApplication *application;
  GtkWidget *window;
  GtkWidget *notebook;
  GtkWidget *location_box;
  GtkWidget *custom_action_box;
  guint custom_action_timeout_id;
  GtkWidget *hidden_button;
  GtkWidget *action_popover;
  GtkWidget *sort_buttons[trout_sort_order_count];
  gboolean updating_sort_menu;
  GtkWidget *transfer_dialog;
  GtkWidget *transfer_label;
  GtkWidget *transfer_progress;
  GtkWidget *transfer_pause_button;
  GtkWidget *transfer_cancel_button;
  GQueue transfer_queue;
  trout_transfer_request *active_transfer;
  GdkClipboard *clipboard;
  trout_thumbnailer *thumbnailer;
};

struct trout_tab {
  trout_window *window;
  GtkWidget *root;
  GFile *directory;
  GFileMonitor *directory_parent_monitor;
  GtkDirectoryList *directory_list;
  GListStore *display_list;
  GtkCustomFilter *filter;
  GtkFilterListModel *filtered_list;
  GtkSortListModel *sorted_list;
  GtkMultiSelection *selection;
  GHashTable *directory_selection;
  GtkWidget *tab_label;
  GtkWidget *location_stack;
  GtkWidget *path_buttons;
  GtkWidget *location_entry;
  GFilenameCompleter *filename_completer;
  GtkWidget *scrolled_window;
  GtkWidget *grid;
  GtkWidget *status_label;
  GtkWidget *context_popover;
  GFile *context_file;
  gboolean context_file_is_directory;
  guint zoom_level;
  guint icon_size;
  guint sort_order;
  guint directory_generation;
  guint directory_refresh_timeout_id;
  guint typeahead_timeout_id;
  gboolean drag_started;
  gboolean directory_dirty;
  gboolean directory_recovery_pending;
  GString *typeahead_prefix;
  gboolean completion_has_suffix;
  gboolean completion_highlight;
  gboolean completion_suppressed;
  guint completion_source_id;
};

struct trout_delete_job {
  GtkWidget *status_label;
  GFile *source;
  GFileEnumerator *enumerator;
  trout_delete_job *parent;
  guint pending_children;
  gboolean delete_enumeration_done;
  gboolean delete_failed;
};

struct trout_transfer_prompt {
  GtkWidget *dialog;
  GFile *source;
  GFile *destination;
  GFile *source_root;
  GFile *destination_root;
  GFileType source_type;
  GFileType destination_type;
  goffset source_size;
  goffset destination_size;
  GMutex mutex;
  GCond condition;
  trout_file_collision_choice choice;
  gboolean active;
  gboolean choice_ready;
};

struct trout_transfer_request {
  trout_window *window;
  GtkWidget *window_widget;
  GtkWidget *status_label;
  GPtrArray *operations;
  trout_transfer_prompt collision;
  GCancellable *cancellable;
  GMutex pause_mutex;
  GCond pause_condition;
  trout_file_operation operation;
  gint references;
  gboolean paused;
  char *current_name;
};

struct trout_file_progress_update {
  trout_transfer_request *request;
  char *name;
  goffset current_bytes;
  goffset total_bytes;
};

struct trout_clipboard_read {
  trout_window *window;
  GtkWidget *window_widget;
  GInputStream *stream;
  GByteArray *bytes;
  GFile *destination;
  GtkWidget *status_label;
};

struct trout_rename_dialog {
  GFile *source;
  GtkWidget *status_label;
  GtkWidget *entry;
};

struct trout_directory_recovery {
  GFile *directory;
  guint generation;
};

struct trout_properties_result {
  uint64_t size;
  gboolean incomplete;
};

struct trout_properties_frame {
  GFile *directory;
  GFileEnumerator *enumerator;
};

void trout_tab_update_location(trout_tab *tab);
void trout_tab_update_status(trout_tab *tab);
void trout_tab_clear_typeahead(trout_tab *tab);
void trout_tab_reset_typeahead_timeout(trout_tab *tab);
void trout_tab_select_typeahead_match(trout_tab *tab);
void trout_tab_set_directory(trout_tab *tab, GFile *directory);
void trout_tab_update_directory_monitor(trout_tab *tab);
void trout_tab_directory_monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event, gpointer user_data);
void trout_tab_recover_directory(trout_tab *tab);
void trout_tab_recover_directory_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
void trout_tab_recover_directory_done(GObject *source_object, GAsyncResult *result, gpointer user_data);
void trout_tab_edit_location(trout_tab *tab);
void trout_tab_complete_location(trout_tab *tab, gboolean highlight);
void trout_tab_schedule_location_completion(trout_tab *tab, gboolean highlight);
gboolean trout_tab_complete_location_idle(gpointer user_data);
void trout_tab_cancel_location_completion(trout_tab *tab);
void trout_window_update_custom_actions(trout_window *window);
void trout_window_apply_custom_actions(trout_window *window);
gboolean trout_window_update_custom_actions_timeout(gpointer user_data);
void trout_window_destroy(gpointer user_data);
gboolean trout_action_matches_selection(trout_action *action, trout_tab *tab);
gboolean trout_action_matches_file(trout_action *action, GFileInfo *file_info);
void trout_action_activate(GtkButton *button, gpointer user_data);
void trout_action_child_exited(GPid process_id, gint wait_status, gpointer user_data);
GtkWidget *trout_action_make_button(trout_action *action, trout_window *window, GtkWidget *popover);
void trout_window_sort_toggled(GtkCheckButton *button, gpointer user_data);
void trout_window_update_sort_menu(trout_window *window, trout_tab *tab);
void trout_window_set_menu_item_label(GtkWidget *item, char *text);
void trout_tab_set_sort_order(trout_tab *tab, guint sort_order);
void trout_window_add_tab(trout_window *window, GFile *directory);
GtkWidget *trout_window_create(GtkApplication *application, GFile *directory);
GtkWidget *trout_tab_create(trout_window *window, GFile *directory);
void trout_application_startup(GtkApplication *application, gpointer user_data);
void trout_application_shutdown(GApplication *application, gpointer user_data);
trout_preferences *trout_preferences_create(void);
void trout_preferences_destroy(gpointer user_data);
void trout_preferences_schedule_save(trout_preferences *preferences);
status_t trout_preferences_save(trout_preferences *preferences);
gboolean trout_preferences_save_timeout(gpointer user_data);
GPtrArray *trout_action_config_load(void);
GPtrArray *trout_action_config_parse(char *contents);
void trout_action_config_destroy(gpointer user_data);
void trout_action_destroy(gpointer user_data);
void trout_tab_on_path_clicked(GtkButton *button, gpointer user_data);
void trout_tab_on_edit_location(GtkButton *button, gpointer user_data);
void trout_tab_on_location_focus_leave(GtkEventControllerFocus *controller, gpointer user_data);
void trout_tab_on_location_changed(GtkEditable *editable, gpointer user_data);
void trout_tab_on_location_position_changed(GObject *object, GParamSpec *property, gpointer user_data);
void trout_tab_on_completion_data(GFilenameCompleter *completer, gpointer user_data);
void trout_tab_on_entry_context_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data);
void trout_tab_on_entry_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data);
void trout_tab_on_entry_released(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data);
void trout_tab_on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data);
GdkContentProvider *trout_tab_on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data);
gboolean trout_tab_on_drop_accept(GtkDropTarget *drop_target, GdkDrop *drop, gpointer user_data);
gboolean trout_tab_on_drop(GtkDropTarget *drop_target, GValue *value, double x, double y, gpointer user_data);
GdkDragAction trout_tab_on_drop_motion(GtkDropTarget *drop_target, double x, double y, gpointer user_data);
GFile *trout_tab_drop_destination(trout_tab *tab, double x, double y);
void trout_tab_context_action(GtkButton *button, gpointer user_data);
void trout_tab_show_properties(trout_tab *tab);
void trout_tab_properties_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
void trout_tab_properties_done(GObject *source_object, GAsyncResult *result, gpointer user_data);
void trout_tab_properties_add_size(trout_properties_result *result, goffset size);
void trout_tab_properties_frame_destroy(gpointer user_data);
void trout_tab_properties_close(GtkButton *button, gpointer user_data);
gboolean trout_tab_properties_close_request(GtkWindow *window, gpointer user_data);
void trout_tab_context_closed(GtkPopover *popover, gpointer user_data);
void trout_tab_copy_selection(trout_tab *tab, gboolean cut);
void trout_tab_paste(trout_tab *tab);
void trout_tab_enqueue_files(trout_tab *tab, GFile *destination, GSList *files, trout_file_operation operation);
void trout_tab_rename_context_file(trout_tab *tab);
void trout_tab_delete_selection(trout_tab *tab);
void trout_tab_show_error(GtkWidget *status_label, GError *error);
void trout_transfer_request_enqueue(trout_window *window, GtkWidget *status_label, GPtrArray *operations, trout_file_operation operation);
void trout_transfer_request_start_next(trout_window *window);
void trout_transfer_request_finish(trout_transfer_request *request);
void trout_transfer_request_destroy(trout_transfer_request *request);
trout_transfer_request *trout_transfer_request_ref(trout_transfer_request *request);
void trout_transfer_request_release_pause(trout_transfer_request *request);
void trout_transfer_request_cancel(trout_transfer_request *request);
gboolean trout_transfer_request_show_collision(gpointer user_data);
void trout_transfer_request_resolve_collision(GtkButton *button, gpointer user_data);
void trout_transfer_request_update_label(trout_transfer_request *request);
gboolean trout_transfer_request_wait(gpointer user_data);
trout_file_collision_choice trout_transfer_request_collision(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, GFileType source_type, GFileType destination_type, goffset source_size, goffset destination_size, gpointer user_data);
void trout_transfer_request_progress(GFile *source, goffset current_bytes, goffset total_bytes, gpointer user_data);
void trout_transfer_request_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
void trout_transfer_request_complete(GObject *source, GAsyncResult *result, gpointer user_data);
void trout_transfer_request_unref(gpointer user_data);
void trout_transfer_pause(GtkButton *button, gpointer user_data);
void trout_transfer_cancel(GtkButton *button, gpointer user_data);
gboolean trout_file_operation_progress_update(gpointer user_data);
void trout_file_operation_progress_update_destroy(gpointer user_data);
void trout_tab_start_delete_operation(GtkWidget *status_label, GFile *source, trout_delete_job *parent);
void trout_tab_delete_job_destroy(trout_delete_job *job);
void trout_tab_delete_operation_complete(GObject *source, GAsyncResult *result, gpointer user_data);
void trout_tab_delete_directory_enumerated(GObject *source, GAsyncResult *result, gpointer user_data);
void trout_tab_delete_directory_next_files(GObject *source, GAsyncResult *result, gpointer user_data);
void trout_tab_delete_job_finish(trout_delete_job *job, gboolean failed);
void trout_tab_clipboard_read_start(GObject *source, GAsyncResult *result, gpointer user_data);
void trout_tab_clipboard_read_chunk(GObject *source, GAsyncResult *result, gpointer user_data);
void trout_tab_clipboard_read_destroy(trout_clipboard_read *read_data);
GtkWidget *trout_tab_make_context_button(char *label, char *action_name, trout_tab *tab, GtkWidget *popover);
void trout_tab_append_context_menu(trout_tab *tab, GtkWidget *popover, GtkWidget *menu_items, gboolean has_context_entry);
void trout_tab_rename_accept(GtkWidget *widget, gpointer user_data);
void trout_tab_rename_cancel(GtkButton *button, gpointer user_data);
void trout_tab_rename_dialog_destroy(gpointer user_data);
void trout_tab_on_background_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data);
void trout_tab_on_background_context_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data);
gboolean trout_window_on_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
gboolean trout_tab_on_scroll(GtkEventControllerScroll *controller, double delta_x, double delta_y, gpointer user_data);
guint trout_zoom_icon_size(guint zoom_level);
gboolean trout_tab_on_typeahead_timeout(gpointer user_data);

guint
trout_zoom_icon_size(guint zoom_level)
{
  return trout_icon_size_minimum << zoom_level;
}

trout_preferences *
trout_preferences_create(void)
{
  trout_preferences *preferences;
  GKeyFile *key_file;
  GError *error;
  gint zoom_level;
  preferences = g_new0(trout_preferences, 1);
  preferences->zoom_level = trout_zoom_level_initial;
  preferences->path = g_build_filename(g_get_user_config_dir(), "trout", "settings.ini", (char *)0);
  key_file = g_key_file_new();
  error = 0;
  if (!g_key_file_load_from_file(key_file, preferences->path, G_KEY_FILE_NONE, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_warning("Could not load Trout preferences: %s", error->message);
    }
    g_error_free(error);
    g_key_file_unref(key_file);
    return preferences;
  }
  error = 0;
  zoom_level = g_key_file_get_integer(key_file, "trout", "zoom-level", &error);
  if (error != 0) {
    g_warning("Could not read Trout zoom preference: %s", error->message);
    g_error_free(error);
  } else if (zoom_level >= trout_zoom_level_initial && zoom_level <= trout_zoom_level_last) {
    preferences->zoom_level = zoom_level;
  } else {
    g_warning("Ignoring invalid Trout zoom preference: %d", zoom_level);
  }
  g_key_file_unref(key_file);
  return preferences;
}

void
trout_preferences_destroy(gpointer user_data)
{
  trout_preferences *preferences;
  preferences = user_data;
  if (preferences->save_source_id != 0) g_source_remove(preferences->save_source_id);
  g_free(preferences->path);
  g_free(preferences);
}

status_t
trout_preferences_save(trout_preferences *preferences)
{
  GKeyFile *key_file;
  GError *error;
  char *directory;
  char *data;
  gsize length;
  if (!preferences->dirty) return trout_status_success;
  key_file = g_key_file_new();
  g_key_file_set_integer(key_file, "trout", "zoom-level", preferences->zoom_level);
  error = 0;
  data = g_key_file_to_data(key_file, &length, &error);
  if (data == 0) {
    g_warning("Could not serialize Trout preferences: %s", error->message);
    g_error_free(error);
    g_key_file_unref(key_file);
    return trout_status_error;
  }
  directory = g_path_get_dirname(preferences->path);
  if (g_mkdir_with_parents(directory, 0700) != 0) {
    g_warning("Could not create Trout preferences directory %s: %s", directory, g_strerror(errno));
    g_free(directory);
    g_free(data);
    g_key_file_unref(key_file);
    return trout_status_error;
  }
  error = 0;
  if (!g_file_set_contents(preferences->path, data, length, &error)) {
    g_warning("Could not save Trout preferences: %s", error->message);
    g_error_free(error);
    g_free(directory);
    g_free(data);
    g_key_file_unref(key_file);
    return trout_status_error;
  }
  preferences->dirty = FALSE;
  g_free(directory);
  g_free(data);
  g_key_file_unref(key_file);
  return trout_status_success;
}

gboolean
trout_preferences_save_timeout(gpointer user_data)
{
  trout_preferences *preferences;
  preferences = user_data;
  preferences->save_source_id = 0;
  if (trout_preferences_save(preferences) != trout_status_success) {
    g_debug("Trout will retry saving its zoom preference on shutdown");
  }
  return G_SOURCE_REMOVE;
}

void
trout_preferences_schedule_save(trout_preferences *preferences)
{
  if (preferences->save_source_id != 0) g_source_remove(preferences->save_source_id);
  preferences->save_source_id = g_timeout_add(trout_zoom_save_delay_milliseconds, trout_preferences_save_timeout, preferences);
}

void
trout_action_destroy(gpointer user_data)
{
  trout_action *action;
  action = user_data;
  g_free(action->label);
  g_strfreev(action->extensions);
  g_strfreev(action->command);
  g_free(action);
}

GPtrArray *
trout_action_config_parse(char *contents)
{
  GPtrArray *actions;
  char **lines;
  char **line;
  char **fields;
  char **extension;
  trout_action *action;
  actions = g_ptr_array_new_with_free_func(trout_action_destroy);
  lines = g_strsplit(contents, "\n", -1);
  line = lines;
  while (*line != 0) {
    if ((*line)[0] != '\0') {
      fields = g_strsplit(*line, " ", -1);
      action = g_new0(trout_action, 1);
      action->label = g_strdup(fields[0]);
      if (g_strcmp0(fields[1], "c") == 0) action->appearance = trout_action_appearance_context;
      else if (g_strcmp0(fields[1], "b") == 0) action->appearance = trout_action_appearance_button;
      else action->appearance = trout_action_appearance_context | trout_action_appearance_button;
      if (g_strcmp0(fields[2], "d") == 0) action->file_types = trout_action_type_directories;
      else if (g_strcmp0(fields[2], "f") == 0) action->file_types = trout_action_type_files;
      else action->file_types = trout_action_type_directories | trout_action_type_files;
      action->extensions = g_strsplit(fields[3], ",", -1);
      extension = action->extensions;
      while (*extension != 0) {
        if (g_strcmp0(*extension, "*") == 0) {
          action->any_extension = TRUE;
          break;
        }
        extension += 1;
      }
      action->command = g_strdupv(fields + 4);
      g_ptr_array_add(actions, action);
      g_strfreev(fields);
    }
    line += 1;
  }
  g_strfreev(lines);
  return actions;
}

GPtrArray *
trout_action_config_load(void)
{
  GPtrArray *actions;
  GError *error;
  char *path;
  char *contents;
  path = g_build_filename(g_get_user_config_dir(), "trout", "actions", (char *)0);
  error = 0;
  if (!g_file_get_contents(path, &contents, 0, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_warning("Could not read Trout custom actions: %s", error->message);
    }
    g_error_free(error);
    g_free(path);
    return g_ptr_array_new_with_free_func(trout_action_destroy);
  }
  actions = trout_action_config_parse(contents);
  g_free(contents);
  g_free(path);
  return actions;
}

void
trout_action_config_destroy(gpointer user_data)
{
  g_ptr_array_unref(user_data);
}

trout_tab *
trout_window_get_current_tab(trout_window *window)
{
  GtkWidget *page;
  page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(window->notebook), gtk_notebook_get_current_page(GTK_NOTEBOOK(window->notebook)));
  if (page == 0) return 0;
  return g_object_get_data(G_OBJECT(page), "trout_tab");
}

static gboolean
trout_action_matches_entry(trout_action *action, GFile *file, GFileType file_type, const char *name)
{
  const char *extension_start;
  char **extension;
  gboolean matches;
  matches = g_file_is_native(file) &&
      ((file_type == G_FILE_TYPE_DIRECTORY && (action->file_types & trout_action_type_directories) != 0) ||
      (file_type != G_FILE_TYPE_DIRECTORY && (action->file_types & trout_action_type_files) != 0));
  if (matches && !action->any_extension) {
    extension_start = name != 0 ? strrchr(name, '.') : 0;
    if (extension_start == 0 || extension_start == name || extension_start[1] == '\0') {
      matches = FALSE;
    } else {
      extension = action->extensions;
      while (*extension != 0 && g_strcmp0(*extension, extension_start + 1) != 0) extension += 1;
      matches = *extension != 0;
    }
  }
  return matches;
}

gboolean
trout_action_matches_file(trout_action *action, GFileInfo *file_info)
{
  return trout_action_matches_entry(action,
      G_FILE(g_file_info_get_attribute_object(file_info, "standard::file")),
      g_file_info_get_file_type(file_info), g_file_info_get_name(file_info));
}

static gboolean
trout_action_matches_directory(trout_action *action, GFile *directory)
{
  char *name;
  gboolean matches;
  name = g_file_get_basename(directory);
  matches = trout_action_matches_entry(action, directory, G_FILE_TYPE_DIRECTORY, name);
  g_free(name);
  return matches;
}

gboolean
trout_action_matches_selection(trout_action *action, trout_tab *tab)
{
  GtkBitset *selected;
  GtkBitsetIter iterator;
  GFileInfo *file_info;
  guint position;
  gboolean matches;
  selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  if (gtk_bitset_get_size(selected) == 0) {
    gtk_bitset_unref(selected);
    return FALSE;
  }
  matches = TRUE;
  gtk_bitset_iter_init_first(&iterator, selected, &position);
  while (matches) {
    file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
    matches = trout_action_matches_file(action, file_info);
    g_object_unref(file_info);
    if (matches && !gtk_bitset_iter_next(&iterator, &position)) break;
  }
  gtk_bitset_unref(selected);
  return matches;
}

GtkWidget *
trout_action_make_button(trout_action *action, trout_window *window, GtkWidget *popover)
{
  GtkWidget *button;
  GtkWidget *button_label;
  button = gtk_button_new_with_label(action->label);
  gtk_widget_set_halign(button, GTK_ALIGN_FILL);
  if (popover != 0) {
    button_label = gtk_button_get_child(GTK_BUTTON(button));
    gtk_widget_set_hexpand(button_label, TRUE);
    gtk_widget_set_halign(button_label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(button_label), 0.0f);
  }
  g_object_set_data(G_OBJECT(button), "trout_custom_action", action);
  if (popover != 0) g_object_set_data(G_OBJECT(button), "trout_custom_popover", popover);
  g_signal_connect(button, "clicked", G_CALLBACK(trout_action_activate), window);
  return button;
}

void
trout_window_update_custom_actions(trout_window *window)
{
  if (window->custom_action_timeout_id != 0) g_source_remove(window->custom_action_timeout_id);
  window->custom_action_timeout_id = g_timeout_add(trout_action_button_update_delay_milliseconds, trout_window_update_custom_actions_timeout, window);
}

gboolean
trout_window_update_custom_actions_timeout(gpointer user_data)
{
  trout_window *window;
  window = user_data;
  window->custom_action_timeout_id = 0;
  trout_window_apply_custom_actions(window);
  return FALSE;
}

void
trout_window_apply_custom_actions(trout_window *window)
{
  trout_tab *tab;
  trout_action *action;
  GtkWidget *button;
  tab = trout_window_get_current_tab(window);
  button = gtk_widget_get_first_child(window->custom_action_box);
  while (button != 0) {
    action = g_object_get_data(G_OBJECT(button), "trout_custom_action");
    gtk_widget_set_visible(button, tab != 0 && trout_action_matches_selection(action, tab));
    button = gtk_widget_get_next_sibling(button);
  }
}

void
trout_window_destroy(gpointer user_data)
{
  trout_window *window;
  window = user_data;
  if (window->custom_action_timeout_id != 0) g_source_remove(window->custom_action_timeout_id);
  g_free(window);
}

void
trout_action_activate(GtkButton *button, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  trout_action *action;
  GtkWidget *popover;
  GtkBitset *selected;
  GtkBitsetIter iterator;
  GFileInfo *file_info;
  GFile *file;
  GFile *context_directory;
  char **arguments;
  char **command_argument;
  char **environment;
  char *path;
  char *working_directory;
  guint command_count;
  guint selected_count;
  guint argument_index;
  guint position;
  pid_t process_id;
  posix_spawn_file_actions_t spawn_actions;
  int spawn_status;
  gboolean path_error;
  gboolean context_directory_action;
  window = user_data;
  tab = g_object_get_data(G_OBJECT(button), "trout_custom_tab");
  if (tab == 0) tab = trout_window_get_current_tab(window);
  action = g_object_get_data(G_OBJECT(button), "trout_custom_action");
  popover = g_object_get_data(G_OBJECT(button), "trout_custom_popover");
  context_directory = g_object_get_data(G_OBJECT(button), "trout_context_directory");
  context_directory_action = context_directory != 0;
  if (tab != 0 && (context_directory_action ? trout_action_matches_directory(action, context_directory) : trout_action_matches_selection(action, tab))) {
    working_directory = g_file_get_path(tab->directory);
    selected = 0;
    selected_count = 1;
    if (!context_directory_action) {
      selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
      selected_count = gtk_bitset_get_size(selected);
    }
    command_count = g_strv_length(action->command);
    arguments = g_new0(char *, command_count + selected_count + 1);
    argument_index = 0;
    command_argument = action->command;
    while (*command_argument != 0) {
      arguments[argument_index] = g_strdup(*command_argument);
      argument_index += 1;
      command_argument += 1;
    }
    path_error = FALSE;
    if (context_directory_action) {
      path = g_file_get_path(context_directory);
      if (path == 0) {
        gtk_label_set_text(GTK_LABEL(tab->status_label), "Custom actions require local files");
        path_error = TRUE;
      } else {
        arguments[argument_index] = path;
        argument_index += 1;
      }
    } else {
      gtk_bitset_iter_init_first(&iterator, selected, &position);
      while (!path_error) {
        file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
        file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
        path = g_file_get_path(file);
        if (path == 0) {
          gtk_label_set_text(GTK_LABEL(tab->status_label), "Custom actions require local files");
          path_error = TRUE;
        } else {
          arguments[argument_index] = path;
          argument_index += 1;
        }
        g_object_unref(file_info);
        if (!path_error && !gtk_bitset_iter_next(&iterator, &position)) break;
      }
    }
    if (!path_error) {
      environment = g_get_environ();
      spawn_status = posix_spawn_file_actions_init(&spawn_actions);
      if (spawn_status == 0) {
        spawn_status = posix_spawn_file_actions_addchdir_np(&spawn_actions, working_directory);
        if (spawn_status == 0) spawn_status = posix_spawnp(&process_id, arguments[0], &spawn_actions, 0, arguments, environment);
        posix_spawn_file_actions_destroy(&spawn_actions);
      }
      g_strfreev(environment);
      if (spawn_status != 0) {
        gtk_label_set_text(GTK_LABEL(tab->status_label), g_strerror(spawn_status));
      } else {
        g_child_watch_add(process_id, trout_action_child_exited, 0);
      }
    }
    g_free(working_directory);
    g_strfreev(arguments);
    if (selected != 0) gtk_bitset_unref(selected);
  }
  if (popover != 0) gtk_popover_popdown(GTK_POPOVER(popover));
}

void
trout_action_child_exited(GPid process_id, gint wait_status, gpointer user_data)
{
  g_spawn_close_pid(process_id);
  (void)wait_status;
  (void)user_data;
}

#include "directory.c"

void
trout_tab_show_error(GtkWidget *status_label, GError *error)
{
  gtk_label_set_text(GTK_LABEL(status_label), error->message);
}

#include "file_transfer_ui.c"

#include "file_operations.c"

GtkWidget *
trout_tab_build_grid(trout_tab *tab)
{
  GtkListItemFactory *factory;
  GtkWidget *grid;
  GtkGesture *click;
  GtkGesture *context_click;
  GtkEventController *scroll;
  GtkDropTarget *drop_target;
  factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(trout_tab_factory_setup), tab);
  g_signal_connect(factory, "bind", G_CALLBACK(trout_tab_factory_bind), tab);
  g_signal_connect(factory, "unbind", G_CALLBACK(trout_tab_factory_unbind), tab);
  grid = gtk_grid_view_new(GTK_SELECTION_MODEL(g_object_ref(tab->selection)), factory);
  gtk_orientable_set_orientation(GTK_ORIENTABLE(grid), GTK_ORIENTATION_HORIZONTAL);
  gtk_scrollable_set_hscroll_policy(GTK_SCROLLABLE(grid), GTK_SCROLL_NATURAL);
  gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(grid), TRUE);
  gtk_grid_view_set_max_columns(GTK_GRID_VIEW(grid), trout_compact_max_rows);
  gtk_widget_add_css_class(grid, "trout_compact");
  g_signal_connect(grid, "activate", G_CALLBACK(trout_tab_on_activate), tab);
  click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(trout_tab_on_background_pressed), tab);
  gtk_widget_add_controller(grid, GTK_EVENT_CONTROLLER(click));
  context_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_click), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(context_click), GTK_PHASE_CAPTURE);
  g_signal_connect(context_click, "pressed", G_CALLBACK(trout_tab_on_background_context_pressed), tab);
  gtk_widget_add_controller(grid, GTK_EVENT_CONTROLLER(context_click));
  drop_target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_MOVE);
  g_signal_connect(drop_target, "accept", G_CALLBACK(trout_tab_on_drop_accept), tab);
  g_signal_connect(drop_target, "drop", G_CALLBACK(trout_tab_on_drop), tab);
  g_signal_connect(drop_target, "motion", G_CALLBACK(trout_tab_on_drop_motion), tab);
  gtk_widget_add_controller(grid, GTK_EVENT_CONTROLLER(drop_target));
  scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
  g_signal_connect(scroll, "scroll", G_CALLBACK(trout_tab_on_scroll), tab);
  gtk_widget_add_controller(grid, scroll);
  return grid;
}

gboolean
trout_tab_on_drop_accept(GtkDropTarget *drop_target, GdkDrop *drop, gpointer user_data)
{
  (void)drop_target;
  (void)user_data;
  return gdk_drop_get_drag(drop) != 0;
}

gboolean
trout_tab_on_drop(GtkDropTarget *drop_target, GValue *value, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GdkDrop *drop;
  GFile *destination;
  GdkFileList *file_list;
  GSList *files;
  tab = user_data;
  drop = gtk_drop_target_get_current_drop(drop_target);
  if (drop == 0 || gdk_drop_get_drag(drop) == 0) return FALSE;
  destination = trout_tab_drop_destination(tab, x, y);
  if (destination == 0) return FALSE;
  file_list = g_value_get_boxed(value);
  files = gdk_file_list_get_files(file_list);
  trout_tab_enqueue_files(tab, destination, files, trout_file_operation_move);
  g_slist_free(files);
  return TRUE;
}

GdkDragAction
trout_tab_on_drop_motion(GtkDropTarget *drop_target, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GdkDrop *drop;
  tab = user_data;
  drop = gtk_drop_target_get_current_drop(drop_target);
  if (drop != 0 && gdk_drop_get_drag(drop) != 0 && trout_tab_drop_destination(tab, x, y) != 0) return GDK_ACTION_MOVE;
  return GDK_ACTION_NONE;
}

GFile *
trout_tab_drop_destination(trout_tab *tab, double x, double y)
{
  GtkWidget *picked;
  GtkListItem *list_item;
  GFileInfo *file_info;
  picked = gtk_widget_pick(tab->grid, x, y, GTK_PICK_DEFAULT);
  while (picked != 0 && picked != tab->grid) {
    list_item = g_object_get_data(G_OBJECT(picked), "trout_list_item");
    if (list_item != 0) {
      file_info = gtk_list_item_get_item(list_item);
      if (g_file_info_get_file_type(file_info) == G_FILE_TYPE_DIRECTORY) {
        return G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
      }
      return 0;
    }
    picked = gtk_widget_get_parent(picked);
  }
  if (picked == tab->grid) return tab->directory;
  return 0;
}

void
trout_tab_on_background_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GdkModifierType state;
  GtkWidget *target;
  tab = user_data;
  state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  target = gtk_widget_pick(tab->grid, x, y, GTK_PICK_DEFAULT);
  if (target != tab->grid) return;
  gtk_widget_grab_focus(tab->grid);
  if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != 0) return;
  gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(tab->selection));
  (void)press_count;
}

void
trout_tab_on_background_context_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GtkWidget *target;
  GtkWidget *popover;
  GtkWidget *menu_items;
  GdkRectangle point;
  tab = user_data;
  target = gtk_widget_pick(tab->grid, x, y, GTK_PICK_DEFAULT);
  if (target != tab->grid) return;
  if (tab->context_file != 0) g_object_unref(tab->context_file);
  tab->context_file = g_object_ref(tab->directory);
  tab->context_file_is_directory = TRUE;
  if (tab->context_popover != 0) {
    popover = tab->context_popover;
    tab->context_popover = 0;
    if (gtk_widget_get_parent(popover) != 0) gtk_widget_unparent(popover);
  }
  popover = gtk_popover_new();
  tab->context_popover = popover;
  menu_items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  trout_tab_append_context_menu(tab, popover, menu_items, FALSE);
  gtk_popover_set_child(GTK_POPOVER(popover), menu_items);
  g_signal_connect(popover, "closed", G_CALLBACK(trout_tab_context_closed), tab);
  point.x = x;
  point.y = y;
  point.width = 1;
  point.height = 1;
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &point);
  gtk_widget_set_parent(popover, tab->grid);
  gtk_popover_popup(GTK_POPOVER(popover));
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_widget_grab_focus(tab->grid);
  (void)press_count;
}

void
trout_tab_on_entry_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GtkWidget *row;
  GtkListItem *list_item;
  guint position;
  GdkModifierType state;
  gboolean selected;
  tab = user_data;
  row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  list_item = g_object_get_data(G_OBJECT(row), "trout_list_item");
  position = gtk_list_item_get_position(list_item);
  state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  tab->drag_started = FALSE;
  gtk_widget_grab_focus(tab->grid);
  selected = gtk_selection_model_is_selected(GTK_SELECTION_MODEL(tab->selection), position);
  if (!selected || (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != 0) {
    gtk_widget_activate_action(tab->grid, "list.select-item", "(ubb)", position,
        (state & GDK_CONTROL_MASK) != 0, (state & GDK_SHIFT_MASK) != 0);
  }
  if (press_count == 2) gtk_widget_activate_action(tab->grid, "list.activate-item", "u", position);
  (void)x;
  (void)y;
}

void
trout_tab_on_entry_released(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GtkWidget *row;
  GtkListItem *list_item;
  GtkBitset *selection;
  guint position;
  GdkModifierType state;
  tab = user_data;
  row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  list_item = g_object_get_data(G_OBJECT(row), "trout_list_item");
  position = gtk_list_item_get_position(list_item);
  state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  if (!tab->drag_started) {
    selection = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == 0 && gtk_bitset_get_size(selection) > 1 &&
        gtk_selection_model_is_selected(GTK_SELECTION_MODEL(tab->selection), position)) {
      gtk_widget_activate_action(tab->grid, "list.select-item", "(ubb)", position, FALSE, FALSE);
    }
    gtk_bitset_unref(selection);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  }
  tab->drag_started = FALSE;
  (void)press_count;
  (void)x;
  (void)y;
}

void
trout_tab_on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  tab->drag_started = TRUE;
  (void)source;
  (void)drag;
}

gboolean
trout_tab_on_scroll(GtkEventControllerScroll *controller, double delta_x, double delta_y, gpointer user_data)
{
  trout_tab *tab;
  trout_preferences *preferences;
  GdkModifierType state;
  GtkAdjustment *adjustment;
  double scroll_delta;
  double scroll_position;
  tab = user_data;
  state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  if ((state & GDK_CONTROL_MASK) != 0) {
    if (delta_y == 0.0) return FALSE;
    if (delta_y < 0.0) {
      if (tab->zoom_level == trout_zoom_level_last) return TRUE;
      tab->zoom_level += trout_zoom_level_step;
    } else {
      if (tab->zoom_level == trout_zoom_level_initial) return TRUE;
      tab->zoom_level -= trout_zoom_level_step;
    }
    tab->icon_size = trout_zoom_icon_size(tab->zoom_level);
    preferences = g_object_get_data(G_OBJECT(tab->window->application), "trout_preferences");
    preferences->zoom_level = tab->zoom_level;
    preferences->dirty = TRUE;
    trout_preferences_schedule_save(preferences);
    scroll_position = gtk_adjustment_get_value(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled_window)));
    tab->grid = trout_tab_build_grid(tab);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tab->scrolled_window), tab->grid);
    gtk_adjustment_set_value(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled_window)), scroll_position);
    return TRUE;
  }
  scroll_delta = delta_x != 0.0 ? delta_x : delta_y;
  if (scroll_delta == 0.0) return FALSE;
  adjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled_window));
  scroll_position = gtk_adjustment_get_value(adjustment);
  scroll_position += scroll_delta * gtk_adjustment_get_step_increment(adjustment);
  gtk_adjustment_set_value(adjustment, scroll_position);
  return TRUE;
}

void
trout_tab_update_error(GtkDirectoryList *directory_list, GParamSpec *property, gpointer user_data)
{
  trout_tab *tab;
  GError *error;
  (void)property;
  tab = user_data;
  error = (GError *)gtk_directory_list_get_error(directory_list);
  if (error != 0 && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))) {
    trout_tab_recover_directory(tab);
  }
  trout_tab_update_status(tab);
}

void
trout_tab_destroy(gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  if (tab->directory_refresh_timeout_id != 0) g_source_remove(tab->directory_refresh_timeout_id);
  if (tab->typeahead_timeout_id != 0) g_source_remove(tab->typeahead_timeout_id);
  if (tab->directory_parent_monitor != 0) {
    g_file_monitor_cancel(tab->directory_parent_monitor);
    g_object_unref(tab->directory_parent_monitor);
  }
  trout_tab_cancel_location_completion(tab);
  g_string_free(tab->typeahead_prefix, TRUE);
  if (tab->context_file != 0) g_object_unref(tab->context_file);
  g_object_unref(tab->filename_completer);
  g_object_unref(tab->directory_list);
  g_object_unref(tab->filter);
  g_object_unref(tab->selection);
  g_object_unref(tab->sorted_list);
  g_object_unref(tab->filtered_list);
  g_object_unref(tab->display_list);
  if (tab->directory_selection != 0) g_hash_table_unref(tab->directory_selection);
  g_object_unref(tab->directory);
  g_free(tab);
}

GtkWidget *
trout_tab_create(trout_window *window, GFile *directory)
{
  trout_tab *tab;
  trout_preferences *preferences;
  GtkWidget *root;
  GtkEventController *entry_keys;
  GtkEventController *entry_focus;
  GtkWidget *location_entry;
  GtkWidget *scrolled;
  GtkWidget *status;
  tab = g_new0(trout_tab, 1);
  tab->window = window;
  tab->directory = g_object_ref(directory);
  tab->directory_generation = 1;
  preferences = g_object_get_data(G_OBJECT(window->application), "trout_preferences");
  tab->zoom_level = preferences->zoom_level;
  tab->sort_order = trout_sort_name_descending;
  tab->icon_size = trout_zoom_icon_size(tab->zoom_level);
  tab->typeahead_prefix = g_string_new(0);
  tab->filename_completer = g_filename_completer_new();
  g_signal_connect(tab->filename_completer, "got-completion-data", G_CALLBACK(trout_tab_on_completion_data), tab);
  tab->directory_list = gtk_directory_list_new(trout_directory_attributes, 0);
  gtk_directory_list_set_io_priority(tab->directory_list, G_PRIORITY_DEFAULT_IDLE);
  g_signal_connect(tab->directory_list, "items-changed", G_CALLBACK(trout_tab_on_directory_items_changed), tab);
  tab->display_list = g_list_store_new(G_TYPE_FILE_INFO);
  tab->filter = gtk_custom_filter_new(trout_filter_hidden, 0, 0);
  tab->filtered_list = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(tab->display_list)), GTK_FILTER(g_object_ref(tab->filter)));
  tab->sorted_list = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(tab->filtered_list)), trout_create_file_sorter(tab->sort_order));
  gtk_sort_list_model_set_incremental(tab->sorted_list, TRUE);
  g_signal_connect(tab->sorted_list, "notify::pending", G_CALLBACK(trout_tab_on_sort_pending_changed), tab);
  tab->selection = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(tab->sorted_list)));
  root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  tab->root = root;
  g_object_set_data_full(G_OBJECT(root), "trout_tab", tab, trout_tab_destroy);
  tab->location_stack = gtk_stack_new();
  tab->path_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(tab->path_buttons, TRUE);
  gtk_stack_add_named(GTK_STACK(tab->location_stack), tab->path_buttons, "buttons");
  location_entry = gtk_entry_new();
  tab->location_entry = location_entry;
  g_signal_connect(location_entry, "changed", G_CALLBACK(trout_tab_on_location_changed), tab);
  g_signal_connect(location_entry, "notify::cursor-position", G_CALLBACK(trout_tab_on_location_position_changed), tab);
  g_signal_connect(location_entry, "notify::selection-bound", G_CALLBACK(trout_tab_on_location_position_changed), tab);
  gtk_widget_set_hexpand(location_entry, TRUE);
  gtk_stack_add_named(GTK_STACK(tab->location_stack), location_entry, "entry");
  entry_keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(entry_keys), GTK_PHASE_CAPTURE);
  g_signal_connect(entry_keys, "key-pressed", G_CALLBACK(trout_tab_on_location_key), tab);
  gtk_widget_add_controller(location_entry, entry_keys);
  entry_focus = gtk_event_controller_focus_new();
  g_signal_connect(entry_focus, "leave", G_CALLBACK(trout_tab_on_location_focus_leave), tab);
  gtk_widget_add_controller(location_entry, entry_focus);
  g_signal_connect(location_entry, "activate", G_CALLBACK(trout_tab_on_location_activate), tab);
  gtk_stack_add_child(GTK_STACK(window->location_box), tab->location_stack);
  scrolled = gtk_scrolled_window_new();
  tab->scrolled_window = scrolled;
  gtk_widget_set_vexpand(scrolled, TRUE);
  tab->grid = trout_tab_build_grid(tab);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), tab->grid);
  gtk_box_append(GTK_BOX(root), scrolled);
  status = gtk_label_new(0);
  tab->status_label = status;
  gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
  gtk_widget_set_margin_start(status, 8);
  gtk_widget_set_margin_end(status, 8);
  gtk_widget_set_margin_top(status, 4);
  gtk_widget_set_margin_bottom(status, 4);
  gtk_box_append(GTK_BOX(root), status);
  g_signal_connect(tab->filtered_list, "notify::n-items", G_CALLBACK(trout_tab_item_count_changed), tab);
  g_signal_connect(tab->selection, "selection-changed", G_CALLBACK(trout_tab_selection_changed), tab);
  g_signal_connect(tab->directory_list, "notify::error", G_CALLBACK(trout_tab_update_error), tab);
  g_signal_connect(tab->directory_list, "notify::loading", G_CALLBACK(trout_tab_on_directory_loading_changed), tab);
  tab->directory_refresh_timeout_id = g_timeout_add(trout_directory_refresh_interval_milliseconds, trout_tab_refresh_directory, tab);
  trout_tab_update_location(tab);
  tab->directory_dirty = TRUE;
  trout_tab_update_directory_monitor(tab);
  gtk_directory_list_set_file(tab->directory_list, tab->directory);
  trout_tab_update_status(tab);
  return root;
}

void
trout_window_page_changed(GtkNotebook *notebook, GtkWidget *page, guint page_number, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  window = user_data;
  tab = g_object_get_data(G_OBJECT(page), "trout_tab");
  gtk_stack_set_visible_child(GTK_STACK(window->location_box), tab->location_stack);
  trout_tab_clear_typeahead(tab);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(window->hidden_button), gtk_filter_list_model_get_filter(tab->filtered_list) == 0);
  trout_window_update_sort_menu(window, tab);
  trout_window_update_custom_actions(window);
  (void)notebook;
  (void)page_number;
}

void
trout_window_page_added(GtkNotebook *notebook, GtkWidget *page, guint page_number, gpointer user_data)
{
  gtk_notebook_set_show_tabs(notebook, gtk_notebook_get_n_pages(notebook) > 1);
  (void)page;
  (void)page_number;
  (void)user_data;
}

void
trout_window_page_removed(GtkNotebook *notebook, GtkWidget *page, guint page_number, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  window = user_data;
  tab = g_object_get_data(G_OBJECT(page), "trout_tab");
  gtk_stack_remove(GTK_STACK(window->location_box), tab->location_stack);
  gtk_notebook_set_show_tabs(notebook, gtk_notebook_get_n_pages(notebook) > 1);
  (void)page_number;
}

void
trout_window_add_tab(trout_window *window, GFile *directory)
{
  GtkWidget *page;
  GtkWidget *label;
  char *name;
  page = trout_tab_create(window, directory);
  name = g_file_get_basename(directory);
  if (name == 0) name = g_file_get_parse_name(directory);
  label = gtk_label_new(name);
  gtk_notebook_append_page(GTK_NOTEBOOK(window->notebook), page, label);
  ((trout_tab *)g_object_get_data(G_OBJECT(page), "trout_tab"))->tab_label = label;
  gtk_notebook_set_current_page(GTK_NOTEBOOK(window->notebook), -1);
  g_free(name);
}

void
trout_window_action_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  window = user_data;
  tab = trout_window_get_current_tab(window);
  if (tab != 0) trout_window_add_tab(window, tab->directory);
  (void)action;
  (void)parameter;
}

void
trout_window_action_new_window(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  GFile *directory;
  window = user_data;
  tab = trout_window_get_current_tab(window);
  directory = tab != 0 ? g_object_ref(tab->directory) : g_file_new_for_path(g_get_home_dir());
  trout_window_create(window->application, directory);
  g_object_unref(directory);
  (void)action;
  (void)parameter;
}

void
trout_window_action_toggle_hidden(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  gboolean show_hidden;
  window = user_data;
  tab = trout_window_get_current_tab(window);
  if (tab != 0) {
    show_hidden = gtk_filter_list_model_get_filter(tab->filtered_list) != 0;
    gtk_filter_list_model_set_filter(tab->filtered_list, show_hidden ? 0 : GTK_FILTER(tab->filter));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(window->hidden_button), show_hidden);
  }
  (void)action;
  (void)parameter;
}

void
trout_window_action_edit_location(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  trout_tab *tab;
  tab = trout_window_get_current_tab(user_data);
  if (tab != 0) trout_tab_edit_location(tab);
  (void)action;
  (void)parameter;
}

void
trout_window_add_action(trout_window *window, char *name, GCallback callback)
{
  GSimpleAction *action;
  action = g_simple_action_new(name, 0);
  g_signal_connect(action, "activate", callback, window);
  g_action_map_add_action(G_ACTION_MAP(window->window), G_ACTION(action));
  g_object_unref(action);
}

void
trout_window_action_button(GtkButton *button, gpointer user_data)
{
  char *action_name;
  GtkWidget *popover;
  action_name = g_object_get_data(G_OBJECT(button), "trout_action");
  gtk_widget_activate_action(GTK_WIDGET(user_data), action_name, 0);
  popover = g_object_get_data(G_OBJECT(button), "trout_popover");
  gtk_popover_popdown(GTK_POPOVER(popover));
}

GtkWidget *
trout_window_make_action_button(char *label, char *action_name, trout_window *window)
{
  GtkWidget *button;
  button = gtk_button_new();
  trout_window_set_menu_item_label(button, label);
  g_object_set_data(G_OBJECT(button), "trout_action", (gpointer)action_name);
  g_object_set_data(G_OBJECT(button), "trout_popover", window->action_popover);
  g_signal_connect(button, "clicked", G_CALLBACK(trout_window_action_button), window->window);
  return button;
}

void
trout_window_set_menu_item_label(GtkWidget *item, char *text)
{
  GtkWidget *label;
  label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_hexpand(label, TRUE);
  if (GTK_IS_CHECK_BUTTON(item)) gtk_check_button_set_child(GTK_CHECK_BUTTON(item), label);
  else gtk_button_set_child(GTK_BUTTON(item), label);
  gtk_widget_set_halign(item, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(item, TRUE);
}

void
trout_window_sort_toggled(GtkCheckButton *button, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  guint sort_order;
  window = user_data;
  if (!window->updating_sort_menu && gtk_check_button_get_active(button)) {
    tab = trout_window_get_current_tab(window);
    sort_order = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "trout_sort_order"));
    if (tab != 0) trout_tab_set_sort_order(tab, sort_order);
  }
}

void
trout_window_update_sort_menu(trout_window *window, trout_tab *tab)
{
  guint sort_index;
  window->updating_sort_menu = TRUE;
  sort_index = 0;
  while (sort_index < trout_sort_order_count) {
    gtk_check_button_set_active(GTK_CHECK_BUTTON(window->sort_buttons[sort_index]), tab->sort_order == sort_index);
    sort_index += 1;
  }
  window->updating_sort_menu = FALSE;
}

GtkWidget *
trout_window_create(GtkApplication *application, GFile *directory)
{
  trout_window *window;
  GtkWidget *header;
  GtkWidget *button;
  GtkWidget *menu_button;
  GtkWidget *popover;
  GtkWidget *menu_actions;
  GtkWidget *header_controls;
  GtkWidget *separator;
  GPtrArray *actions;
  trout_action *action;
  guint action_index;
  guint sort_index;
  GtkEventController *keys;
  char *sort_labels[trout_sort_order_count] = {
    "Sort by name descending",
    "Sort by name ascending",
    "Sort by modification time descending",
    "Sort by modification time ascending",
    "Sort by size descending",
    "Sort by size ascending"
  };
  window = g_new0(trout_window, 1);
  window->application = application;
  window->thumbnailer = g_object_get_data(G_OBJECT(application), "trout_thumbnailer");
  window->window = gtk_application_window_new(application);
  g_queue_init(&window->transfer_queue);
  window->clipboard = gdk_display_get_clipboard(gtk_widget_get_display(window->window));
  window->notebook = gtk_notebook_new();
  g_object_set_data_full(G_OBJECT(window->window), "trout_window", window, trout_window_destroy);
  gtk_window_set_title(GTK_WINDOW(window->window), "");
  gtk_window_set_default_size(GTK_WINDOW(window->window), trout_window_width, trout_window_height);
  header = gtk_header_bar_new();
  gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
  header_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  window->location_box = gtk_stack_new();
  gtk_widget_set_hexpand(window->location_box, TRUE);
  gtk_box_append(GTK_BOX(header_controls), window->location_box);
  window->custom_action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  actions = g_object_get_data(G_OBJECT(application), "trout_actions");
  action_index = 0;
  while (action_index < actions->len) {
    action = g_ptr_array_index(actions, action_index);
    if ((action->appearance & trout_action_appearance_button) != 0) {
      button = trout_action_make_button(action, window, 0);
      gtk_widget_set_visible(button, FALSE);
      gtk_box_append(GTK_BOX(window->custom_action_box), button);
    }
    action_index += 1;
  }
  gtk_box_append(GTK_BOX(header_controls), window->custom_action_box);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), header_controls);
  menu_button = gtk_menu_button_new();
  gtk_menu_button_set_label(GTK_MENU_BUTTON(menu_button), "≡");
  popover = gtk_popover_new();
  window->action_popover = popover;
  menu_actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(menu_actions), trout_window_make_action_button("New Tab", "win.new-tab", window));
  gtk_box_append(GTK_BOX(menu_actions), trout_window_make_action_button("New Window", "win.new-window", window));
  button = gtk_toggle_button_new();
  trout_window_set_menu_item_label(button, "Hidden");
  window->hidden_button = button;
  g_signal_connect(button, "clicked", G_CALLBACK(trout_window_action_button), window->window);
  g_object_set_data(G_OBJECT(button), "trout_action", (gpointer)"win.toggle-hidden");
  g_object_set_data(G_OBJECT(button), "trout_popover", popover);
  gtk_box_append(GTK_BOX(menu_actions), button);
  separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(menu_actions), separator);
  sort_index = 0;
  while (sort_index < trout_sort_order_count) {
    button = gtk_check_button_new();
    trout_window_set_menu_item_label(button, sort_labels[sort_index]);
    if (sort_index > 0) gtk_check_button_set_group(GTK_CHECK_BUTTON(button), GTK_CHECK_BUTTON(window->sort_buttons[0]));
    window->sort_buttons[sort_index] = button;
    g_object_set_data(G_OBJECT(button), "trout_sort_order", GUINT_TO_POINTER(sort_index));
    g_signal_connect(button, "toggled", G_CALLBACK(trout_window_sort_toggled), window);
    gtk_box_append(GTK_BOX(menu_actions), button);
    sort_index += 1;
  }
  gtk_popover_set_child(GTK_POPOVER(popover), menu_actions);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button), popover);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);
  gtk_window_set_titlebar(GTK_WINDOW(window->window), header);
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(window->notebook), TRUE);
  gtk_widget_set_vexpand(window->notebook, TRUE);
  gtk_window_set_child(GTK_WINDOW(window->window), window->notebook);
  g_signal_connect(window->notebook, "switch-page", G_CALLBACK(trout_window_page_changed), window);
  g_signal_connect(window->notebook, "page-added", G_CALLBACK(trout_window_page_added), window);
  g_signal_connect(window->notebook, "page-removed", G_CALLBACK(trout_window_page_removed), window);
  keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(keys), GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(trout_window_on_key), window);
  gtk_widget_add_controller(window->window, keys);
  trout_window_add_action(window, "new-tab", G_CALLBACK(trout_window_action_new_tab));
  trout_window_add_action(window, "new-window", G_CALLBACK(trout_window_action_new_window));
  trout_window_add_action(window, "toggle-hidden", G_CALLBACK(trout_window_action_toggle_hidden));
  trout_window_add_action(window, "edit-location", G_CALLBACK(trout_window_action_edit_location));
  trout_window_add_tab(window, directory);
  gtk_window_present(GTK_WINDOW(window->window));
  return window->window;
}

gboolean
trout_window_on_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  GtkNotebook *notebook;
  GtkWidget *focus;
  char *last_character;
  gunichar character;
  window = user_data;
  (void)controller;
  (void)keycode;
  if ((state & GDK_CONTROL_MASK) != 0) {
    keyval = gdk_keyval_to_lower(keyval);
    if (keyval == GDK_KEY_t) gtk_widget_activate_action(window->window, "win.new-tab", 0);
    else if (keyval == GDK_KEY_n) gtk_widget_activate_action(window->window, "win.new-window", 0);
    else if (keyval == GDK_KEY_h) gtk_widget_activate_action(window->window, "win.toggle-hidden", 0);
    else if (keyval == GDK_KEY_l) gtk_widget_activate_action(window->window, "win.edit-location", 0);
    else if (keyval == GDK_KEY_w) {
      notebook = GTK_NOTEBOOK(window->notebook);
      if (gtk_notebook_get_n_pages(notebook) > trout_window_single_tab_count) {
        gtk_notebook_remove_page(notebook, gtk_notebook_get_current_page(notebook));
      } else gtk_window_close(GTK_WINDOW(window->window));
    } else if (keyval == GDK_KEY_q) gtk_window_close(GTK_WINDOW(window->window));
    else return FALSE;
    return TRUE;
  }
  tab = trout_window_get_current_tab(window);
  if (tab == 0) return FALSE;
  focus = gtk_root_get_focus(GTK_ROOT(window->window));
  if (focus != tab->grid && (focus == 0 || !gtk_widget_is_ancestor(focus, tab->grid))) return FALSE;
  if ((state & GDK_MODIFIER_MASK & ~(GDK_SHIFT_MASK | GDK_LOCK_MASK)) != 0) return FALSE;
  if (keyval == GDK_KEY_Escape && tab->typeahead_prefix->len > 0) {
    trout_tab_clear_typeahead(tab);
    return TRUE;
  }
  if (keyval == GDK_KEY_BackSpace && tab->typeahead_prefix->len > 0) {
    last_character = g_utf8_prev_char(tab->typeahead_prefix->str + tab->typeahead_prefix->len);
    g_string_truncate(tab->typeahead_prefix, last_character - tab->typeahead_prefix->str);
    if (tab->typeahead_prefix->len > 0) {
      trout_tab_reset_typeahead_timeout(tab);
      trout_tab_select_typeahead_match(tab);
    } else trout_tab_clear_typeahead(tab);
    return TRUE;
  }
  character = gdk_keyval_to_unicode(keyval);
  if (!g_unichar_isprint(character)) return FALSE;
  g_string_append_unichar(tab->typeahead_prefix, character);
  trout_tab_reset_typeahead_timeout(tab);
  trout_tab_select_typeahead_match(tab);
  return TRUE;
}

void
trout_application_activate(GtkApplication *application, gpointer user_data)
{
  GFile *directory;
  directory = g_file_new_for_path(g_get_home_dir());
  trout_window_create(application, directory);
  g_object_unref(directory);
  (void)user_data;
}

void
trout_application_startup(GtkApplication *application, gpointer user_data)
{
  GtkCssProvider *provider;
  provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, trout_compact_css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
  (void)application;
  (void)user_data;
}

void
trout_application_shutdown(GApplication *application, gpointer user_data)
{
  trout_preferences *preferences;
  trout_thumbnailer *thumbnailer;
  preferences = g_object_get_data(G_OBJECT(application), "trout_preferences");
  if (preferences->save_source_id != 0) {
    g_source_remove(preferences->save_source_id);
    preferences->save_source_id = 0;
  }
  if (trout_preferences_save(preferences) != trout_status_success) {
    g_warning("Trout could not persist its last zoom level");
  }
  thumbnailer = g_object_get_data(G_OBJECT(application), "trout_thumbnailer");
  if (trout_thumbnailer_stop(thumbnailer) != trout_status_success) {
    g_warning("Could not complete thumbnail shutdown");
  }
  (void)user_data;
}

void
trout_application_open(GApplication *application, GFile **files, gint file_count, char *hint, gpointer user_data)
{
  GFile *directory;
  if (file_count > 0) directory = files[0];
  else {
    directory = g_file_new_for_path(g_get_home_dir());
    trout_window_create(GTK_APPLICATION(application), directory);
    g_object_unref(directory);
    (void)hint;
    (void)user_data;
    return;
  }
  trout_window_create(GTK_APPLICATION(application), directory);
  (void)hint;
  (void)user_data;
}

#ifndef trout_test_build
int
main(int argument_count, char **argument_vector)
{
  GtkApplication *application;
  trout_preferences *preferences;
  GPtrArray *actions;
  trout_thumbnailer *thumbnailer;
  int result;
  application = gtk_application_new(trout_application_id, G_APPLICATION_HANDLES_OPEN);
  if (trout_thumbnailer_create(&thumbnailer) != trout_status_success) {
    g_object_unref(application);
    return trout_status_error;
  }
  preferences = trout_preferences_create();
  actions = trout_action_config_load();
  g_object_set_data_full(G_OBJECT(application), "trout_preferences", preferences, trout_preferences_destroy);
  g_object_set_data_full(G_OBJECT(application), "trout_actions", actions, trout_action_config_destroy);
  g_object_set_data_full(G_OBJECT(application), "trout_thumbnailer", thumbnailer, trout_thumbnailer_destroy);
  g_signal_connect(application, "startup", G_CALLBACK(trout_application_startup), 0);
  g_signal_connect(application, "activate", G_CALLBACK(trout_application_activate), 0);
  g_signal_connect(application, "open", G_CALLBACK(trout_application_open), 0);
  g_signal_connect(application, "shutdown", G_CALLBACK(trout_application_shutdown), 0);
  result = g_application_run(G_APPLICATION(application), argument_count, argument_vector);
  g_object_unref(application);
  return result;
}
#endif
