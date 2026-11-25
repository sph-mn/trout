/* Delete through the same bounded worker queue as copy/move. The former
 * recursive asynchronous job callbacks are no longer used. */
void
trout_tab_start_delete_operation(GtkWidget *parent_widget, GFile *source, trout_delete_job *parent)
{
  GtkRoot *root;
  trout_window *window;
  GPtrArray *operations;
  g_return_if_fail(parent == 0);
  root = gtk_widget_get_root(parent_widget);
  if (root == 0) return;
  window = g_object_get_data(G_OBJECT(root), "trout_window");
  if (window == 0) return;
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  g_ptr_array_add(operations, trout_file_transfer_pair_new(source, source));
  trout_transfer_request_enqueue(window, operations, trout_file_operation_delete);
}

void
trout_tab_copy_selection(trout_tab *tab, gboolean cut)
{
  GtkBitset *selection;
  GtkBitsetIter iter;
  guint position;
  GFileInfo *file_info;
  GFile *file;
  char *uri;
  GString *copied_files;
  GString *uri_list;
  GBytes *copied_bytes;
  GBytes *uri_bytes;
  GdkContentProvider *providers[2];
  GdkContentProvider *provider;
  gboolean has_selection;
  selection = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  has_selection = gtk_bitset_iter_init_first(&iter, selection, &position);
  if (!has_selection) {
    gtk_bitset_unref(selection);
    return;
  }
  copied_files = g_string_new(cut ? "cut\n" : "copy\n");
  uri_list = g_string_new(0);
  while (has_selection) {
    file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
    file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
    uri = g_file_get_uri(file);
    g_string_append(copied_files, uri);
    g_string_append_c(copied_files, '\n');
    g_string_append(uri_list, uri);
    g_string_append(uri_list, "\r\n");
    g_free(uri);
    g_object_unref(file_info);
    has_selection = gtk_bitset_iter_next(&iter, &position);
  }
  copied_bytes = g_bytes_new(copied_files->str, copied_files->len);
  uri_bytes = g_bytes_new(uri_list->str, uri_list->len);
  providers[0] = gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", copied_bytes);
  providers[1] = gdk_content_provider_new_for_bytes("text/uri-list", uri_bytes);
  provider = gdk_content_provider_new_union(providers, 2);
  gdk_clipboard_set_content(tab->window->clipboard, provider);
  g_object_unref(provider);
  g_bytes_unref(copied_bytes);
  g_bytes_unref(uri_bytes);
  g_string_free(copied_files, TRUE);
  g_string_free(uri_list, TRUE);
  gtk_bitset_unref(selection);
}

void
trout_tab_enqueue_files(trout_tab *tab, GFile *destination, GSList *files, trout_file_operation operation)
{
  GSList *file_node;
  GFile *file;
  GFile *target;
  char *name;
  GPtrArray *operations;
  file_node = files;
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  while (file_node != 0) {
    file = G_FILE(file_node->data);
    name = g_file_get_basename(file);
    if (name != 0) {
      target = g_file_get_child(destination, name);
      g_ptr_array_add(operations, trout_file_transfer_pair_new(file, target));
      g_object_unref(target);
      g_free(name);
    }
    file_node = file_node->next;
  }
  trout_transfer_request_enqueue(tab->window, operations, operation);
}

GdkContentProvider *
trout_tab_on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GtkBitset *selection;
  GtkBitsetIter iter;
  guint position;
  GFileInfo *file_info;
  GFile *file;
  GSList *files;
  gboolean has_selection;
  GdkFileList *file_list;
  GValue value = {0};
  GdkContentProvider *provider;
  tab = user_data;
  selection = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  has_selection = gtk_bitset_iter_init_first(&iter, selection, &position);
  if (!has_selection) {
    gtk_bitset_unref(selection);
    return 0;
  }
  files = 0;
  while (has_selection) {
    file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
    file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
    files = g_slist_prepend(files, g_object_ref(file));
    g_object_unref(file_info);
    has_selection = gtk_bitset_iter_next(&iter, &position);
  }
  files = g_slist_reverse(files);
  file_list = gdk_file_list_new_from_list(files);
  g_slist_free_full(files, g_object_unref);
  g_value_init(&value, GDK_TYPE_FILE_LIST);
  g_value_set_boxed(&value, file_list);
  provider = gdk_content_provider_new_for_value(&value);
  g_value_unset(&value);
  g_boxed_free(GDK_TYPE_FILE_LIST, file_list);
  gtk_bitset_unref(selection);
  (void)source;
  (void)x;
  (void)y;
  return provider;
}

void
trout_tab_clipboard_read_destroy(trout_clipboard_read *read_data)
{
  if (read_data->stream != 0) g_object_unref(read_data->stream);
  g_object_unref(read_data->window_widget);
  g_object_unref(read_data->destination);
  g_object_unref(read_data->parent_widget);
  if (read_data->bytes != 0) g_byte_array_unref(read_data->bytes);
  g_free(read_data);
}

void
trout_tab_paste(trout_tab *tab)
{
  char *mime_types[3];
  GFile *destination;
  trout_clipboard_read *read_data;
  mime_types[0] = "x-special/gnome-copied-files";
  mime_types[1] = "text/uri-list";
  mime_types[2] = 0;
  read_data = g_new0(trout_clipboard_read, 1);
  read_data->window = tab->window;
  read_data->window_widget = g_object_ref(tab->window->window);
  destination = tab->context_file_is_directory ? tab->context_file : tab->directory;
  read_data->destination = g_object_ref(destination);
  read_data->parent_widget = g_object_ref(tab->root);
  gdk_clipboard_read_async(tab->window->clipboard, (gpointer)mime_types, G_PRIORITY_DEFAULT, 0, trout_tab_clipboard_read_start, read_data);
}

void
trout_tab_clipboard_read_start(GObject *source, GAsyncResult *result, gpointer user_data)
{
  trout_clipboard_read *read_data;
  GError *error;
  read_data = user_data;
  error = 0;
  read_data->stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), result, 0, &error);
  if (read_data->stream == 0) {
    if (error != 0) {
      trout_tab_show_error(read_data->parent_widget, error);
      g_error_free(error);
    }
    trout_tab_clipboard_read_destroy(read_data);
    return;
  }
  read_data->bytes = g_byte_array_new();
  g_input_stream_read_bytes_async(read_data->stream, 8192, G_PRIORITY_DEFAULT, 0, trout_tab_clipboard_read_chunk, read_data);
}

void
trout_tab_clipboard_read_chunk(GObject *source, GAsyncResult *result, gpointer user_data)
{
  trout_clipboard_read *read_data;
  GBytes *chunk;
  gpointer chunk_data;
  gsize chunk_length;
  char *contents;
  char *uris_text;
  char **uris;
  char **uri;
  GFile *file;
  GFile *destination;
  char *name;
  GError *error;
  GPtrArray *operations;
  trout_file_transfer_pair *pair;
  trout_file_operation operation;
  read_data = user_data;
  error = 0;
  chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
  if (chunk == 0) {
    if (error != 0) {
      trout_tab_show_error(read_data->parent_widget, error);
      g_error_free(error);
    }
    trout_tab_clipboard_read_destroy(read_data);
    return;
  }
  chunk_data = g_bytes_unref_to_data(chunk, &chunk_length);
  if (chunk_length > 0) {
    g_byte_array_append(read_data->bytes, chunk_data, chunk_length);
    g_free(chunk_data);
    g_input_stream_read_bytes_async(read_data->stream, 8192, G_PRIORITY_DEFAULT, 0, trout_tab_clipboard_read_chunk, read_data);
    return;
  }
  g_free(chunk_data);
  contents = g_strndup((char *)read_data->bytes->data, read_data->bytes->len);
  uris_text = contents;
  if (g_str_has_prefix(contents, "cut\n")) uris_text += 4;
  else if (g_str_has_prefix(contents, "copy\n")) uris_text += 5;
  uris = g_uri_list_extract_uris(uris_text);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  operation = g_str_has_prefix(contents, "cut\n") ? trout_file_operation_move : trout_file_operation_copy;
  uri = uris;
  while (uri != 0 && *uri != 0) {
    file = g_file_new_for_uri(*uri);
    name = g_file_get_basename(file);
    if (name != 0) {
      destination = g_file_get_child(read_data->destination, name);
      pair = trout_file_transfer_pair_new(file, destination);
      g_ptr_array_add(operations, pair);
      g_object_unref(destination);
      g_free(name);
    }
    g_object_unref(file);
    uri += 1;
  }
  trout_transfer_request_enqueue(read_data->window, operations, operation);
  g_strfreev(uris);
  g_free(contents);
  trout_tab_clipboard_read_destroy(read_data);
}

void
trout_tab_delete_selection(trout_tab *tab)
{
  GtkBitset *selection;
  GtkBitsetIter iter;
  guint position;
  GFileInfo *file_info;
  GFile *file;
  GPtrArray *operations;
  gboolean has_selection;
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  selection = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  has_selection = gtk_bitset_iter_init_first(&iter, selection, &position);
  while (has_selection) {
    file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
    file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
    g_ptr_array_add(operations, trout_file_transfer_pair_new(file, file));
    g_object_unref(file_info);
    has_selection = gtk_bitset_iter_next(&iter, &position);
  }
  gtk_bitset_unref(selection);
  trout_transfer_request_enqueue(tab->window, operations, trout_file_operation_delete);
}

void
trout_tab_rename_dialog_destroy(gpointer user_data)
{
  trout_rename_dialog *rename_data;
  rename_data = user_data;
  g_object_unref(rename_data->source);
  g_object_unref(rename_data->owner_widget);
  g_free(rename_data);
}

void
trout_tab_rename_accept(GtkWidget *widget, gpointer user_data)
{
  trout_rename_dialog *rename_data;
  GtkWidget *dialog;
  GtkRoot *owner;
  GPtrArray *operations;
  trout_file_transfer_pair *pair;
  trout_window *window;
  GError *error;
  char *name;
  rename_data = user_data;
  dialog = GTK_WIDGET(gtk_widget_get_root(widget));
  name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(rename_data->entry)));
  if (name[0] != '\0' && strchr(name, '/') == 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
    owner = gtk_widget_get_root(rename_data->owner_widget);
    if (owner != 0) {
      window = g_object_get_data(G_OBJECT(owner), "trout_window");
      error = 0;
      pair = trout_file_transfer_pair_new_rename(rename_data->source, name, &error);
      if (pair == 0) {
        if (error != 0) {
          trout_tab_show_error(rename_data->owner_widget, error);
          g_error_free(error);
        }
      } else {
        if (g_file_equal(trout_file_transfer_pair_get_source(pair), trout_file_transfer_pair_get_destination(pair))) trout_file_transfer_pair_destroy(pair);
        else {
          operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
          g_ptr_array_add(operations, pair);
          trout_transfer_request_enqueue(window, operations, trout_file_operation_rename);
        }
        gtk_window_destroy(GTK_WINDOW(dialog));
      }
    }
  }
  g_free(name);
}

void
trout_tab_rename_cancel(GtkButton *button, gpointer user_data)
{
  gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
  (void)user_data;
}

void
trout_tab_create_dialog_destroy(gpointer user_data)
{
  trout_create_dialog *create_data;
  create_data = user_data;
  g_object_unref(create_data->directory);
  g_object_unref(create_data->root);
  g_free(create_data);
}

gboolean
trout_tab_create_name_is_valid(char *name)
{
  return name[0] != '\0' && strchr(name, '/') == 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

void
trout_tab_create_dialog_changed(GtkEditable *editable, gpointer user_data)
{
  trout_create_dialog *create_data;
  char *name;
  create_data = user_data;
  name = (char *)gtk_editable_get_text(editable);
  gtk_widget_set_sensitive(create_data->create_button, !create_data->in_progress && trout_tab_create_name_is_valid(name));
}

void
trout_tab_create_request_destroy(gpointer user_data)
{
  trout_create_request *request;
  request = user_data;
  g_object_unref(request->target);
  g_free(request);
}

void
trout_tab_create_dialog_accept(GtkWidget *widget, gpointer user_data)
{
  trout_create_dialog *create_data;
  trout_create_request *request;
  GTask *task;
  GFile *target;
  char *name;
  create_data = user_data;
  name = (char *)gtk_editable_get_text(GTK_EDITABLE(create_data->entry));
  if (create_data->in_progress || !trout_tab_create_name_is_valid(name)) return;
  target = g_file_get_child(create_data->directory, name);
  request = g_new(trout_create_request, 1);
  request->target = target;
  request->create_directory = create_data->create_directory;
  create_data->in_progress = TRUE;
  gtk_widget_set_sensitive(create_data->entry, FALSE);
  gtk_widget_set_sensitive(create_data->create_button, FALSE);
  gtk_widget_set_sensitive(create_data->cancel_button, FALSE);
  gtk_label_set_text(GTK_LABEL(create_data->message), "Creating…");
  task = g_task_new(G_OBJECT(gtk_widget_get_root(widget)), 0, trout_tab_create_complete, 0);
  g_task_set_task_data(task, request, trout_tab_create_request_destroy);
  g_task_run_in_thread(task, trout_tab_create_worker);
  g_object_unref(task);
}

void
trout_tab_create_dialog_cancel(GtkButton *button, gpointer user_data)
{
  gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
  (void)user_data;
}

void
trout_tab_create_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  trout_create_request *request;
  GFileOutputStream *stream;
  GError *error;
  gboolean succeeded;
  request = task_data;
  error = 0;
  if (request->create_directory) {
    succeeded = g_file_make_directory(request->target, cancellable, &error);
  } else {
    stream = g_file_create(request->target, G_FILE_CREATE_NONE, cancellable, &error);
    succeeded = stream != 0;
    if (stream != 0) {
      succeeded = g_output_stream_close(G_OUTPUT_STREAM(stream), cancellable, &error);
      g_object_unref(stream);
    }
  }
  if (succeeded) g_task_return_boolean(task, TRUE);
  else g_task_return_error(task, error);
  (void)source_object;
}

void
trout_tab_create_complete(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  trout_create_dialog *create_data;
  GError *error;
  gboolean succeeded;
  char *name;
  create_data = g_object_get_data(source_object, "trout_create_data");
  error = 0;
  succeeded = g_task_propagate_boolean(G_TASK(result), &error);
  if (succeeded) {
    if (gtk_widget_get_root(create_data->root) != 0) trout_tab_reload_directory(create_data->tab);
    if (gtk_widget_get_visible(GTK_WIDGET(source_object))) gtk_window_destroy(GTK_WINDOW(source_object));
  } else {
    create_data->in_progress = FALSE;
    gtk_widget_set_sensitive(create_data->entry, TRUE);
    gtk_widget_set_sensitive(create_data->cancel_button, TRUE);
    name = (char *)gtk_editable_get_text(GTK_EDITABLE(create_data->entry));
    gtk_widget_set_sensitive(create_data->create_button, trout_tab_create_name_is_valid(name));
    gtk_label_set_text(GTK_LABEL(create_data->message), error->message);
    g_error_free(error);
  }
  (void)user_data;
}

void
trout_tab_create_named_entry(trout_tab *tab, gboolean create_directory)
{
  trout_create_dialog *create_data;
  GtkRoot *parent;
  GtkWidget *dialog;
  GtkWidget *content;
  GtkWidget *entry;
  GtkWidget *message;
  GtkWidget *buttons;
  GtkWidget *cancel_button;
  GtkWidget *create_button;
  dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), create_directory ? "New Directory" : "New File");
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
  parent = gtk_widget_get_root(tab->root);
  if (parent != 0 && GTK_IS_WINDOW(parent)) {
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  }
  gtk_window_set_default_size(GTK_WINDOW(dialog), 360, -1);
  create_data = g_new0(trout_create_dialog, 1);
  create_data->tab = tab;
  create_data->directory = g_object_ref(tab->directory);
  create_data->root = g_object_ref(tab->root);
  create_data->create_directory = create_directory;
  g_object_set_data_full(G_OBJECT(dialog), "trout_create_data", create_data, trout_tab_create_dialog_destroy);
  content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);
  entry = gtk_entry_new();
  create_data->entry = entry;
  gtk_box_append(GTK_BOX(content), entry);
  message = gtk_label_new("");
  create_data->message = message;
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_box_append(GTK_BOX(content), message);
  buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  cancel_button = gtk_button_new_with_label("Cancel");
  create_data->cancel_button = cancel_button;
  create_button = gtk_button_new_with_label("Create");
  create_data->create_button = create_button;
  gtk_widget_set_sensitive(create_button, FALSE);
  g_signal_connect(cancel_button, "clicked", G_CALLBACK(trout_tab_create_dialog_cancel), create_data);
  g_signal_connect(create_button, "clicked", G_CALLBACK(trout_tab_create_dialog_accept), create_data);
  g_signal_connect(entry, "activate", G_CALLBACK(trout_tab_create_dialog_accept), create_data);
  g_signal_connect(entry, "changed", G_CALLBACK(trout_tab_create_dialog_changed), create_data);
  gtk_box_append(GTK_BOX(buttons), cancel_button);
  gtk_box_append(GTK_BOX(buttons), create_button);
  gtk_box_append(GTK_BOX(content), buttons);
  gtk_window_set_child(GTK_WINDOW(dialog), content);
  gtk_window_set_default_widget(GTK_WINDOW(dialog), create_button);
  gtk_window_present(GTK_WINDOW(dialog));
  gtk_widget_grab_focus(entry);
}

void
trout_window_action_new_directory(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  window = user_data;
  tab = trout_window_get_current_tab(window);
  if (tab != 0) trout_tab_create_named_entry(tab, TRUE);
  (void)action;
  (void)parameter;
}

void
trout_window_action_new_file(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  trout_window *window;
  trout_tab *tab;
  window = user_data;
  tab = trout_window_get_current_tab(window);
  if (tab != 0) trout_tab_create_named_entry(tab, FALSE);
  (void)action;
  (void)parameter;
}

void
trout_tab_rename_context_file(trout_tab *tab)
{
  GtkWidget *dialog;
  GtkWidget *content;
  GtkWidget *entry;
  GtkWidget *buttons;
  GtkWidget *cancel_button;
  GtkWidget *rename_button;
  GtkRoot *root;
  trout_rename_dialog *rename_data;
  char *name;
  dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Rename");
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  root = gtk_widget_get_root(tab->location_entry);
  if (root != 0) gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(root));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 360, -1);
  rename_data = g_new0(trout_rename_dialog, 1);
  rename_data->source = g_object_ref(tab->context_file);
  rename_data->owner_widget = g_object_ref(tab->root);
  g_object_set_data_full(G_OBJECT(dialog), "trout_rename_data", rename_data, trout_tab_rename_dialog_destroy);
  content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);
  entry = gtk_entry_new();
  rename_data->entry = entry;
  name = g_file_get_basename(tab->context_file);
  if (name != 0) {
    char *display_name = g_file_is_native(tab->context_file) ? g_filename_display_name(name) : g_utf8_make_valid(name, -1);
    gtk_editable_set_text(GTK_EDITABLE(entry), display_name);
    g_free(display_name);
    g_free(name);
  }
  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_box_append(GTK_BOX(content), entry);
  buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  cancel_button = gtk_button_new_with_label("Cancel");
  rename_button = gtk_button_new_with_label("Rename");
  g_signal_connect(cancel_button, "clicked", G_CALLBACK(trout_tab_rename_cancel), rename_data);
  g_signal_connect(rename_button, "clicked", G_CALLBACK(trout_tab_rename_accept), rename_data);
  g_signal_connect(entry, "activate", G_CALLBACK(trout_tab_rename_accept), rename_data);
  gtk_box_append(GTK_BOX(buttons), cancel_button);
  gtk_box_append(GTK_BOX(buttons), rename_button);
  gtk_box_append(GTK_BOX(content), buttons);
  gtk_window_set_child(GTK_WINDOW(dialog), content);
  gtk_window_set_default_widget(GTK_WINDOW(dialog), rename_button);
  gtk_window_present(GTK_WINDOW(dialog));
  gtk_widget_grab_focus(entry);
}

GtkWidget *
trout_tab_make_context_button(char *label, char *action_name, trout_tab *tab, GtkWidget *popover)
{
  GtkWidget *button;
  GtkWidget *button_label;
  button = gtk_button_new_with_label(label);
  gtk_widget_set_halign(button, GTK_ALIGN_FILL);
  button_label = gtk_button_get_child(GTK_BUTTON(button));
  gtk_widget_set_hexpand(button_label, TRUE);
  gtk_widget_set_halign(button_label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(button_label), 0.0f);
  g_object_set_data(G_OBJECT(button), "trout_context_action", action_name);
  g_object_set_data(G_OBJECT(button), "trout_context_popover", popover);
  g_signal_connect(button, "clicked", G_CALLBACK(trout_tab_context_action), tab);
  return button;
}

void
trout_tab_properties_frame_destroy(gpointer user_data)
{
  trout_properties_frame *frame;
  GError *error;
  frame = user_data;
  error = 0;
  g_file_enumerator_close(frame->enumerator, 0, &error);
  g_clear_error(&error);
  g_object_unref(frame->enumerator);
  g_object_unref(frame->directory);
  g_free(frame);
}

void
trout_tab_properties_add_size(trout_properties_result *result, goffset size)
{
  uint64_t value;
  if (size < 0) {
    result->incomplete = TRUE;
    return;
  }
  value = size;
  if (UINT64_MAX - result->size < value) {
    result->size = UINT64_MAX;
    result->incomplete = TRUE;
    return;
  }
  result->size += value;
}

void
trout_tab_properties_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  GFile *file;
  GFileInfo *info;
  GFileEnumerator *enumerator;
  GFile *child;
  GError *error;
  trout_properties_result *result;
  trout_properties_frame *frame;
  trout_properties_frame *child_frame;
  GPtrArray *frames;
  GFileType file_type;
  char *name;
  file = task_data;
  result = g_new0(trout_properties_result, 1);
  error = 0;
  info = g_file_query_info(file, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
  if (info == 0) {
    result->incomplete = TRUE;
    g_clear_error(&error);
    g_task_return_pointer(task, result, g_free);
    return;
  }
  file_type = g_file_info_get_file_type(info);
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) trout_tab_properties_add_size(result, g_file_info_get_size(info));
  else result->incomplete = TRUE;
  g_object_unref(info);
  frames = g_ptr_array_new_with_free_func(trout_tab_properties_frame_destroy);
  if (file_type == G_FILE_TYPE_DIRECTORY) {
    error = 0;
    enumerator = g_file_enumerate_children(file, "standard::name,standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
    if (enumerator == 0) {
      result->incomplete = TRUE;
      g_clear_error(&error);
    } else {
      frame = g_new(trout_properties_frame, 1);
      frame->directory = g_object_ref(file);
      frame->enumerator = enumerator;
      g_ptr_array_add(frames, frame);
    }
  }
  while (frames->len > 0 && (cancellable == 0 || !g_cancellable_is_cancelled(cancellable))) {
    frame = g_ptr_array_index(frames, frames->len - 1);
    error = 0;
    info = g_file_enumerator_next_file(frame->enumerator, cancellable, &error);
    if (error != 0) {
      result->incomplete = TRUE;
      g_clear_error(&error);
      g_ptr_array_remove_index(frames, frames->len - 1);
    } else if (info == 0) {
      g_ptr_array_remove_index(frames, frames->len - 1);
    } else {
      file_type = g_file_info_get_file_type(info);
      if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) trout_tab_properties_add_size(result, g_file_info_get_size(info));
      else result->incomplete = TRUE;
      if (file_type == G_FILE_TYPE_DIRECTORY) {
        name = (char *)g_file_info_get_name(info);
        if (name == 0) {
          result->incomplete = TRUE;
        } else {
          child = g_file_get_child(frame->directory, name);
          error = 0;
          enumerator = g_file_enumerate_children(child, "standard::name,standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
          if (enumerator == 0) {
            result->incomplete = TRUE;
            g_clear_error(&error);
          } else {
            child_frame = g_new(trout_properties_frame, 1);
            child_frame->directory = child;
            child_frame->enumerator = enumerator;
            g_ptr_array_add(frames, child_frame);
            child = 0;
          }
          if (child != 0) g_object_unref(child);
        }
      }
      g_object_unref(info);
    }
  }
  if (cancellable != 0 && g_cancellable_is_cancelled(cancellable)) result->incomplete = TRUE;
  g_ptr_array_unref(frames);
  g_task_return_pointer(task, result, g_free);
  (void)source_object;
}

void
trout_tab_properties_done(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  GtkWidget *dialog;
  GtkWidget *label;
  trout_properties_result *properties;
  GError *error;
  char *size_text;
  char *text;
  dialog = GTK_WIDGET(source_object);
  error = 0;
  properties = g_task_propagate_pointer(G_TASK(result), &error);
  if (error != 0) g_error_free(error);
  if (properties != 0 && gtk_widget_get_visible(dialog)) {
    label = g_object_get_data(G_OBJECT(dialog), "trout_properties_size_label");
    size_text = trout_format_size(properties->size, properties->incomplete);
    text = g_strdup_printf("Size: %s", size_text);
    gtk_label_set_text(GTK_LABEL(label), text);
    g_free(size_text);
    g_free(text);
  }
  g_free(properties);
  (void)user_data;
}

gboolean
trout_tab_properties_close_request(GtkWindow *window, gpointer user_data)
{
  GCancellable *cancellable;
  cancellable = g_object_get_data(G_OBJECT(window), "trout_properties_cancellable");
  if (cancellable != 0) g_cancellable_cancel(cancellable);
  (void)user_data;
  return FALSE;
}

void
trout_tab_properties_close(GtkButton *button, gpointer user_data)
{
  gtk_window_close(GTK_WINDOW(user_data));
  (void)button;
}

void
trout_tab_show_properties(trout_tab *tab)
{
  GtkWidget *dialog;
  GtkWidget *content;
  GtkWidget *label;
  GtkWidget *buttons;
  GtkWidget *close_button;
  GtkRoot *parent;
  GCancellable *cancellable;
  GTask *task;
  dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Properties");
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
  parent = gtk_widget_get_root(tab->root);
  if (parent != 0 && GTK_IS_WINDOW(parent)) {
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  }
  content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);
  label = gtk_label_new("Calculating…");
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  g_object_set_data(G_OBJECT(dialog), "trout_properties_size_label", label);
  gtk_box_append(GTK_BOX(content), label);
  buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  close_button = gtk_button_new_with_label("Close");
  g_signal_connect(close_button, "clicked", G_CALLBACK(trout_tab_properties_close), dialog);
  gtk_box_append(GTK_BOX(buttons), close_button);
  gtk_box_append(GTK_BOX(content), buttons);
  gtk_window_set_child(GTK_WINDOW(dialog), content);
  g_signal_connect(dialog, "close-request", G_CALLBACK(trout_tab_properties_close_request), 0);
  cancellable = g_cancellable_new();
  g_object_set_data_full(G_OBJECT(dialog), "trout_properties_cancellable", cancellable, g_object_unref);
  task = g_task_new(G_OBJECT(dialog), cancellable, trout_tab_properties_done, 0);
  g_task_set_task_data(task, g_object_ref(tab->context_file), g_object_unref);
  g_task_run_in_thread(task, trout_tab_properties_worker);
  g_object_unref(task);
  gtk_window_present(GTK_WINDOW(dialog));
}

void
trout_tab_append_context_menu(trout_tab *tab, GtkWidget *popover, GtkWidget *menu_items, gboolean has_context_entry)
{
  GtkBitset *selection;
  GtkWidget *button;
  GPtrArray *actions;
  trout_action *action;
  guint action_index;
  gboolean has_selection;
  gboolean has_custom_actions;
  gboolean action_matches;
  selection = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  has_selection = gtk_bitset_get_size(selection) > 0;
  gtk_bitset_unref(selection);
  button = trout_tab_make_context_button("Copy", "copy", tab, popover);
  gtk_widget_set_sensitive(button, has_selection);
  gtk_box_append(GTK_BOX(menu_items), button);
  button = trout_tab_make_context_button("Cut", "cut", tab, popover);
  gtk_widget_set_sensitive(button, has_selection);
  gtk_box_append(GTK_BOX(menu_items), button);
  if (tab->context_file_is_directory) gtk_box_append(GTK_BOX(menu_items), trout_tab_make_context_button("Paste", "paste", tab, popover));
  if (has_context_entry) {
    gtk_box_append(GTK_BOX(menu_items), trout_tab_make_context_button("Rename", "rename", tab, popover));
    button = trout_tab_make_context_button("Delete", "delete", tab, popover);
    gtk_widget_set_sensitive(button, FALSE);
    gtk_box_append(GTK_BOX(menu_items), button);
  }
  gtk_box_append(GTK_BOX(menu_items), trout_tab_make_context_button("Properties", "properties", tab, popover));
  actions = g_object_get_data(G_OBJECT(tab->window->application), "trout_actions");
  action_index = 0;
  has_custom_actions = FALSE;
  while (action_index < actions->len) {
    action = g_ptr_array_index(actions, action_index);
    if ((action->appearance & trout_action_appearance_context) != 0) {
      action_matches = has_context_entry ? trout_action_matches_selection(action, tab) : trout_action_matches_directory(action, tab->context_file);
      if (action_matches) {
        if (!has_custom_actions) {
          gtk_box_append(GTK_BOX(menu_items), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
          has_custom_actions = TRUE;
        }
        button = trout_action_make_button(action, tab->window, popover);
        g_object_set_data(G_OBJECT(button), "trout_custom_tab", tab);
        if (!has_context_entry) g_object_set_data(G_OBJECT(button), "trout_context_directory", tab->context_file);
        gtk_box_append(GTK_BOX(menu_items), button);
      }
    }
    action_index += 1;
  }
}

void
trout_tab_context_action(GtkButton *button, gpointer user_data)
{
  trout_tab *tab;
  GtkWidget *popover;
  char *action_name;
  tab = user_data;
  action_name = g_object_get_data(G_OBJECT(button), "trout_context_action");
  popover = g_object_get_data(G_OBJECT(button), "trout_context_popover");
  if (g_strcmp0(action_name, "copy") == 0) trout_tab_copy_selection(tab, FALSE);
  else if (g_strcmp0(action_name, "cut") == 0) trout_tab_copy_selection(tab, TRUE);
  else if (g_strcmp0(action_name, "paste") == 0) trout_tab_paste(tab);
  else if (g_strcmp0(action_name, "rename") == 0) trout_tab_rename_context_file(tab);
  else if (g_strcmp0(action_name, "delete") == 0) trout_tab_delete_selection(tab);
  else if (g_strcmp0(action_name, "properties") == 0) trout_tab_show_properties(tab);
  gtk_popover_popdown(GTK_POPOVER(popover));
}

void
trout_tab_context_closed(GtkPopover *popover, gpointer user_data)
{
  trout_tab *tab;
  tab = user_data;
  if (tab->context_popover == GTK_WIDGET(popover)) tab->context_popover = 0;
  if (gtk_widget_get_parent(GTK_WIDGET(popover)) != 0) gtk_widget_unparent(GTK_WIDGET(popover));
}

void
trout_tab_on_entry_context_pressed(GtkGestureClick *gesture, gint press_count, double x, double y, gpointer user_data)
{
  trout_tab *tab;
  GtkWidget *row;
  GtkListItem *list_item;
  guint position;
  GFileInfo *file_info;
  GFile *file;
  GtkWidget *popover;
  GtkWidget *menu_items;
  GFileType file_type;
  GdkRectangle point;
  tab = user_data;
  row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  list_item = g_object_get_data(G_OBJECT(row), "trout_list_item");
  position = gtk_list_item_get_position(list_item);
  if (position == GTK_INVALID_LIST_POSITION) return;
  if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(tab->selection), position)) {
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(tab->selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(tab->selection), position, FALSE);
  }
  file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
  file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
  file_type = g_file_info_get_file_type(file_info);
  if (tab->context_file != 0) g_object_unref(tab->context_file);
  tab->context_file = g_object_ref(file);
  tab->context_file_is_directory = file_type == G_FILE_TYPE_DIRECTORY;
  g_object_unref(file_info);
  if (tab->context_popover != 0) {
    popover = tab->context_popover;
    tab->context_popover = 0;
    if (gtk_widget_get_parent(popover) != 0) gtk_widget_unparent(popover);
  }
  popover = gtk_popover_new();
  tab->context_popover = popover;
  menu_items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  trout_tab_append_context_menu(tab, popover, menu_items, TRUE);
  gtk_popover_set_child(GTK_POPOVER(popover), menu_items);
  g_signal_connect(popover, "closed", G_CALLBACK(trout_tab_context_closed), tab);
  point.x = x;
  point.y = y;
  point.width = 1;
  point.height = 1;
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &point);
  gtk_widget_set_parent(popover, row);
  gtk_popover_popup(GTK_POPOVER(popover));
  (void)press_count;
}

void
trout_tab_open_file(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GError *error;
  (void)user_data;
  error = 0;
  g_app_info_launch_default_for_uri_finish(result, &error);
  if (error != 0) {
    g_warning("Could not open file: %s", error->message);
    g_error_free(error);
  }
  (void)source;
}

void
trout_tab_on_activate(GtkGridView *grid, guint position, gpointer user_data)
{
  trout_tab *tab;
  GFileInfo *file_info;
  GFile *file;
  GFileType file_type;
  char *uri;
  tab = user_data;
  file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
  file_type = g_file_info_get_file_type(file_info);
  file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
  if (file_type == G_FILE_TYPE_DIRECTORY) {
    trout_tab_set_directory(tab, file);
  } else {
    uri = g_file_get_uri(file);
    g_app_info_launch_default_for_uri_async(uri, 0, 0, trout_tab_open_file, tab);
    g_free(uri);
  }
  g_object_unref(file_info);
  (void)grid;
}

