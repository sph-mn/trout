#include "file_transfer.h"

struct trout_file_transfer_pair {
  GFile *source;
  GFile *destination;
};

typedef struct {
  const trout_file_transfer_callbacks *callbacks;
  GFile *source;
  GCancellable *cancellable;
} trout_file_transfer_progress;

static gboolean trout_file_transfer_wait(const trout_file_transfer_callbacks *callbacks, GCancellable *cancellable, GError **error);
static void trout_file_transfer_progress_update(goffset current_bytes, goffset total_bytes, gpointer user_data);
static gboolean trout_file_transfer_is_collision(GError *error);
static GFileInfo *trout_file_transfer_query_destination(GFile *destination, GCancellable *cancellable, gboolean *exists, GError **error);
static gboolean trout_file_transfer_entry(GFile *source, GFile *destination, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error);
static gboolean trout_file_transfer_copy_directory(GFile *source, GFile *destination, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error);

gboolean
trout_file_copy_should_merge(GFileType source_type, GFileType destination_type, trout_file_operation operation)
{
  return operation == trout_file_operation_copy && source_type == G_FILE_TYPE_DIRECTORY && destination_type == G_FILE_TYPE_DIRECTORY;
}

trout_file_transfer_pair *
trout_file_transfer_pair_new(GFile *source, GFile *destination)
{
  trout_file_transfer_pair *pair;
  pair = g_new0(trout_file_transfer_pair, 1);
  pair->source = g_object_ref(source);
  pair->destination = g_object_ref(destination);
  return pair;
}

gboolean
trout_file_transfer_execute(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error)
{
  guint index;
  trout_file_transfer_pair *pair;
  index = 0;
  while (index < operations->len) {
    pair = g_ptr_array_index(operations, index);
    if (!trout_file_transfer_entry(pair->source, pair->destination, operation, cancellable, callbacks, error)) return FALSE;
    index += 1;
  }
  return TRUE;
}

void
trout_file_transfer_pair_destroy(gpointer user_data)
{
  trout_file_transfer_pair *pair;
  pair = user_data;
  g_object_unref(pair->source);
  g_object_unref(pair->destination);
  g_free(pair);
}

static gboolean
trout_file_transfer_wait(const trout_file_transfer_callbacks *callbacks, GCancellable *cancellable, GError **error)
{
  if (callbacks->wait(callbacks->user_data) && !g_cancellable_is_cancelled(cancellable)) return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Transfer cancelled");
  return FALSE;
}

static void
trout_file_transfer_progress_update(goffset current_bytes, goffset total_bytes, gpointer user_data)
{
  trout_file_transfer_progress *progress;
  progress = user_data;
  if (!g_cancellable_is_cancelled(progress->cancellable)) {
    if (progress->callbacks->wait(progress->callbacks->user_data)) {
      if (progress->callbacks->progress != 0) progress->callbacks->progress(progress->source, current_bytes, total_bytes, progress->callbacks->user_data);
    }
  }
}

static gboolean
trout_file_transfer_is_collision(GError *error)
{
  return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS) ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY) ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE);
}

static GFileInfo *
trout_file_transfer_query_destination(GFile *destination, GCancellable *cancellable, gboolean *exists, GError **error)
{
  GFileInfo *info;
  info = g_file_query_info(destination, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (info != 0) {
    *exists = TRUE;
    return info;
  }
  if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
    g_clear_error(error);
    *exists = FALSE;
    return 0;
  }
  return 0;
}

static gboolean
trout_file_transfer_entry(GFile *source, GFile *destination, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error)
{
  GFileInfo *source_info;
  GFileInfo *destination_info;
  GFileType source_type;
  GFileType destination_type;
  GFileCopyFlags flags;
  trout_file_collision_choice choice;
  gboolean destination_exists;
  gboolean succeeded;
  guint attempt;
  trout_file_transfer_progress progress;
  if (!trout_file_transfer_wait(callbacks, cancellable, error)) return FALSE;
  source_info = g_file_query_info(source, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (source_info == 0) return FALSE;
  source_type = g_file_info_get_file_type(source_info);
  g_object_unref(source_info);
  if (operation == trout_file_operation_copy && source_type == G_FILE_TYPE_DIRECTORY && g_file_has_prefix(destination, source)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE, "Cannot copy a directory into itself or one of its subdirectories");
    return FALSE;
  }
  attempt = 0;
  while (attempt < 3) {
    if (!trout_file_transfer_wait(callbacks, cancellable, error)) return FALSE;
    destination_exists = FALSE;
    destination_info = trout_file_transfer_query_destination(destination, cancellable, &destination_exists, error);
    if (!destination_exists && *error != 0) return FALSE;
    destination_type = destination_info != 0 ? g_file_info_get_file_type(destination_info) : G_FILE_TYPE_UNKNOWN;
    if (destination_exists && trout_file_copy_should_merge(source_type, destination_type, operation)) {
      g_object_unref(destination_info);
      return trout_file_transfer_copy_directory(source, destination, cancellable, callbacks, error);
    }
    flags = G_FILE_COPY_NONE;
    if (destination_exists) {
      choice = callbacks->collision(source, destination, source_type, destination_type, callbacks->user_data);
      if (choice == trout_file_collision_skip) {
        g_object_unref(destination_info);
        return TRUE;
      }
      if (choice == trout_file_collision_cancel) {
        g_object_unref(destination_info);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Transfer cancelled");
        return FALSE;
      }
      if (source_type == G_FILE_TYPE_REGULAR && destination_type == G_FILE_TYPE_REGULAR) flags = G_FILE_COPY_OVERWRITE;
      else {
        g_object_unref(destination_info);
        if (!trout_file_delete_tree(destination, cancellable, error)) return FALSE;
        destination_exists = FALSE;
        destination_info = 0;
      }
    }
    if (destination_info != 0) g_object_unref(destination_info);
    if (operation == trout_file_operation_copy && source_type == G_FILE_TYPE_DIRECTORY) {
      if (!destination_exists && !g_file_make_directory(destination, cancellable, error)) {
        if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
          g_clear_error(error);
          attempt += 1;
          continue;
        }
        return FALSE;
      }
      return trout_file_transfer_copy_directory(source, destination, cancellable, callbacks, error);
    }
    if (!trout_file_transfer_wait(callbacks, cancellable, error)) return FALSE;
    progress.callbacks = callbacks;
    progress.source = source;
    progress.cancellable = cancellable;
    if (operation == trout_file_operation_copy) succeeded = g_file_copy(source, destination, flags, cancellable, trout_file_transfer_progress_update, &progress, error);
    else succeeded = g_file_move(source, destination, flags, cancellable, trout_file_transfer_progress_update, &progress, error);
    if (succeeded) return TRUE;
    if (trout_file_transfer_is_collision(*error) && attempt < 2) {
      g_clear_error(error);
      attempt += 1;
      continue;
    }
    return FALSE;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Destination changed repeatedly during transfer");
  return FALSE;
}

static gboolean
trout_file_transfer_copy_directory(GFile *source, GFile *destination, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error)
{
  GFileEnumerator *enumerator;
  GFileInfo *info;
  GFile *source_child;
  GFile *destination_child;
  const char *name;
  enumerator = g_file_enumerate_children(source, "standard::name", G_FILE_QUERY_INFO_NONE, cancellable, error);
  if (enumerator == 0) return FALSE;
  while ((info = g_file_enumerator_next_file(enumerator, cancellable, error)) != 0) {
    name = g_file_info_get_name(info);
    source_child = g_file_get_child(source, name);
    destination_child = g_file_get_child(destination, name);
    g_object_unref(info);
    if (!trout_file_transfer_entry(source_child, destination_child, trout_file_operation_copy, cancellable, callbacks, error)) {
      g_object_unref(source_child);
      g_object_unref(destination_child);
      g_object_unref(enumerator);
      return FALSE;
    }
    g_object_unref(source_child);
    g_object_unref(destination_child);
  }
  if (*error != 0) {
    g_object_unref(enumerator);
    return FALSE;
  }
  if (!g_file_enumerator_close(enumerator, cancellable, error)) {
    g_object_unref(enumerator);
    return FALSE;
  }
  g_object_unref(enumerator);
  return TRUE;
}

gboolean
trout_file_delete_tree(GFile *file, GCancellable *cancellable, GError **error)
{
  GFileInfo *info;
  GFileInfo *child_info;
  GFileEnumerator *enumerator;
  GPtrArray *directories;
  GPtrArray *files;
  GFile *directory;
  GFile *child;
  const char *name;
  GFileType child_type;
  guint index;
  info = g_file_query_info(file, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (info == 0) {
    if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_clear_error(error);
      return TRUE;
    }
    return FALSE;
  }
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
    g_object_unref(info);
    return g_file_delete(file, cancellable, error);
  }
  g_object_unref(info);
  directories = g_ptr_array_new_with_free_func(g_object_unref);
  files = g_ptr_array_new_with_free_func(g_object_unref);
  g_ptr_array_add(directories, g_object_ref(file));
  index = 0;
  while (index < directories->len) {
    directory = g_ptr_array_index(directories, index);
    enumerator = g_file_enumerate_children(directory, "standard::name,standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (enumerator == 0) {
      g_ptr_array_unref(directories);
      g_ptr_array_unref(files);
      return FALSE;
    }
    while ((info = g_file_enumerator_next_file(enumerator, cancellable, error)) != 0) {
      name = g_file_info_get_name(info);
      child = g_file_get_child(directory, name);
      child_type = g_file_info_get_file_type(info);
      if (child_type == G_FILE_TYPE_UNKNOWN) {
        child_info = g_file_query_info(child, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
        if (child_info == 0) {
          g_object_unref(child);
          g_object_unref(info);
          g_object_unref(enumerator);
          g_ptr_array_unref(directories);
          g_ptr_array_unref(files);
          return FALSE;
        }
        child_type = g_file_info_get_file_type(child_info);
        g_object_unref(child_info);
      }
      if (child_type == G_FILE_TYPE_UNKNOWN) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not determine an existing item's type");
        g_object_unref(child);
        g_object_unref(info);
        g_object_unref(enumerator);
        g_ptr_array_unref(directories);
        g_ptr_array_unref(files);
        return FALSE;
      }
      if (child_type == G_FILE_TYPE_DIRECTORY) g_ptr_array_add(directories, child);
      else g_ptr_array_add(files, child);
      g_object_unref(info);
    }
    if (*error != 0) {
      g_object_unref(enumerator);
      g_ptr_array_unref(directories);
      g_ptr_array_unref(files);
      return FALSE;
    }
    if (!g_file_enumerator_close(enumerator, cancellable, error)) {
      g_object_unref(enumerator);
      g_ptr_array_unref(directories);
      g_ptr_array_unref(files);
      return FALSE;
    }
    g_object_unref(enumerator);
    index += 1;
  }
  index = 0;
  while (index < files->len) {
    if (!g_file_delete(g_ptr_array_index(files, index), cancellable, error)) {
      g_ptr_array_unref(directories);
      g_ptr_array_unref(files);
      return FALSE;
    }
    index += 1;
  }
  index = directories->len;
  while (index > 0) {
    directory = g_ptr_array_index(directories, index - 1);
    if (!g_file_delete(directory, cancellable, error)) {
      g_ptr_array_unref(directories);
      g_ptr_array_unref(files);
      return FALSE;
    }
    index -= 1;
  }
  g_ptr_array_unref(directories);
  g_ptr_array_unref(files);
  return TRUE;
}
