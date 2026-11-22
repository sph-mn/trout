gboolean
trout_filter_hidden(gpointer item, gpointer user_data)
{
  (void)user_data;
  return !g_file_info_get_is_hidden(item);
}

int
trout_file_sort_group(GFileInfo *file_info, gpointer user_data)
{
  (void)user_data;
  return g_file_info_get_file_type(file_info) != G_FILE_TYPE_DIRECTORY;
}

guint64
trout_file_sort_modified(GFileInfo *file_info, gpointer user_data)
{
  guint64 modified;
  guint32 modified_microseconds;
  (void)user_data;
  modified = g_file_info_get_attribute_uint64(file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  modified_microseconds = g_file_info_get_attribute_uint32(file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
  return modified * G_USEC_PER_SEC + modified_microseconds;
}

gint64
trout_file_sort_size(GFileInfo *file_info, gpointer user_data)
{
  (void)user_data;
  return g_file_info_get_size(file_info);
}

gint
trout_file_sort_name_compare(gconstpointer left, gconstpointer right, gpointer user_data)
{
  GFileInfo *left_info;
  GFileInfo *right_info;
  char *left_key;
  char *right_key;
  gint comparison;
  gboolean descending;
  left_info = (GFileInfo *)left;
  right_info = (GFileInfo *)right;
  left_key = g_object_get_data(G_OBJECT(left_info), "trout_collation_key");
  right_key = g_object_get_data(G_OBJECT(right_info), "trout_collation_key");
  comparison = strcmp(left_key, right_key);
  descending = user_data != 0;
  if (comparison < 0) return descending ? 1 : -1;
  if (comparison > 0) return descending ? -1 : 1;
  return 0;
}

GtkSorter *
trout_create_file_sorter(guint sort_order)
{
  GtkMultiSorter *sorter;
  GtkExpression *group_expression;
  GtkExpression *sort_expression;
  GtkCustomSorter *name_sorter;
  GtkNumericSorter *numeric_sorter;
  GtkSortType sort_direction;
  gboolean modified_sort;
  gboolean size_sort;
  gboolean descending;
  sorter = gtk_multi_sorter_new();
  group_expression = gtk_cclosure_expression_new(G_TYPE_INT, 0, 0, 0, G_CALLBACK(trout_file_sort_group), 0, 0);
  gtk_multi_sorter_append(sorter, GTK_SORTER(gtk_numeric_sorter_new(group_expression)));
  modified_sort = sort_order == trout_sort_modified_descending || sort_order == trout_sort_modified_ascending;
  size_sort = sort_order == trout_sort_size_descending || sort_order == trout_sort_size_ascending;
  descending = sort_order == trout_sort_name_descending || sort_order == trout_sort_modified_descending || sort_order == trout_sort_size_descending;
  sort_direction = descending ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING;
  if (modified_sort) sort_expression = gtk_cclosure_expression_new(G_TYPE_UINT64, 0, 0, 0, G_CALLBACK(trout_file_sort_modified), 0, 0);
  else if (size_sort) sort_expression = gtk_cclosure_expression_new(G_TYPE_INT64, 0, 0, 0, G_CALLBACK(trout_file_sort_size), 0, 0);
  else sort_expression = 0;
  if (!modified_sort && !size_sort) {
    name_sorter = gtk_custom_sorter_new(trout_file_sort_name_compare, GINT_TO_POINTER(descending), 0);
    gtk_multi_sorter_append(sorter, GTK_SORTER(name_sorter));
  } else {
    numeric_sorter = gtk_numeric_sorter_new(sort_expression);
    gtk_numeric_sorter_set_sort_order(numeric_sorter, sort_direction);
    gtk_multi_sorter_append(sorter, GTK_SORTER(numeric_sorter));
    name_sorter = gtk_custom_sorter_new(trout_file_sort_name_compare, 0, 0);
    gtk_multi_sorter_append(sorter, GTK_SORTER(name_sorter));
  }
  return GTK_SORTER(sorter);
}

void
trout_tab_set_sort_order(trout_tab *tab, guint sort_order)
{
  GtkSorter *sorter;
  if (tab->sort_order != sort_order) {
    tab->sort_order = sort_order;
    sorter = trout_create_file_sorter(sort_order);
    gtk_sort_list_model_set_sorter(tab->sorted_list, sorter);
    g_object_unref(sorter);
  }
}

void
trout_tab_update_status(trout_tab *tab)
{
  guint file_count;
  GtkBitset *selected;
  guint selected_count;
  char *text;
  file_count = g_list_model_get_n_items(G_LIST_MODEL(tab->filtered_list));
  selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  selected_count = gtk_bitset_get_size(selected);
  if (gtk_directory_list_is_loading(tab->directory_list)) text = g_strdup_printf("%u selected", selected_count);
  else text = g_strdup_printf("%u items    %u selected", file_count, selected_count);
  gtk_label_set_text(GTK_LABEL(tab->status_label), text);
  g_free(text);
  gtk_bitset_unref(selected);
}

gboolean
trout_tab_on_typeahead_timeout(gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  tab->typeahead_timeout_id = 0;
  g_string_truncate(tab->typeahead_prefix, 0);
  return G_SOURCE_REMOVE;
}

void
trout_tab_reset_typeahead_timeout(trout_tab *tab)
{
  if (tab->typeahead_timeout_id != 0) g_source_remove(tab->typeahead_timeout_id);
  tab->typeahead_timeout_id = g_timeout_add(trout_typeahead_timeout_milliseconds, trout_tab_on_typeahead_timeout, tab);
}

void
trout_tab_clear_typeahead(trout_tab *tab)
{
  if (tab->typeahead_timeout_id != 0) {
    g_source_remove(tab->typeahead_timeout_id);
    tab->typeahead_timeout_id = 0;
  }
  g_string_truncate(tab->typeahead_prefix, 0);
}

void
trout_tab_select_typeahead_match(trout_tab *tab)
{
  GFileInfo *file_info;
  GQuark folded_name_key;
  char *normalized_prefix;
  char *folded_prefix;
  char *normalized_name;
  char *folded_name;
  guint item_count;
  guint position;
  gboolean matches;
  folded_name_key = g_quark_from_static_string("trout_folded_name");
  normalized_prefix = g_utf8_normalize(tab->typeahead_prefix->str, -1, G_NORMALIZE_ALL);
  folded_prefix = g_utf8_casefold(normalized_prefix, -1);
  g_free(normalized_prefix);
  item_count = g_list_model_get_n_items(G_LIST_MODEL(tab->sorted_list));
  position = 0;
  while (position < item_count) {
    file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
    folded_name = g_object_get_qdata(G_OBJECT(file_info), folded_name_key);
    if (folded_name == 0) {
      normalized_name = g_utf8_normalize(g_file_info_get_display_name(file_info), -1, G_NORMALIZE_ALL);
      folded_name = g_utf8_casefold(normalized_name, -1);
      g_free(normalized_name);
      g_object_set_qdata_full(G_OBJECT(file_info), folded_name_key, folded_name, g_free);
    }
    matches = g_str_has_prefix(folded_name, folded_prefix);
    g_object_unref(file_info);
    if (matches) break;
    position += 1;
  }
  g_free(folded_prefix);
  if (position < item_count) {
    gtk_grid_view_scroll_to(GTK_GRID_VIEW(tab->grid), position, GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, 0);
  } else gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(tab->selection));
}

void
trout_tab_item_count_changed(GtkFilterListModel *model, GParamSpec *property, gpointer user_data)
{
  (void)model;
  (void)property;
  trout_tab_update_status(user_data);
}

void
trout_tab_selection_changed(GtkSelectionModel *model, guint position, guint count, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  trout_tab_update_status(tab);
  if (trout_window_get_current_tab(tab->window) == tab) trout_window_update_custom_actions(tab->window);
  (void)model;
  (void)position;
  (void)count;
}

gboolean trout_tab_publish_directory_range(trout_tab *tab, GListModel *model, guint position, guint removed, guint added);

void
trout_tab_on_directory_items_changed(GListModel *model, guint position, guint removed, guint added, gpointer user_data)
{
  trout_tab *tab;
  GFileInfo *file_info;
  GFile *file;
  char *name;
  char *collation_key;
  GFileType file_type;
  guint index;
  tab = user_data;
  index = position;
  while (index < position + added) {
    file_info = g_list_model_get_item(model, index);
    name = (char *)g_file_info_get_name(file_info);
    collation_key = g_utf8_collate_key(g_file_info_get_display_name(file_info), -1);
    g_object_set_data_full(G_OBJECT(file_info), "trout_collation_key", collation_key, g_free);
    file_type = g_file_info_get_file_type(file_info);
    if (file_type == G_FILE_TYPE_DIRECTORY) {
      file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
      if (trout_thumbnailer_schedule_directory(tab->window->thumbnailer, file) != trout_status_success) {
        g_debug("Could not queue thumbnail directory %s", name);
      }
    } else if (file_type == G_FILE_TYPE_REGULAR || file_type == G_FILE_TYPE_SYMBOLIC_LINK) {
      if (trout_thumbnailer_schedule_file_info(tab->window->thumbnailer, file_info) != trout_status_success) {
        g_debug("Could not queue thumbnail file %s", name);
      }
    }
    g_object_unref(file_info);
    index += 1;
  }
  if (removed == 0 && added == 0) return;
  if (tab->directory_dirty || gtk_directory_list_is_loading(tab->directory_list) || tab->directory_range_pending) {
    tab->directory_dirty = TRUE;
    tab->directory_range_pending = FALSE;
  } else {
    tab->directory_range_pending = TRUE;
    tab->directory_range_position = position;
    tab->directory_range_removed = removed;
    tab->directory_range_added = added;
  }
}

static void
trout_tab_restore_directory_selection(trout_tab *tab)
{
  GtkBitset *selected_positions;
  GtkBitset *selection_mask;
  GFileInfo *file_info;
  GFile *file;
  guint item_count;
  guint position;
  GHashTable *selected_files;
  if (tab->directory_selection == 0 || gtk_sort_list_model_get_pending(tab->sorted_list) != 0) return;
  selected_files = tab->directory_selection;
  tab->directory_selection = 0;
  if (g_hash_table_size(selected_files) == 0) {
    g_hash_table_unref(selected_files);
    return;
  }
  selected_positions = gtk_bitset_new_empty();
  item_count = g_list_model_get_n_items(G_LIST_MODEL(tab->sorted_list));
  position = 0;
  while (position < item_count) {
    file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
    file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
    if (g_hash_table_contains(selected_files, file)) gtk_bitset_add(selected_positions, position);
    g_object_unref(file_info);
    position += 1;
  }
  selection_mask = gtk_bitset_new_range(0, item_count);
  gtk_selection_model_set_selection(GTK_SELECTION_MODEL(tab->selection), selected_positions, selection_mask);
  gtk_bitset_unref(selection_mask);
  gtk_bitset_unref(selected_positions);
  g_hash_table_unref(selected_files);
}

static gboolean
trout_file_equal(gconstpointer left, gconstpointer right)
{
  return g_file_equal((GFile *)left, (GFile *)right);
}

void
trout_tab_save_directory_selection(trout_tab *tab)
{
  GtkBitset *selected;
  GtkBitsetIter iterator;
  GHashTable *selected_files;
  GFileInfo *file_info;
  GFile *file;
  guint position;
  selected_files = tab->directory_selection;
  tab->directory_selection = 0;
  if (selected_files == 0) selected_files = g_hash_table_new_full(g_file_hash, trout_file_equal, g_object_unref, 0);
  selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  if (gtk_bitset_iter_init_first(&iterator, selected, &position)) {
    do {
      file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
      file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
      g_hash_table_add(selected_files, g_object_ref(file));
      g_object_unref(file_info);
    } while (gtk_bitset_iter_next(&iterator, &position));
  }
  gtk_bitset_unref(selected);
  tab->directory_selection = selected_files;
}

gboolean
trout_tab_publish_directory_range(trout_tab *tab, GListModel *model, guint position, guint removed, guint added)
{
  GtkBitset *selected;
  gpointer *added_items;
  gboolean has_selection;
  guint index;
  if (tab->directory_dirty || gtk_directory_list_is_loading(tab->directory_list)) return FALSE;
  selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  has_selection = gtk_bitset_get_size(selected) != 0;
  gtk_bitset_unref(selected);
  if (has_selection || tab->directory_selection != 0) trout_tab_save_directory_selection(tab);
  added_items = added != 0 ? g_new(gpointer, added) : 0;
  index = 0;
  while (index < added) {
    added_items[index] = g_list_model_get_item(model, position + index);
    index += 1;
  }
  g_list_store_splice(tab->display_list, position, removed, added_items, added);
  index = 0;
  while (index < added) {
    g_object_unref(added_items[index]);
    index += 1;
  }
  g_free(added_items);
  if (tab->directory_selection != 0) trout_tab_restore_directory_selection(tab);
  return TRUE;
}

static gboolean
trout_file_info_display_equal(GFileInfo *left, GFileInfo *right)
{
  GFile *left_file;
  GFile *right_file;
  GIcon *left_icon;
  GIcon *right_icon;
  left_file = G_FILE(g_file_info_get_attribute_object(left, "standard::file"));
  right_file = G_FILE(g_file_info_get_attribute_object(right, "standard::file"));
  left_icon = g_file_info_get_icon(left);
  right_icon = g_file_info_get_icon(right);
  return g_file_equal(left_file, right_file) &&
      g_strcmp0(g_file_info_get_name(left), g_file_info_get_name(right)) == 0 &&
      g_strcmp0(g_file_info_get_display_name(left), g_file_info_get_display_name(right)) == 0 &&
      g_file_info_get_file_type(left) == g_file_info_get_file_type(right) &&
      (left_icon == right_icon || (left_icon != 0 && right_icon != 0 && g_icon_equal(left_icon, right_icon))) &&
      g_strcmp0(g_file_info_get_attribute_string(left, "standard::fast-content-type"),
          g_file_info_get_attribute_string(right, "standard::fast-content-type")) == 0 &&
      g_file_info_get_is_hidden(left) == g_file_info_get_is_hidden(right) &&
      g_file_info_get_size(left) == g_file_info_get_size(right) &&
      g_file_info_get_attribute_uint64(left, G_FILE_ATTRIBUTE_TIME_MODIFIED) ==
          g_file_info_get_attribute_uint64(right, G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
      g_file_info_get_attribute_uint32(left, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) ==
          g_file_info_get_attribute_uint32(right, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
}

static void
trout_tab_publish_directory(trout_tab *tab)
{
  GFileInfo *old_file_info;
  GPtrArray *snapshot;
  gpointer *added_items;
  guint position;
  guint item_count;
  guint old_item_count;
  guint common_prefix;
  guint common_suffix;
  guint added_count;
  gboolean items_match;
  snapshot = g_ptr_array_new_with_free_func(g_object_unref);
  item_count = g_list_model_get_n_items(G_LIST_MODEL(tab->directory_list));
  position = 0;
  while (position < item_count) {
    g_ptr_array_add(snapshot, g_list_model_get_item(G_LIST_MODEL(tab->directory_list), position));
    position += 1;
  }
  old_item_count = g_list_model_get_n_items(G_LIST_MODEL(tab->display_list));
  common_prefix = 0;
  while (common_prefix < old_item_count && common_prefix < snapshot->len) {
    old_file_info = g_list_model_get_item(G_LIST_MODEL(tab->display_list), common_prefix);
    items_match = trout_file_info_display_equal(old_file_info, g_ptr_array_index(snapshot, common_prefix));
    g_object_unref(old_file_info);
    if (!items_match) break;
    common_prefix += 1;
  }
  common_suffix = 0;
  while (common_suffix < old_item_count - common_prefix && common_suffix < snapshot->len - common_prefix) {
    old_file_info = g_list_model_get_item(G_LIST_MODEL(tab->display_list), old_item_count - common_suffix - 1);
    items_match = trout_file_info_display_equal(old_file_info, g_ptr_array_index(snapshot, snapshot->len - common_suffix - 1));
    g_object_unref(old_file_info);
    if (!items_match) break;
    common_suffix += 1;
  }
  tab->directory_dirty = FALSE;
  tab->directory_range_pending = FALSE;
  if (common_prefix == old_item_count && common_prefix == snapshot->len) {
    g_ptr_array_unref(snapshot);
    trout_tab_restore_directory_selection(tab);
    return;
  }
  trout_tab_save_directory_selection(tab);
  added_count = snapshot->len - common_prefix - common_suffix;
  added_items = added_count != 0 ? snapshot->pdata + common_prefix : 0;
  g_list_store_splice(tab->display_list, common_prefix, old_item_count - common_prefix - common_suffix, added_items, added_count);
  g_ptr_array_unref(snapshot);
  trout_tab_restore_directory_selection(tab);
}

static gboolean
trout_tab_refresh_directory(gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  if (!gtk_directory_list_is_loading(tab->directory_list)) {
    if (tab->directory_dirty) trout_tab_publish_directory(tab);
    else if (tab->directory_range_pending) {
      tab->directory_range_pending = FALSE;
      if (!trout_tab_publish_directory_range(tab, G_LIST_MODEL(tab->directory_list), tab->directory_range_position,
          tab->directory_range_removed, tab->directory_range_added)) tab->directory_dirty = TRUE;
    }
  }
  return G_SOURCE_CONTINUE;
}

static void
trout_tab_on_directory_loading_changed(GtkDirectoryList *directory_list, GParamSpec *property, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  trout_tab_update_status(tab);
  if (!gtk_directory_list_is_loading(directory_list) && tab->directory_dirty) trout_tab_publish_directory(tab);
  (void)property;
}

static void
trout_tab_on_sort_pending_changed(GtkSortListModel *model, GParamSpec *property, gpointer user_data)
{
  trout_tab_restore_directory_selection(user_data);
  (void)model;
  (void)property;
}

void
trout_tab_update_location(trout_tab *tab)
{
  GPtrArray *ancestors;
  GFile *file;
  GFile *parent;
  GtkWidget *button;
  char *name;
  guint index;
  ancestors = g_ptr_array_new_with_free_func(g_object_unref);
  file = g_object_ref(tab->directory);
  while (file != 0) {
    g_ptr_array_add(ancestors, file);
    parent = g_file_get_parent(file);
    file = parent;
  }
  while (gtk_widget_get_first_child(tab->path_buttons) != 0) {
    gtk_box_remove(GTK_BOX(tab->path_buttons), gtk_widget_get_first_child(tab->path_buttons));
  }
  index = ancestors->len;
  while (index > 0) {
    index -= 1;
    file = g_ptr_array_index(ancestors, index);
    name = g_file_get_basename(file);
    if (name == 0) name = g_strdup("/");
    button = gtk_toggle_button_new_with_label(name);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), file == tab->directory);
    g_free(name);
    g_object_set_data_full(G_OBJECT(button), "trout_directory", g_object_ref(file), g_object_unref);
    g_signal_connect(button, "clicked", G_CALLBACK(trout_tab_on_path_clicked), tab);
    gtk_box_append(GTK_BOX(tab->path_buttons), button);
  }
  button = gtk_button_new();
  gtk_widget_set_size_request(button, 50, -1);
  g_signal_connect(button, "clicked", G_CALLBACK(trout_tab_on_edit_location), tab);
  gtk_box_append(GTK_BOX(tab->path_buttons), button);
  gtk_stack_set_visible_child_name(GTK_STACK(tab->location_stack), "buttons");
  if (tab->tab_label != 0) {
    name = g_file_get_basename(tab->directory);
    if (name == 0) name = g_file_get_parse_name(tab->directory);
    gtk_label_set_text(GTK_LABEL(tab->tab_label), name);
    g_free(name);
  }
  g_ptr_array_unref(ancestors);
}

void
trout_tab_recovery_destroy(gpointer user_data)
{
  trout_directory_recovery *recovery;
  recovery = user_data;
  g_object_unref(recovery->directory);
  g_free(recovery);
}

void
trout_tab_update_directory_monitor(trout_tab *tab)
{
  GFile *parent;
  GError *error;
  if (tab->directory_parent_monitor != 0) {
    g_file_monitor_cancel(tab->directory_parent_monitor);
    g_object_unref(tab->directory_parent_monitor);
    tab->directory_parent_monitor = 0;
  }
  parent = g_file_get_parent(tab->directory);
  if (parent == 0) return;
  error = 0;
  tab->directory_parent_monitor = g_file_monitor_directory(parent, G_FILE_MONITOR_WATCH_MOVES, 0, &error);
  if (error != 0) g_error_free(error);
  if (tab->directory_parent_monitor != 0) {
    g_signal_connect(tab->directory_parent_monitor, "changed", G_CALLBACK(trout_tab_directory_monitor_changed), tab);
  }
  g_object_unref(parent);
}

void
trout_tab_directory_monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event, gpointer user_data)
{
  trout_tab *tab;
  gboolean directory_event;
  tab = user_data;
  directory_event = event == G_FILE_MONITOR_EVENT_DELETED ||
      event == G_FILE_MONITOR_EVENT_MOVED_OUT ||
      event == G_FILE_MONITOR_EVENT_RENAMED ||
      event == G_FILE_MONITOR_EVENT_MOVED;
  if (directory_event && (g_file_equal(file, tab->directory) || (other_file != 0 && g_file_equal(other_file, tab->directory)))) {
    trout_tab_recover_directory(tab);
  }
  (void)monitor;
}

void
trout_tab_recover_directory(trout_tab *tab)
{
  trout_directory_recovery *recovery;
  GTask *task;
  if (tab->directory_recovery_pending) return;
  tab->directory_recovery_pending = TRUE;
  recovery = g_new(trout_directory_recovery, 1);
  recovery->directory = g_object_ref(tab->directory);
  recovery->generation = tab->directory_generation;
  task = g_task_new(tab->root, 0, trout_tab_recover_directory_done, tab);
  g_task_set_task_data(task, recovery, trout_tab_recovery_destroy);
  g_task_run_in_thread(task, trout_tab_recover_directory_worker);
  g_object_unref(task);
}

void
trout_tab_recover_directory_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  trout_directory_recovery *recovery;
  GFileInfo *info;
  GFile *candidate;
  GFile *parent;
  GError *error;
  recovery = task_data;
  info = 0;
  error = 0;
  info = g_file_query_info(recovery->directory, "standard::type", G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (info != 0 && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
    g_object_unref(info);
    g_task_return_pointer(task, 0, 0);
    return;
  }
  if (info != 0) g_object_unref(info);
  if (error != 0 && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY)) {
    g_error_free(error);
    g_task_return_pointer(task, 0, 0);
    return;
  }
  g_clear_error(&error);
  candidate = g_file_get_parent(recovery->directory);
  while (candidate != 0 && (cancellable == 0 || !g_cancellable_is_cancelled(cancellable))) {
    error = 0;
    info = g_file_query_info(candidate, "standard::type", G_FILE_QUERY_INFO_NONE, cancellable, &error);
    if (info != 0 && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
      g_object_unref(info);
      g_clear_error(&error);
      g_task_return_pointer(task, candidate, g_object_unref);
      return;
    }
    if (info != 0) g_object_unref(info);
    g_clear_error(&error);
    parent = g_file_get_parent(candidate);
    g_object_unref(candidate);
    candidate = parent;
  }
  if (candidate != 0) g_object_unref(candidate);
  g_task_return_pointer(task, 0, 0);
  (void)source_object;
}

void
trout_tab_recover_directory_done(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  trout_tab *tab;
  trout_directory_recovery *recovery;
  GFile *directory;
  GError *error;
  tab = user_data;
  recovery = g_task_get_task_data(G_TASK(result));
  error = 0;
  directory = g_task_propagate_pointer(G_TASK(result), &error);
  if (error != 0) g_error_free(error);
  if (recovery->generation == tab->directory_generation && g_file_equal(recovery->directory, tab->directory)) {
    tab->directory_recovery_pending = FALSE;
    if (directory != 0 && gtk_widget_get_root(tab->root) != 0) trout_tab_set_directory(tab, directory);
  }
  if (directory != 0) g_object_unref(directory);
  (void)source_object;
}

void
trout_tab_set_directory(trout_tab *tab, GFile *directory)
{
  gboolean directory_changed;
  trout_tab_cancel_location_completion(tab);
  trout_tab_clear_typeahead(tab);
  directory_changed = !g_file_equal(tab->directory, directory);
  tab->directory_generation += 1;
  tab->directory_recovery_pending = FALSE;
  if (tab->directory != 0) g_object_unref(tab->directory);
  tab->directory = g_object_ref(directory);
  if (directory_changed) {
    trout_tab_update_directory_monitor(tab);
    trout_tab_update_free_space(tab);
    tab->directory_range_pending = FALSE;
    if (tab->directory_selection != 0) {
      g_hash_table_unref(tab->directory_selection);
      tab->directory_selection = 0;
    }
    g_list_store_remove_all(tab->display_list);
    tab->directory_dirty = TRUE;
  }
  gtk_directory_list_set_file(tab->directory_list, directory);
  trout_tab_update_location(tab);
  gtk_widget_grab_focus(tab->grid);
  trout_tab_update_status(tab);
  if (trout_window_get_current_tab(tab->window) == tab) trout_window_update_custom_actions(tab->window);
}

void
trout_tab_reload_directory(trout_tab *tab)
{
  tab->directory_dirty = TRUE;
  tab->directory_range_pending = FALSE;
  gtk_directory_list_set_file(tab->directory_list, 0);
  gtk_directory_list_set_file(tab->directory_list, tab->directory);
}

void
trout_tab_on_path_clicked(GtkButton *button, gpointer user_data)
{
  trout_tab *tab;
  GFile *directory;
  tab = user_data;
  directory = g_object_get_data(G_OBJECT(button), "trout_directory");
  trout_tab_set_directory(tab, directory);
}

void
trout_tab_on_edit_location(GtkButton *button, gpointer user_data)
{
  trout_tab_edit_location(user_data);
  (void)button;
}

void
trout_tab_on_location_activate(GtkEntry *entry, gpointer user_data)
{
  trout_tab *tab;
  GFile *directory;
  GFile *home_directory;
  char *text;
  tab = user_data;
  text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
  if (text[0] == '~' && (text[1] == '/' || text[1] == '\0')) {
    home_directory = g_file_new_for_path(g_get_home_dir());
    if (text[1] == '/') {
      directory = g_file_resolve_relative_path(home_directory, text + 2);
      g_object_unref(home_directory);
    } else directory = home_directory;
  } else directory = g_file_new_for_commandline_arg(text);
  trout_tab_set_directory(tab, directory);
  gtk_stack_set_visible_child_name(GTK_STACK(tab->location_stack), "buttons");
  g_object_unref(directory);
  g_free(text);
}

gboolean
trout_tab_on_location_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
  trout_tab *tab;
  (void)controller;
  (void)keycode;
  tab = user_data;
  if (keyval == GDK_KEY_Tab && (state & GDK_CONTROL_MASK) == 0 && gtk_editable_get_position(GTK_EDITABLE(tab->location_entry)) == gtk_entry_get_text_length(GTK_ENTRY(tab->location_entry))) {
    if (!tab->completion_has_suffix) {
      tab->completion_has_suffix = FALSE;
      trout_tab_schedule_location_completion(tab, FALSE);
    } else {
      trout_tab_cancel_location_completion(tab);
      tab->completion_has_suffix = FALSE;
      gtk_editable_set_position(GTK_EDITABLE(tab->location_entry), -1);
    }
    return TRUE;
  }
  if (keyval != GDK_KEY_Escape) return FALSE;
  gtk_stack_set_visible_child_name(GTK_STACK(tab->location_stack), "buttons");
  gtk_widget_grab_focus(tab->grid);
  return TRUE;
}

void
trout_tab_on_location_focus_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  trout_tab_cancel_location_completion(tab);
  gtk_stack_set_visible_child_name(GTK_STACK(tab->location_stack), "buttons");
  (void)controller;
}

void
trout_tab_edit_location(trout_tab *tab)
{
  char *path;
  trout_tab_clear_typeahead(tab);
  trout_tab_cancel_location_completion(tab);
  path = g_file_get_path(tab->directory);
  if (path == 0) path = g_file_get_uri(tab->directory);
  tab->completion_suppressed = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(tab->location_entry), path);
  tab->completion_suppressed = FALSE;
  gtk_stack_set_visible_child_name(GTK_STACK(tab->location_stack), "entry");
  gtk_widget_grab_focus(tab->location_entry);
  gtk_editable_select_region(GTK_EDITABLE(tab->location_entry), 0, -1);
  g_free(path);
}

void
trout_tab_schedule_location_completion(trout_tab *tab, gboolean highlight)
{
  tab->completion_highlight = highlight;
  if (tab->completion_source_id == 0) tab->completion_source_id = g_idle_add(trout_tab_complete_location_idle, tab);
}

gboolean
trout_tab_complete_location_idle(gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  tab->completion_source_id = 0;
  if (!tab->completion_suppressed && !tab->completion_has_suffix) trout_tab_complete_location(tab, tab->completion_highlight);
  return G_SOURCE_REMOVE;
}

void
trout_tab_cancel_location_completion(trout_tab *tab)
{
  if (tab->completion_source_id != 0) {
    g_source_remove(tab->completion_source_id);
    tab->completion_source_id = 0;
  }
}

void
trout_tab_complete_location(trout_tab *tab, gboolean highlight)
{
  GtkEditable *editable;
  char *text;
  char *completion_text;
  char *suffix;
  gint cursor_position;
  gint selection_start;
  gint selection_end;
  glong text_length;
  editable = GTK_EDITABLE(tab->location_entry);
  text = g_strdup(gtk_editable_get_text(editable));
  if (text[0] == '\0' || text[strlen(text) - 1] == '/') {
    g_free(text);
    return;
  }
  cursor_position = gtk_editable_get_position(editable);
  if (cursor_position != g_utf8_strlen(text, -1) || gtk_editable_get_selection_bounds(editable, &selection_start, &selection_end)) {
    g_free(text);
    return;
  }
  if (g_str_has_prefix(text, "~/")) completion_text = g_build_filename(g_get_home_dir(), text + 2, (char *)0);
  else completion_text = g_strdup(text);
  tab->completion_suppressed = TRUE;
  suffix = g_filename_completer_get_completion_suffix(tab->filename_completer, completion_text);
  if (suffix != 0 && suffix[0] != '\0') {
    text_length = g_utf8_strlen(text, -1);
    gtk_editable_insert_text(editable, suffix, -1, &cursor_position);
    if (highlight) gtk_editable_select_region(editable, text_length, -1);
    else gtk_editable_set_position(editable, cursor_position);
    tab->completion_has_suffix = highlight;
  }
  tab->completion_suppressed = FALSE;
  g_free(suffix);
  g_free(completion_text);
  g_free(text);
}

void
trout_tab_on_location_changed(GtkEditable *editable, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  if (tab->completion_suppressed) return;
  tab->completion_has_suffix = FALSE;
  trout_tab_schedule_location_completion(tab, TRUE);
  (void)editable;
}

void
trout_tab_on_location_position_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  if (!tab->completion_suppressed) tab->completion_has_suffix = FALSE;
  (void)object;
  (void)property;
}

void
trout_tab_on_completion_data(GFilenameCompleter *completer, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  if (gtk_widget_has_focus(tab->location_entry) && !tab->completion_suppressed && !tab->completion_has_suffix) {
    trout_tab_schedule_location_completion(tab, tab->completion_highlight);
  }
  (void)completer;
}

void
trout_tab_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  trout_tab *tab;
  GtkWidget *row;
  GtkWidget *image;
  GtkWidget *overlay;
  GtkWidget *picture;
  GtkWidget *label;
  GtkGesture *context_click;
  GtkGesture *primary_click;
  GtkDragSource *drag_source;
  (void)factory;
  tab = user_data;
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, trout_icon_label_spacing);
  image = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(image), tab->icon_size);
  gtk_widget_set_size_request(image, tab->icon_size, tab->icon_size);
  overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), image);
  gtk_widget_set_size_request(overlay, tab->icon_size, tab->icon_size);
  picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_can_target(picture, FALSE);
  gtk_widget_set_size_request(picture, tab->icon_size, tab->icon_size);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), picture);
  gtk_overlay_set_measure_overlay(GTK_OVERLAY(overlay), picture, FALSE);
  label = gtk_label_new(0);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_max_width_chars(GTK_LABEL(label), trout_filename_max_width_chars);
  gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(row), overlay);
  gtk_box_append(GTK_BOX(row), label);
  g_object_set_data(G_OBJECT(row), "trout_list_item", list_item);
  g_object_set_data(G_OBJECT(list_item), "trout_image", image);
  g_object_set_data(G_OBJECT(list_item), "trout_picture", picture);
  g_object_set_data(G_OBJECT(list_item), "trout_label", label);
  context_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_click), GDK_BUTTON_SECONDARY);
  g_signal_connect(context_click, "pressed", G_CALLBACK(trout_tab_on_entry_context_pressed), tab);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(context_click));
  drag_source = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag_source, GDK_ACTION_COPY | GDK_ACTION_MOVE);
  g_signal_connect(drag_source, "drag-begin", G_CALLBACK(trout_tab_on_drag_begin), tab);
  g_signal_connect(drag_source, "drag-end", G_CALLBACK(trout_tab_on_drag_end), tab);
  g_signal_connect(drag_source, "drag-cancel", G_CALLBACK(trout_tab_on_drag_cancel), tab);
  g_signal_connect(drag_source, "prepare", G_CALLBACK(trout_tab_on_drag_prepare), tab);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drag_source));
  primary_click = gtk_gesture_click_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(primary_click), GTK_PHASE_CAPTURE);
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(primary_click), GDK_BUTTON_PRIMARY);
  g_signal_connect(primary_click, "pressed", G_CALLBACK(trout_tab_on_entry_pressed), tab);
  g_signal_connect(primary_click, "released", G_CALLBACK(trout_tab_on_entry_released), tab);
  g_signal_connect(primary_click, "cancel", G_CALLBACK(trout_tab_on_entry_cancelled), tab);
  g_signal_connect(primary_click, "stopped", G_CALLBACK(trout_tab_on_entry_stopped), tab);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(primary_click));
  gtk_list_item_set_child(list_item, row);
}

void
trout_tab_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  trout_tab *tab;
  GFileInfo *file_info;
  GIcon *icon;
  GtkWidget *image;
  GtkPicture *picture;
  GtkWidget *label;
  GFile *file;
  char *name;
  GFileType file_type;
  (void)factory;
  tab = user_data;
  file_info = gtk_list_item_get_item(list_item);
  image = g_object_get_data(G_OBJECT(list_item), "trout_image");
  picture = g_object_get_data(G_OBJECT(list_item), "trout_picture");
  label = g_object_get_data(G_OBJECT(list_item), "trout_label");
  icon = g_file_info_get_icon(file_info);
  gtk_widget_set_visible(image, TRUE);
  gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
  gtk_image_set_pixel_size(GTK_IMAGE(image), tab->icon_size);
  gtk_widget_set_size_request(image, tab->icon_size, tab->icon_size);
  gtk_widget_set_size_request(gtk_widget_get_parent(image), tab->icon_size, tab->icon_size);
  gtk_widget_set_size_request(GTK_WIDGET(picture), tab->icon_size, tab->icon_size);
  gtk_picture_set_paintable(picture, 0);
  file_type = g_file_info_get_file_type(file_info);
  name = (char *)g_file_info_get_display_name(file_info);
  gtk_label_set_text(GTK_LABEL(label), name);
  if (file_type == G_FILE_TYPE_REGULAR || file_type == G_FILE_TYPE_SYMBOLIC_LINK) {
    file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
    if (trout_thumbnailer_bind_picture(tab->window->thumbnailer, file, (char *)g_file_info_get_name(file_info), picture) != trout_status_success) {
      g_debug("Could not bind thumbnail for %s", name);
    }
  }
}

void
trout_tab_factory_unbind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  trout_tab *tab;
  GtkPicture *picture;
  (void)factory;
  tab = user_data;
  picture = g_object_get_data(G_OBJECT(list_item), "trout_picture");
  if (g_object_get_data(G_OBJECT(picture), "trout_thumbnail_uri") != 0) {
    trout_thumbnailer_unbind_picture(tab->window->thumbnailer, picture);
  }
}

