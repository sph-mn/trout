static const char *
trout_transfer_operation_action(trout_file_operation operation)
{
  if (operation == trout_file_operation_rename) return "Rename";
  if (operation == trout_file_operation_delete) return "Delete";
  return operation == trout_file_operation_copy ? "Copy" : "Move";
}

static const char *
trout_transfer_operation_activity(trout_file_operation operation)
{
  if (operation == trout_file_operation_rename) return "Renaming";
  if (operation == trout_file_operation_delete) return "Deleting";
  return operation == trout_file_operation_copy ? "Copying" : "Moving";
}

/* GFile basenames can be native filename bytes, not GTK-compatible UTF-8. */
static char *
trout_transfer_display_basename(GFile *file)
{
  char *name;
  char *display;
  name = g_file_get_basename(file);
  if (name == 0) return g_file_get_parse_name(file);
  display = g_file_is_native(file) ? g_filename_display_name(name) : g_utf8_make_valid(name, -1);
  g_free(name);
  return display;
}

void
trout_transfer_request_destroy(trout_transfer_request *request)
{
  if (g_atomic_int_dec_and_test(&request->references)) {
    g_object_unref(request->window_widget);
    g_ptr_array_unref(request->operations);
    g_clear_object(&request->collision.source);
    g_clear_object(&request->collision.destination);
    g_clear_object(&request->collision.source_root);
    g_clear_object(&request->collision.destination_root);
    g_cond_clear(&request->collision.condition);
    g_mutex_clear(&request->collision.mutex);
    g_free(request->current_name);
    g_free(request->summary);
    g_object_unref(request->cancellable);
    g_cond_clear(&request->pause_condition);
    g_mutex_clear(&request->pause_mutex);
    g_free(request);
  }
}

trout_transfer_request *
trout_transfer_request_ref(trout_transfer_request *request)
{
  g_atomic_int_inc(&request->references);
  return request;
}

void
trout_transfer_request_release_pause(trout_transfer_request *request)
{
  g_mutex_lock(&request->pause_mutex);
  request->paused = FALSE;
  g_cond_broadcast(&request->pause_condition);
  g_mutex_unlock(&request->pause_mutex);
}

gboolean
trout_transfer_request_wait(gpointer user_data)
{
  trout_transfer_request *request;
  request = user_data;
  g_mutex_lock(&request->pause_mutex);
  while (request->paused && !g_cancellable_is_cancelled(request->cancellable)) g_cond_wait_until(&request->pause_condition, &request->pause_mutex, g_get_monotonic_time() + 100000);
  g_mutex_unlock(&request->pause_mutex);
  return !g_cancellable_is_cancelled(request->cancellable);
}

void
trout_transfer_request_progress(GFile *source, goffset current_bytes, goffset total_bytes, gpointer user_data)
{
  trout_transfer_request *request;
  trout_file_progress_update *update;
  GSource *idle_source;
  request = user_data;
  if (g_cancellable_is_cancelled(request->cancellable)) return;
  update = g_new0(trout_file_progress_update, 1);
  update->request = trout_transfer_request_ref(request);
  update->name = trout_transfer_display_basename(source);
  update->current_bytes = current_bytes;
  update->total_bytes = total_bytes;
  idle_source = g_idle_source_new();
  g_source_set_callback(idle_source, trout_file_operation_progress_update, update, trout_file_operation_progress_update_destroy);
  g_source_attach(idle_source, g_main_context_default());
  g_source_unref(idle_source);
}

trout_file_collision_choice
trout_transfer_request_collision(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, GFileType source_type, GFileType destination_type, goffset source_size, goffset destination_size, gpointer user_data)
{
  trout_transfer_request *request;
  GSource *idle_source;
  trout_file_collision_choice choice;
  request = user_data;
  g_mutex_lock(&request->collision.mutex);
  if (g_cancellable_is_cancelled(request->cancellable)) {
    g_mutex_unlock(&request->collision.mutex);
    return trout_file_collision_cancel;
  }
  request->collision.source = g_object_ref(source);
  request->collision.destination = g_object_ref(destination);
  request->collision.source_root = g_object_ref(source_root);
  request->collision.destination_root = g_object_ref(destination_root);
  request->collision.source_type = source_type;
  request->collision.destination_type = destination_type;
  request->collision.source_size = source_size;
  request->collision.destination_size = destination_size;
  request->collision.active = TRUE;
  request->collision.choice_ready = FALSE;
  request->collision.choice = trout_file_collision_cancel;
  g_mutex_unlock(&request->collision.mutex);
  idle_source = g_idle_source_new();
  g_source_set_callback(idle_source, trout_transfer_request_show_collision, trout_transfer_request_ref(request), trout_transfer_request_unref);
  g_source_attach(idle_source, g_main_context_default());
  g_source_unref(idle_source);
  g_mutex_lock(&request->collision.mutex);
  while (!request->collision.choice_ready && !g_cancellable_is_cancelled(request->cancellable)) g_cond_wait_until(&request->collision.condition, &request->collision.mutex, g_get_monotonic_time() + 100000);
  choice = request->collision.choice_ready ? request->collision.choice : trout_file_collision_cancel;
  request->collision.choice_ready = FALSE;
  request->collision.active = FALSE;
  g_clear_object(&request->collision.source);
  g_clear_object(&request->collision.destination);
  g_clear_object(&request->collision.source_root);
  g_clear_object(&request->collision.destination_root);
  g_mutex_unlock(&request->collision.mutex);
  return choice;
}

static char *
trout_transfer_request_format_size(goffset bytes)
{
  char formatted_size[G_ASCII_DTOSTR_BUF_SIZE];
  char *end;
  if (bytes > 0 && bytes < 5000) return g_strdup("<0.01 MB");
  g_ascii_formatd(formatted_size, sizeof(formatted_size), "%.2f", (double)bytes / 1000000.0);
  end = formatted_size + strlen(formatted_size);
  while (end > formatted_size && end[-1] == '0') {
    end -= 1;
    *end = '\0';
  }
  if (end > formatted_size && end[-1] == '.') *(end - 1) = '\0';
  return g_strdup_printf("%s MB", formatted_size);
}

gboolean
trout_transfer_request_show_collision(gpointer user_data)
{
  trout_transfer_request *request;
  GtkWidget *dialog;
  GtkWidget *content;
  GtkWidget *label;
  GtkWidget *paths;
  GtkWidget *buttons;
  GtkWidget *skip_button;
  GtkWidget *replace_button;
  GtkWidget *merge_button;
  GtkWidget *cancel_button;
  GFile *source;
  GFile *destination;
  GFile *source_root;
  GFile *destination_root;
  GFileType source_type;
  GFileType destination_type;
  goffset source_size;
  goffset destination_size;
  char *destination_name;
  char *source_path;
  char *destination_path;
  char *source_details;
  char *destination_details;
  char *size_text;
  char *path_text;
  char *message;
  request = user_data;
  g_mutex_lock(&request->collision.mutex);
  if (!request->collision.active || g_cancellable_is_cancelled(request->cancellable)) {
    g_mutex_unlock(&request->collision.mutex);
    return FALSE;
  }
  source = g_object_ref(request->collision.source);
  destination = g_object_ref(request->collision.destination);
  source_root = g_object_ref(request->collision.source_root);
  destination_root = g_object_ref(request->collision.destination_root);
  source_type = request->collision.source_type;
  destination_type = request->collision.destination_type;
  source_size = request->collision.source_size;
  destination_size = request->collision.destination_size;
  g_mutex_unlock(&request->collision.mutex);
  source_path = g_file_get_relative_path(source_root, source);
  if (source_path == 0 || source_path[0] == '\0') {
    g_free(source_path);
    source_path = trout_transfer_display_basename(source);
    if (source_path == 0) source_path = g_file_get_parse_name(source);
  }
  destination_path = g_file_get_relative_path(destination_root, destination);
  if (destination_path == 0 || destination_path[0] == '\0') {
    g_free(destination_path);
    destination_path = trout_transfer_display_basename(destination);
    if (destination_path == 0) destination_path = g_file_get_parse_name(destination);
  }
  if (!g_utf8_validate(source_path, -1, 0)) {
    char *display = g_file_is_native(source) ? g_filename_display_name(source_path) : g_utf8_make_valid(source_path, -1);
    g_free(source_path);
    source_path = display;
  }
  if (!g_utf8_validate(destination_path, -1, 0)) {
    char *display = g_file_is_native(destination) ? g_filename_display_name(destination_path) : g_utf8_make_valid(destination_path, -1);
    g_free(destination_path);
    destination_path = display;
  }
  source_details = g_strdup(source_path);
  destination_details = g_strdup(destination_path);
  if (source_type == G_FILE_TYPE_REGULAR) {
    size_text = trout_transfer_request_format_size(source_size);
    g_free(source_details);
    source_details = g_strdup_printf("%s (%s)", source_path, size_text);
    g_free(size_text);
  }
  if (destination_type == G_FILE_TYPE_REGULAR) {
    size_text = trout_transfer_request_format_size(destination_size);
    g_free(destination_details);
    destination_details = g_strdup_printf("%s (%s)", destination_path, size_text);
    g_free(size_text);
  }
  destination_name = trout_transfer_display_basename(destination);
  if (trout_file_transfer_should_merge(source_type, destination_type)) message = g_strdup_printf("A directory named “%s” already exists. Merge combines their contents and asks about conflicting entries. Choose Merge, Skip or Cancel.", destination_name);
  else if (source_type == G_FILE_TYPE_DIRECTORY || destination_type == G_FILE_TYPE_DIRECTORY) message = g_strdup_printf("An item named “%s” has a different type. Trout will not replace a directory with a non-directory or vice versa. Choose Skip or Cancel.", destination_name);
  else message = g_strdup_printf("An item named “%s” already exists in the destination.", destination_name);
  dialog = gtk_window_new();
  request->collision.dialog = dialog;
  g_object_set_data(G_OBJECT(dialog), "trout_transfer_request", request);
  gtk_window_set_title(GTK_WINDOW(dialog), "Destination Already Exists");
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(request->window->window));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 460, -1);
  content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);
  label = gtk_label_new(message);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  path_text = g_strdup_printf("From: %s\nTo: %s", source_details, destination_details);
  paths = gtk_label_new(path_text);
  gtk_label_set_xalign(GTK_LABEL(paths), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(paths), TRUE);
  gtk_label_set_selectable(GTK_LABEL(paths), TRUE);
  gtk_box_append(GTK_BOX(content), label);
  gtk_box_append(GTK_BOX(content), paths);
  buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  skip_button = gtk_button_new_with_label("Skip");
  cancel_button = gtk_button_new_with_label(request->operation == trout_file_operation_copy ? "Cancel Copy" : "Cancel Move");
  g_signal_connect(skip_button, "clicked", G_CALLBACK(trout_transfer_request_resolve_collision), GINT_TO_POINTER(trout_file_collision_skip));
  g_signal_connect(cancel_button, "clicked", G_CALLBACK(trout_transfer_request_resolve_collision), GINT_TO_POINTER(trout_file_collision_cancel));
  gtk_box_append(GTK_BOX(buttons), skip_button);
  if (trout_file_transfer_should_merge(source_type, destination_type)) {
    merge_button = gtk_button_new_with_label("Merge");
    g_signal_connect(merge_button, "clicked", G_CALLBACK(trout_transfer_request_resolve_collision), GINT_TO_POINTER(trout_file_collision_merge));
    gtk_box_append(GTK_BOX(buttons), merge_button);
  } else if (source_type != G_FILE_TYPE_DIRECTORY && destination_type != G_FILE_TYPE_DIRECTORY) {
    replace_button = gtk_button_new_with_label("Replace");
    g_signal_connect(replace_button, "clicked", G_CALLBACK(trout_transfer_request_resolve_collision), GINT_TO_POINTER(trout_file_collision_replace));
    gtk_box_append(GTK_BOX(buttons), replace_button);
  }
  gtk_box_append(GTK_BOX(buttons), cancel_button);
  gtk_box_append(GTK_BOX(content), buttons);
  gtk_window_set_child(GTK_WINDOW(dialog), content);
  gtk_window_set_default_widget(GTK_WINDOW(dialog), skip_button);
  gtk_widget_set_sensitive(request->window->transfer_pause_button, FALSE);
  trout_transfer_request_update_label(request);
  gtk_window_present(GTK_WINDOW(dialog));
  g_object_unref(source);
  g_object_unref(destination);
  g_object_unref(source_root);
  g_object_unref(destination_root);
  g_free(destination_name);
  g_free(source_path);
  g_free(destination_path);
  g_free(source_details);
  g_free(destination_details);
  g_free(path_text);
  g_free(message);
  return FALSE;
}

void
trout_transfer_request_resolve_collision(GtkButton *button, gpointer user_data)
{
  GtkWidget *dialog;
  trout_transfer_request *request;
  trout_file_collision_choice choice;
  dialog = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(button)));
  request = g_object_get_data(G_OBJECT(dialog), "trout_transfer_request");
  choice = GPOINTER_TO_UINT(user_data);
  trout_transfer_request_ref(request);
  request->collision.dialog = 0;
  gtk_window_destroy(GTK_WINDOW(dialog));
  if (choice == trout_file_collision_cancel) trout_transfer_request_cancel(request);
  else {
    g_mutex_lock(&request->collision.mutex);
    request->collision.choice = choice;
    request->collision.choice_ready = TRUE;
    request->collision.active = FALSE;
    g_cond_broadcast(&request->collision.condition);
    g_mutex_unlock(&request->collision.mutex);
    if (request->window->active_transfer == request) {
      gtk_widget_set_sensitive(request->window->transfer_pause_button, TRUE);
      gtk_button_set_label(GTK_BUTTON(request->window->transfer_pause_button), request->paused ? "Resume" : "Pause");
      trout_transfer_request_update_label(request);
    }
  }
  trout_transfer_request_destroy(request);
}

void
trout_transfer_request_update_label(trout_transfer_request *request)
{
  guint queued_count;
  gboolean paused;
  gboolean collision_active;
  char *text;
  queued_count = g_queue_get_length(&request->window->transfer_queue);
  g_mutex_lock(&request->pause_mutex);
  paused = request->paused;
  g_mutex_unlock(&request->pause_mutex);
  g_mutex_lock(&request->collision.mutex);
  collision_active = request->collision.active;
  g_mutex_unlock(&request->collision.mutex);
  if (collision_active) {
    if (queued_count > 0) text = g_strdup_printf("Waiting: %s %s · %u queued", trout_transfer_operation_activity(request->operation), request->current_name != 0 ? request->current_name : "files", queued_count);
    else text = g_strdup_printf("Waiting: %s %s", trout_transfer_operation_activity(request->operation), request->current_name != 0 ? request->current_name : "files");
  } else if (paused) {
    if (queued_count > 0) text = g_strdup_printf("Paused: %s %s · %u queued", trout_transfer_operation_activity(request->operation), request->current_name != 0 ? request->current_name : "files", queued_count);
    else text = g_strdup_printf("Paused: %s %s", trout_transfer_operation_activity(request->operation), request->current_name != 0 ? request->current_name : "files");
  } else if (queued_count > 0) text = g_strdup_printf("%s %s · %u queued", trout_transfer_operation_activity(request->operation), request->current_name != 0 ? request->current_name : "files", queued_count);
  else text = g_strdup_printf("%s %s", trout_transfer_operation_activity(request->operation), request->current_name != 0 ? request->current_name : "files");
  gtk_label_set_text(GTK_LABEL(request->window->transfer_label), text);
  g_free(text);
}

char *
trout_transfer_request_summary(trout_transfer_request *request)
{
  trout_file_transfer_pair *pair;
  GFile *source;
  GFile *destination;
  GFile *destination_directory;
  char *source_name;
  char *destination_name;
  char *summary;
  pair = g_ptr_array_index(request->operations, 0);
  source = trout_file_transfer_pair_get_source(pair);
  destination = trout_file_transfer_pair_get_destination(pair);
  source_name = trout_transfer_display_basename(source);
  if (source_name == 0) source_name = g_file_get_parse_name(source);
  if (request->operation == trout_file_operation_delete) {
    if (request->operations->len == 1) summary = g_strdup_printf("Delete %s", source_name);
    else summary = g_strdup_printf("Delete %u items", request->operations->len);
    g_free(source_name);
    return summary;
  }
  if (request->operations->len == 1) {
    destination_name = g_file_get_parse_name(destination);
    summary = g_strdup_printf("%s %s → %s", trout_transfer_operation_action(request->operation),
        source_name, destination_name);
  } else {
    destination_directory = g_file_get_parent(destination);
    destination_name = g_file_get_parse_name(destination_directory != 0 ? destination_directory : destination);
    summary = g_strdup_printf("%s %u items → %s", trout_transfer_operation_action(request->operation),
        request->operations->len, destination_name);
    g_clear_object(&destination_directory);
  }
  g_free(source_name);
  g_free(destination_name);
  return summary;
}

void
trout_transfer_request_add_queue_row(trout_transfer_request *request)
{
  GtkWidget *row;
  GtkWidget *label;
  GtkWidget *remove_button;
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  label = gtk_label_new(request->summary);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_hexpand(label, TRUE);
  remove_button = gtk_button_new_with_label("Remove");
  g_signal_connect(remove_button, "clicked", G_CALLBACK(trout_transfer_request_remove_queued), request);
  gtk_box_append(GTK_BOX(row), label);
  gtk_box_append(GTK_BOX(row), remove_button);
  gtk_box_append(GTK_BOX(request->window->transfer_queue_list), row);
  request->queue_row = row;
}

void
trout_transfer_request_update_queue(trout_window *window)
{
  gtk_widget_set_visible(window->transfer_queue_scroller, !g_queue_is_empty(&window->transfer_queue));
}

static void
trout_transfer_request_remove_queue_row(trout_transfer_request *request)
{
  if (request->queue_row != 0) {
    gtk_box_remove(GTK_BOX(request->window->transfer_queue_list), request->queue_row);
    request->queue_row = 0;
  }
}

static void
trout_transfer_queue_close_dialog(trout_window *window)
{
  gtk_window_destroy(GTK_WINDOW(window->transfer_dialog));
  window->transfer_dialog = 0;
  window->transfer_label = 0;
  window->transfer_progress = 0;
  window->transfer_error_label = 0;
  window->transfer_queue_scroller = 0;
  window->transfer_queue_list = 0;
  window->transfer_pause_button = 0;
  window->transfer_cancel_button = 0;
  window->transfer_had_failure = FALSE;
}

static gboolean
trout_transfer_queue_has_notice(trout_window *window)
{
  return window->transfer_had_failure || (window->transfer_dialog != 0 &&
      g_object_get_data(G_OBJECT(window->transfer_dialog), "trout_transfer_incomplete") != 0);
}

static void
trout_transfer_queue_add_notice(trout_transfer_request *request, const char *message)
{
  GtkLabel *label;
  const char *previous;
  char *text;
  label = GTK_LABEL(request->window->transfer_error_label);
  previous = gtk_label_get_text(label);
  if (previous[0] != '\0') text = g_strdup_printf("%s\n%s: %s", previous, request->summary, message);
  else text = g_strdup_printf("%s: %s", request->summary, message);
  gtk_label_set_text(label, text);
  gtk_widget_set_visible(GTK_WIDGET(label), TRUE);
  g_free(text);
}

static void
trout_transfer_queue_show_finished_failure(trout_window *window)
{
  gtk_label_set_text(GTK_LABEL(window->transfer_label), window->transfer_had_failure ? "Operation failed" : "Operations finished with skips or cancellations");
  if (window->transfer_had_failure) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(window->transfer_progress), 0.0);
  gtk_widget_set_sensitive(window->transfer_pause_button, FALSE);
  gtk_widget_set_sensitive(window->transfer_cancel_button, TRUE);
  gtk_button_set_label(GTK_BUTTON(window->transfer_cancel_button), "Close");
}

void
trout_transfer_request_remove_queued(GtkButton *button, gpointer user_data)
{
  trout_transfer_request *request;
  trout_window *window;
  request = trout_transfer_request_ref(user_data);
  window = request->window;
  g_queue_remove(&window->transfer_queue, request);
  trout_transfer_request_remove_queue_row(request);
  trout_transfer_request_destroy(request);
  trout_transfer_request_update_queue(window);
  if (window->active_transfer != 0) trout_transfer_request_update_label(window->active_transfer);
  else if (g_queue_is_empty(&window->transfer_queue)) {
    window->transfer_queue_paused = FALSE;
    if (trout_transfer_queue_has_notice(window)) trout_transfer_queue_show_finished_failure(window);
    else trout_transfer_queue_close_dialog(window);
  } else trout_transfer_queue_show_paused(window);
  trout_transfer_request_destroy(request);
  (void)button;
}

void
trout_transfer_queue_show_paused(trout_window *window)
{
  guint queued_count;
  char *text;
  queued_count = g_queue_get_length(&window->transfer_queue);
  text = g_strdup_printf("Paused after failure · %u queued", queued_count);
  gtk_label_set_text(GTK_LABEL(window->transfer_label), text);
  g_free(text);
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(window->transfer_progress), 0.0);
  gtk_widget_set_sensitive(window->transfer_pause_button, TRUE);
  gtk_button_set_label(GTK_BUTTON(window->transfer_pause_button), "Resume");
  gtk_widget_set_sensitive(window->transfer_cancel_button, FALSE);
}

void
trout_transfer_request_finish(trout_transfer_request *request)
{
  trout_window *window;
  GtkWidget *window_widget;
  window = request->window;
  if (window->active_transfer != request) return;
  window_widget = g_object_ref(request->window_widget);
  window->active_transfer = 0;
  trout_transfer_request_destroy(request);
  if (g_queue_is_empty(&window->transfer_queue)) {
    window->transfer_queue_paused = FALSE;
    if (trout_transfer_queue_has_notice(window)) trout_transfer_queue_show_finished_failure(window);
    else trout_transfer_queue_close_dialog(window);
  } else if (window->transfer_queue_paused) trout_transfer_queue_show_paused(window);
  else trout_transfer_request_start_next(window);
  g_object_unref(window_widget);
}

void
trout_transfer_request_start_next(trout_window *window)
{
  trout_transfer_request *request;
  GTask *task;
  if (window->transfer_queue_paused || g_queue_is_empty(&window->transfer_queue)) return;
  request = g_queue_pop_head(&window->transfer_queue);
  trout_transfer_request_remove_queue_row(request);
  window->active_transfer = request;
  trout_transfer_request_update_queue(window);
  gtk_widget_set_sensitive(window->transfer_cancel_button, TRUE);
  gtk_button_set_label(GTK_BUTTON(window->transfer_cancel_button), "Cancel");
  gtk_widget_set_sensitive(window->transfer_pause_button, TRUE);
  gtk_button_set_label(GTK_BUTTON(window->transfer_pause_button), "Pause");
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(window->transfer_progress), 0.0);
  g_free(request->current_name);
  if (request->operation == trout_file_operation_rename) request->current_name = trout_transfer_display_basename(trout_file_transfer_pair_get_source(g_ptr_array_index(request->operations, 0)));
  else request->current_name = g_strdup("files");
  trout_transfer_request_update_label(request);
  task = g_task_new(0, request->cancellable, trout_transfer_request_complete, trout_transfer_request_ref(request));
  /* The worker determines whether cancellation happened before or after a
   * committed native operation. Do not mask its final result in GTask. */
  g_task_set_check_cancellable(task, FALSE);
  g_task_set_task_data(task, request, 0);
  g_task_run_in_thread(task, trout_transfer_request_worker);
  g_object_unref(task);
}

void
trout_transfer_request_enqueue(trout_window *window, GPtrArray *operations, trout_file_operation operation)
{
  trout_transfer_request *request;
  GtkWidget *content;
  GtkWidget *buttons;
  GtkWidget *queue_scroller;
  GtkWidget *queue_list;
  GtkWidget *pause_button;
  GtkWidget *cancel_button;
  if (operations->len == 0) {
    g_ptr_array_unref(operations);
    return;
  }
  request = g_new0(trout_transfer_request, 1);
  request->window = window;
  request->window_widget = g_object_ref(window->window);
  request->operations = operations;
  request->operation = operation;
  request->cancellable = g_cancellable_new();
  request->references = 1;
  request->summary = trout_transfer_request_summary(request);
  g_mutex_init(&request->pause_mutex);
  g_cond_init(&request->pause_condition);
  g_mutex_init(&request->collision.mutex);
  g_cond_init(&request->collision.condition);
  g_queue_push_tail(&window->transfer_queue, request);
  if (window->transfer_dialog == 0) {
    window->transfer_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window->transfer_dialog), "File Operations");
    gtk_window_set_transient_for(GTK_WINDOW(window->transfer_dialog), GTK_WINDOW(window->window));
    gtk_window_set_default_size(GTK_WINDOW(window->transfer_dialog), 420, -1);
    gtk_window_set_deletable(GTK_WINDOW(window->transfer_dialog), FALSE);
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    window->transfer_label = gtk_label_new(0);
    gtk_label_set_xalign(GTK_LABEL(window->transfer_label), 0.0f);
    window->transfer_progress = gtk_progress_bar_new();
    window->transfer_error_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(window->transfer_error_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(window->transfer_error_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(window->transfer_error_label), TRUE);
    gtk_widget_set_visible(window->transfer_error_label, FALSE);
    queue_scroller = gtk_scrolled_window_new();
    window->transfer_queue_scroller = queue_scroller;
    gtk_widget_set_size_request(queue_scroller, -1, 150);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(queue_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    queue_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    window->transfer_queue_list = queue_list;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(queue_scroller), queue_list);
    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    pause_button = gtk_button_new_with_label("Pause");
    window->transfer_pause_button = pause_button;
    g_signal_connect(pause_button, "clicked", G_CALLBACK(trout_transfer_pause), window);
    cancel_button = gtk_button_new_with_label("Cancel");
    window->transfer_cancel_button = cancel_button;
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(trout_transfer_cancel), window);
    gtk_box_append(GTK_BOX(content), window->transfer_label);
    gtk_box_append(GTK_BOX(content), window->transfer_progress);
    gtk_box_append(GTK_BOX(content), window->transfer_error_label);
    gtk_box_append(GTK_BOX(content), queue_scroller);
    gtk_box_append(GTK_BOX(buttons), pause_button);
    gtk_box_append(GTK_BOX(buttons), cancel_button);
    gtk_box_append(GTK_BOX(content), buttons);
    gtk_window_set_child(GTK_WINDOW(window->transfer_dialog), content);
  }
  trout_transfer_request_add_queue_row(request);
  trout_transfer_request_update_queue(window);
  gtk_window_present(GTK_WINDOW(window->transfer_dialog));
  if (window->active_transfer == 0 && window->transfer_queue_paused) trout_transfer_queue_show_paused(window);
  else if (window->active_transfer == 0) trout_transfer_request_start_next(window);
  else trout_transfer_request_update_label(window->active_transfer);
}

void
trout_transfer_request_cancel(trout_transfer_request *request)
{
  GtkWidget *dialog;
  g_cancellable_cancel(request->cancellable);
  trout_transfer_request_release_pause(request);
  g_mutex_lock(&request->collision.mutex);
  request->collision.choice = trout_file_collision_cancel;
  request->collision.choice_ready = TRUE;
  request->collision.active = FALSE;
  g_cond_broadcast(&request->collision.condition);
  g_mutex_unlock(&request->collision.mutex);
  dialog = request->collision.dialog;
  request->collision.dialog = 0;
  if (dialog != 0) gtk_window_destroy(GTK_WINDOW(dialog));
  if (request->window->active_transfer == request) {
    gtk_widget_set_sensitive(request->window->transfer_pause_button, FALSE);
    gtk_widget_set_sensitive(request->window->transfer_cancel_button, FALSE);
    gtk_button_set_label(GTK_BUTTON(request->window->transfer_cancel_button), "Cancelling…");
  }
}

void
trout_transfer_pause(GtkButton *button, gpointer user_data)
{
  trout_window *window;
  trout_transfer_request *request;
  window = user_data;
  request = window->active_transfer;
  if (request != 0) {
    g_mutex_lock(&request->pause_mutex);
    request->paused = !request->paused;
    g_cond_broadcast(&request->pause_condition);
    g_mutex_unlock(&request->pause_mutex);
    gtk_button_set_label(button, request->paused ? "Resume" : "Pause");
    trout_transfer_request_update_label(request);
  } else if (window->transfer_queue_paused) {
    window->transfer_queue_paused = FALSE;
    trout_transfer_request_start_next(window);
  }
}

void
trout_transfer_cancel(GtkButton *button, gpointer user_data)
{
  trout_window *window;
  trout_transfer_request *request;
  window = user_data;
  request = window->active_transfer;
  if (request != 0) trout_transfer_request_cancel(request);
  else if (trout_transfer_queue_has_notice(window) && g_queue_is_empty(&window->transfer_queue)) trout_transfer_queue_close_dialog(window);
  (void)button;
}

gboolean
trout_file_operation_progress_update(gpointer user_data)
{
  trout_file_progress_update *update;
  trout_transfer_request *request;
  update = user_data;
  request = update->request;
  if (request->window->active_transfer == request) {
    g_free(request->current_name);
    request->current_name = g_strdup(update->name != 0 ? update->name : "file");
    trout_transfer_request_update_label(request);
    if (update->total_bytes > 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(request->window->transfer_progress), CLAMP((double)update->current_bytes / update->total_bytes, 0.0, 1.0));
    else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(request->window->transfer_progress));
  }
  return FALSE;
}

void
trout_file_operation_progress_update_destroy(gpointer user_data)
{
  trout_file_progress_update *update;
  update = user_data;
  trout_transfer_request_destroy(update->request);
  g_free(update->name);
  g_free(update);
}

void
trout_transfer_request_unref(gpointer user_data)
{
  trout_transfer_request_destroy(user_data);
}

void
trout_transfer_request_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  trout_transfer_request *request;
  trout_file_transfer_callbacks callbacks;
  GError *error;
  trout_file_transfer_result result;
  request = task_data;
  callbacks.user_data = request;
  callbacks.wait = trout_transfer_request_wait;
  callbacks.collision = trout_transfer_request_collision;
  callbacks.progress = trout_transfer_request_progress;
  error = 0;
  result = trout_file_transfer_execute_full(request->operations, request->operation, cancellable, &callbacks, &error);
  if (error != 0) g_task_return_error(task, error);
  else if (result == trout_file_transfer_failed || result == trout_file_transfer_cancelled) g_task_return_new_error(task, G_IO_ERROR, result == trout_file_transfer_cancelled ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED, "Transfer did not complete");
  else g_task_return_int(task, result);
  (void)source_object;
}

void
trout_transfer_request_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
  trout_transfer_request *request;
  GError *error;
  gssize outcome;
  request = user_data;
  error = 0;
  outcome = g_task_propagate_int(G_TASK(result), &error);
  if (request->collision.dialog != 0) {
    GtkWidget *dialog = request->collision.dialog;
    request->collision.dialog = 0;
    gtk_window_destroy(GTK_WINDOW(dialog));
  }
  if (error != 0) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_object_set_data(G_OBJECT(request->window->transfer_dialog), "trout_transfer_incomplete", GINT_TO_POINTER(1));
      trout_transfer_queue_add_notice(request, "Cancelled. Earlier completed changes have not been undone.");
    } else {
      request->window->transfer_had_failure = TRUE;
      char *message = g_strdup_printf("%s. Earlier completed changes have not been undone.", error->message);
      trout_transfer_queue_add_notice(request, message);
      g_free(message);
      if (!g_queue_is_empty(&request->window->transfer_queue)) request->window->transfer_queue_paused = TRUE;
    }
  } else if (outcome == trout_file_transfer_partial || outcome == trout_file_transfer_skipped) {
    g_object_set_data(G_OBJECT(request->window->transfer_dialog), "trout_transfer_incomplete", GINT_TO_POINTER(1));
    trout_transfer_queue_add_notice(request, outcome == trout_file_transfer_partial ? "Partially completed; some entries were skipped." : "Skipped; no entries were transferred.");
  }
  if (error != 0) g_error_free(error);
  trout_transfer_request_finish(request);
  trout_transfer_request_destroy(request);
  (void)source;
}

