void
trout_tab_delete_job_destroy(trout_delete_job *job)
{
  g_object_unref(job->status_label);
  g_object_unref(job->source);
  if (job->enumerator != 0) g_object_unref(job->enumerator);
  g_free(job);
}

void
trout_tab_start_delete_operation(GtkWidget *status_label, GFile *source, trout_delete_job *parent)
{
  trout_delete_job *job;
  job = g_new0(trout_delete_job, 1);
  job->status_label = g_object_ref(status_label);
  job->source = g_object_ref(source);
  job->parent = parent;
  if (parent != 0) parent->pending_children += 1;
  g_file_delete_async(source, G_PRIORITY_DEFAULT, 0, trout_tab_delete_operation_complete, job);
}

void
trout_tab_delete_job_finish(trout_delete_job *job, gboolean failed)
{
  trout_delete_job *parent;
  parent = job->parent;
  trout_tab_delete_job_destroy(job);
  if (parent != 0) {
    if (failed) parent->delete_failed = TRUE;
    parent->pending_children -= 1;
    if (parent->delete_enumeration_done && parent->pending_children == 0) {
      if (parent->delete_failed) trout_tab_delete_job_finish(parent, TRUE);
      else g_file_delete_async(parent->source, G_PRIORITY_DEFAULT, 0, trout_tab_delete_operation_complete, parent);
    }
  }
}

void
trout_tab_delete_operation_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
  trout_delete_job *job;
  GError *error;
  gboolean succeeded;
  job = user_data;
  error = 0;
  succeeded = g_file_delete_finish(G_FILE(source), result, &error);
  if (!succeeded && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE))) {
    g_error_free(error);
    if (job->enumerator != 0) {
      g_object_unref(job->enumerator);
      job->enumerator = 0;
    }
    job->delete_enumeration_done = FALSE;
    g_file_enumerate_children_async(job->source, "standard::name", G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, 0, trout_tab_delete_directory_enumerated, job);
    return;
  }
  if (!succeeded && error != 0) {
    trout_tab_show_error(job->status_label, error);
    g_error_free(error);
  }
  trout_tab_delete_job_finish(job, !succeeded);
}

void
trout_tab_delete_directory_enumerated(GObject *source, GAsyncResult *result, gpointer user_data)
{
  trout_delete_job *job;
  GError *error;
  job = user_data;
  error = 0;
  job->enumerator = g_file_enumerate_children_finish(G_FILE(source), result, &error);
  if (job->enumerator == 0) {
    trout_tab_show_error(job->status_label, error);
    g_error_free(error);
    job->delete_enumeration_done = TRUE;
    job->delete_failed = TRUE;
    if (job->pending_children == 0) trout_tab_delete_job_finish(job, TRUE);
    return;
  }
  g_file_enumerator_next_files_async(job->enumerator, 64, G_PRIORITY_DEFAULT, 0, trout_tab_delete_directory_next_files, job);
}

void
trout_tab_delete_directory_next_files(GObject *source, GAsyncResult *result, gpointer user_data)
{
  trout_delete_job *job;
  GList *file_infos;
  GList *file_info_node;
  GFileInfo *file_info;
  GFile *child;
  char *name;
  GError *error;
  job = user_data;
  error = 0;
  file_infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), result, &error);
  if (file_infos == 0) {
    if (error != 0) {
      trout_tab_show_error(job->status_label, error);
      g_error_free(error);
      job->delete_enumeration_done = TRUE;
      job->delete_failed = TRUE;
      if (job->pending_children == 0) trout_tab_delete_job_finish(job, TRUE);
      return;
    }
    job->delete_enumeration_done = TRUE;
    g_object_unref(job->enumerator);
    job->enumerator = 0;
    if (job->pending_children == 0) {
      if (job->delete_failed) trout_tab_delete_job_finish(job, TRUE);
      else g_file_delete_async(job->source, G_PRIORITY_DEFAULT, 0, trout_tab_delete_operation_complete, job);
    }
    return;
  }
  file_info_node = file_infos;
  while (file_info_node != 0) {
    file_info = file_info_node->data;
    name = g_strdup(g_file_info_get_name(file_info));
    child = g_file_get_child(job->source, name);
    trout_tab_start_delete_operation(job->status_label, child, job);
    g_object_unref(child);
    g_free(name);
    g_object_unref(file_info);
    file_info_node = file_info_node->next;
  }
  g_list_free(file_infos);
  g_file_enumerator_next_files_async(job->enumerator, 64, G_PRIORITY_DEFAULT, 0, trout_tab_delete_directory_next_files, job);
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
      if (!g_file_equal(file, target)) g_ptr_array_add(operations, trout_file_transfer_pair_new(file, target));
      g_object_unref(target);
      g_free(name);
    }
    file_node = file_node->next;
  }
  trout_transfer_request_enqueue(tab->window, tab->status_label, operations, operation);
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
  g_object_unref(read_data->status_label);
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
  read_data->status_label = g_object_ref(tab->status_label);
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
      trout_tab_show_error(read_data->status_label, error);
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
      trout_tab_show_error(read_data->status_label, error);
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
      if (!g_file_equal(file, destination)) {
        pair = trout_file_transfer_pair_new(file, destination);
        g_ptr_array_add(operations, pair);
      }
      g_object_unref(destination);
      g_free(name);
    }
    g_object_unref(file);
    uri += 1;
  }
  trout_transfer_request_enqueue(read_data->window, read_data->status_label, operations, operation);
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
  gboolean has_selection;
  selection = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(tab->selection));
  has_selection = gtk_bitset_iter_init_first(&iter, selection, &position);
  while (has_selection) {
    file_info = g_list_model_get_item(G_LIST_MODEL(tab->sorted_list), position);
    file = G_FILE(g_file_info_get_attribute_object(file_info, "standard::file"));
    trout_tab_start_delete_operation(tab->status_label, file, 0);
    g_object_unref(file_info);
    has_selection = gtk_bitset_iter_next(&iter, &position);
  }
  gtk_bitset_unref(selection);
}

void
trout_tab_rename_dialog_destroy(gpointer user_data)
{
  trout_rename_dialog *rename_data;
  rename_data = user_data;
  g_object_unref(rename_data->source);
  g_object_unref(rename_data->status_label);
  g_free(rename_data);
}

void
trout_tab_rename_accept(GtkWidget *widget, gpointer user_data)
{
  trout_rename_dialog *rename_data;
  GtkWidget *dialog;
  GFile *parent;
  GFile *destination;
  GPtrArray *operations;
  trout_file_transfer_pair *pair;
  trout_window *window;
  char *name;
  rename_data = user_data;
  dialog = GTK_WIDGET(gtk_widget_get_root(widget));
  name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(rename_data->entry)));
  if (name[0] != '\0' && strchr(name, '/') == 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
    parent = g_file_get_parent(rename_data->source);
    destination = g_file_get_child(parent, name);
    if (!g_file_equal(rename_data->source, destination)) {
      operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
      pair = trout_file_transfer_pair_new(rename_data->source, destination);
      g_ptr_array_add(operations, pair);
      window = g_object_get_data(G_OBJECT(gtk_widget_get_root(rename_data->status_label)), "trout_window");
      trout_transfer_request_enqueue(window, rename_data->status_label, operations, trout_file_operation_move);
    }
    g_object_unref(destination);
    g_object_unref(parent);
    gtk_window_destroy(GTK_WINDOW(dialog));
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
  rename_data->status_label = g_object_ref(tab->status_label);
  g_object_set_data_full(G_OBJECT(dialog), "trout_rename_data", rename_data, trout_tab_rename_dialog_destroy);
  content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);
  entry = gtk_entry_new();
  rename_data->entry = entry;
  name = g_file_get_basename(tab->context_file);
  gtk_editable_set_text(GTK_EDITABLE(entry), name);
  g_free(name);
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
  double value;
  char number[64];
  char *text;
  dialog = GTK_WIDGET(source_object);
  error = 0;
  properties = g_task_propagate_pointer(G_TASK(result), &error);
  if (error != 0) g_error_free(error);
  if (properties != 0 && gtk_widget_get_visible(dialog)) {
    label = g_object_get_data(G_OBJECT(dialog), "trout_properties_size_label");
    if (properties->size < UINT64_C(10000000)) {
      value = (double)properties->size / 1000.0;
      g_ascii_formatd(number, sizeof(number), "%.2f", value);
      text = g_strdup_printf("%s%s kB", properties->incomplete ? ">= " : "", number);
    } else {
      value = (double)properties->size / 1000000000.0;
      g_ascii_formatd(number, sizeof(number), "%.2f", value);
      text = g_strdup_printf("%s%s GB", properties->incomplete ? ">= " : "", number);
    }
    gtk_label_set_text(GTK_LABEL(label), text);
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
  task = g_task_new(dialog, cancellable, trout_tab_properties_done, 0);
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

