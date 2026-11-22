void
trout_transfer_request_destroy(trout_transfer_request *request)
{
  if (g_atomic_int_dec_and_test(&request->references)) {
    g_object_unref(request->window_widget);
    g_object_unref(request->status_label);
    g_ptr_array_unref(request->operations);
    g_clear_object(&request->collision.source);
    g_clear_object(&request->collision.destination);
    g_clear_object(&request->collision.source_root);
    g_clear_object(&request->collision.destination_root);
    g_cond_clear(&request->collision.condition);
    g_mutex_clear(&request->collision.mutex);
    g_free(request->current_name);
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
  while (request->paused && !g_cancellable_is_cancelled(request->cancellable)) g_cond_wait(&request->pause_condition, &request->pause_mutex);
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
  update->name = g_file_get_basename(source);
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
  while (!request->collision.choice_ready && !g_cancellable_is_cancelled(request->cancellable)) g_cond_wait(&request->collision.condition, &request->collision.mutex);
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
  if (source_path[0] == '\0') {
    g_free(source_path);
    source_path = g_file_get_basename(source);
  }
  destination_path = g_file_get_relative_path(destination_root, destination);
  if (destination_path[0] == '\0') {
    g_free(destination_path);
    destination_path = g_file_get_basename(destination);
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
  destination_name = g_file_get_basename(destination);
  if (destination_type == G_FILE_TYPE_DIRECTORY) message = g_strdup_printf("An item named “%s” already exists. Trout will not replace directories because that could delete their contents. Choose Skip or Cancel.", destination_name);
  else message = g_strdup_printf("An item named “%s” already exists in the destination.", destination_name);
  dialog = gtk_window_new();
  request->collision.dialog = dialog;
  g_object_set_data(G_OBJECT(dialog), "trout_transfer_request", request);
  gtk_window_set_title(GTK_WINDOW(dialog), "File Already Exists");
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
  if (destination_type != G_FILE_TYPE_DIRECTORY) {
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
    if (queued_count > 0) text = g_strdup_printf("Waiting: %s %s · %u queued", request->operation == trout_file_operation_copy ? "Copying" : "Moving", request->current_name != 0 ? request->current_name : "files", queued_count);
    else text = g_strdup_printf("Waiting: %s %s", request->operation == trout_file_operation_copy ? "Copying" : "Moving", request->current_name != 0 ? request->current_name : "files");
  } else if (paused) {
    if (queued_count > 0) text = g_strdup_printf("Paused: %s %s · %u queued", request->operation == trout_file_operation_copy ? "Copying" : "Moving", request->current_name != 0 ? request->current_name : "files", queued_count);
    else text = g_strdup_printf("Paused: %s %s", request->operation == trout_file_operation_copy ? "Copying" : "Moving", request->current_name != 0 ? request->current_name : "files");
  } else if (queued_count > 0) text = g_strdup_printf("%s %s · %u queued", request->operation == trout_file_operation_copy ? "Copying" : "Moving", request->current_name != 0 ? request->current_name : "files", queued_count);
  else text = g_strdup_printf("%s %s", request->operation == trout_file_operation_copy ? "Copying" : "Moving", request->current_name != 0 ? request->current_name : "files");
  gtk_label_set_text(GTK_LABEL(request->window->transfer_label), text);
  g_free(text);
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
    gtk_window_destroy(GTK_WINDOW(window->transfer_dialog));
    window->transfer_dialog = 0;
    window->transfer_label = 0;
    window->transfer_progress = 0;
    window->transfer_pause_button = 0;
    window->transfer_cancel_button = 0;
  } else trout_transfer_request_start_next(window);
  g_object_unref(window_widget);
}

void
trout_transfer_request_start_next(trout_window *window)
{
  trout_transfer_request *request;
  GTask *task;
  request = g_queue_pop_head(&window->transfer_queue);
  window->active_transfer = request;
  gtk_widget_set_sensitive(window->transfer_cancel_button, TRUE);
  gtk_button_set_label(GTK_BUTTON(window->transfer_cancel_button), "Cancel");
  gtk_widget_set_sensitive(window->transfer_pause_button, TRUE);
  gtk_button_set_label(GTK_BUTTON(window->transfer_pause_button), "Pause");
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(window->transfer_progress), 0.0);
  g_free(request->current_name);
  request->current_name = g_strdup("files");
  trout_transfer_request_update_label(request);
  task = g_task_new(0, request->cancellable, trout_transfer_request_complete, trout_transfer_request_ref(request));
  g_task_set_task_data(task, request, 0);
  g_task_run_in_thread(task, trout_transfer_request_worker);
  g_object_unref(task);
}

void
trout_transfer_request_enqueue(trout_window *window, GtkWidget *status_label, GPtrArray *operations, trout_file_operation operation)
{
  trout_transfer_request *request;
  GtkWidget *content;
  GtkWidget *buttons;
  GtkWidget *pause_button;
  GtkWidget *cancel_button;
  if (operations->len == 0) {
    g_ptr_array_unref(operations);
    return;
  }
  request = g_new0(trout_transfer_request, 1);
  request->window = window;
  request->window_widget = g_object_ref(window->window);
  request->status_label = g_object_ref(status_label);
  request->operations = operations;
  request->operation = operation;
  request->cancellable = g_cancellable_new();
  request->references = 1;
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
    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    pause_button = gtk_button_new_with_label("Pause");
    window->transfer_pause_button = pause_button;
    g_signal_connect(pause_button, "clicked", G_CALLBACK(trout_transfer_pause), window);
    cancel_button = gtk_button_new_with_label("Cancel");
    window->transfer_cancel_button = cancel_button;
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(trout_transfer_cancel), window);
    gtk_box_append(GTK_BOX(content), window->transfer_label);
    gtk_box_append(GTK_BOX(content), window->transfer_progress);
    gtk_box_append(GTK_BOX(buttons), pause_button);
    gtk_box_append(GTK_BOX(buttons), cancel_button);
    gtk_box_append(GTK_BOX(content), buttons);
    gtk_window_set_child(GTK_WINDOW(window->transfer_dialog), content);
  }
  gtk_window_present(GTK_WINDOW(window->transfer_dialog));
  if (window->active_transfer == 0) trout_transfer_request_start_next(window);
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
    if (update->total_bytes > 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(request->window->transfer_progress), (double)update->current_bytes / update->total_bytes);
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
  gboolean succeeded;
  request = task_data;
  callbacks.user_data = request;
  callbacks.wait = trout_transfer_request_wait;
  callbacks.collision = trout_transfer_request_collision;
  callbacks.progress = trout_transfer_request_progress;
  error = 0;
  succeeded = trout_file_transfer_execute(request->operations, request->operation, cancellable, &callbacks, &error);
  if (succeeded) g_task_return_boolean(task, TRUE);
  else g_task_return_error(task, error);
  (void)source_object;
}

void
trout_transfer_request_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
  trout_transfer_request *request;
  GError *error;
  gboolean succeeded;
  request = user_data;
  error = 0;
  succeeded = g_task_propagate_boolean(G_TASK(result), &error);
  if (!succeeded && error != 0 && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) gtk_label_set_text(GTK_LABEL(request->status_label), "Cannot copy a directory into itself or one of its subdirectories");
    else trout_tab_show_error(request->status_label, error);
  }
  if (error != 0) g_error_free(error);
  trout_transfer_request_finish(request);
  trout_transfer_request_destroy(request);
  (void)source;
}

