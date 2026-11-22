#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "file_transfer.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

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
static gboolean trout_file_transfer_destination_is_inside_source(GFile *source, GFile *destination, GFileInfo *source_info, GCancellable *cancellable, gboolean *inside, GError **error);
static gboolean trout_file_transfer_entry(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error);
static gboolean trout_file_transfer_directory(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error);

gboolean
trout_file_transfer_should_merge(GFileType source_type, GFileType destination_type)
{
  return source_type == G_FILE_TYPE_DIRECTORY && destination_type == G_FILE_TYPE_DIRECTORY;
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
    if (!trout_file_transfer_entry(pair->source, pair->destination, pair->source, pair->destination, operation, cancellable, callbacks, error)) return FALSE;
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
  info = g_file_query_info(destination, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
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
trout_file_transfer_destination_is_inside_source(GFile *source, GFile *destination, GFileInfo *source_info, GCancellable *cancellable, gboolean *inside, GError **error)
{
  char *source_path;
  char *destination_path;
  char *ancestor_path;
  char *resolved_path;
  char *parent_path;
  char *source_filesystem_id;
  char *source_file_id;
  char *ancestor_filesystem_id;
  char *ancestor_file_id;
  GFile *ancestor;
  GFile *parent;
  GFileInfo *ancestor_info;
  GError *query_error;
  struct stat source_stat;
  struct stat ancestor_stat;
  int error_number;
  *inside = FALSE;
  source_path = g_file_get_path(source);
  destination_path = g_file_get_path(destination);
  if (source_path != 0 && destination_path != 0) {
    if (stat(source_path, &source_stat) != 0) {
      error_number = errno;
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(error_number), "Could not inspect transfer source: %s", g_strerror(error_number));
      g_free(source_path);
      g_free(destination_path);
      return FALSE;
    }
    ancestor_path = realpath(destination_path, 0);
    if (ancestor_path == 0 && errno != ENOENT && errno != ENOTDIR) {
      error_number = errno;
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(error_number), "Could not resolve transfer destination: %s", g_strerror(error_number));
      g_free(source_path);
      g_free(destination_path);
      return FALSE;
    }
    if (ancestor_path == 0) {
      parent_path = g_path_get_dirname(destination_path);
      while (1) {
        resolved_path = realpath(parent_path, 0);
        if (resolved_path != 0) {
          g_free(parent_path);
          ancestor_path = resolved_path;
          break;
        }
        error_number = errno;
        if (error_number != ENOENT && error_number != ENOTDIR) {
          g_set_error(error, G_IO_ERROR, g_io_error_from_errno(error_number), "Could not resolve transfer destination ancestor: %s", g_strerror(error_number));
          g_free(source_path);
          g_free(destination_path);
          g_free(parent_path);
          return FALSE;
        }
        resolved_path = g_path_get_dirname(parent_path);
        if (g_strcmp0(resolved_path, parent_path) == 0) {
          g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Could not find an existing transfer destination ancestor");
          g_free(source_path);
          g_free(destination_path);
          g_free(parent_path);
          g_free(resolved_path);
          return FALSE;
        }
        g_free(parent_path);
        parent_path = resolved_path;
      }
    }
    while (1) {
      if (stat(ancestor_path, &ancestor_stat) != 0) {
        error_number = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(error_number), "Could not inspect transfer destination ancestor: %s", g_strerror(error_number));
        g_free(source_path);
        g_free(destination_path);
        g_free(ancestor_path);
        return FALSE;
      }
      if (source_stat.st_dev == ancestor_stat.st_dev && source_stat.st_ino == ancestor_stat.st_ino) {
        *inside = TRUE;
        g_free(source_path);
        g_free(destination_path);
        g_free(ancestor_path);
        return TRUE;
      }
      parent_path = g_path_get_dirname(ancestor_path);
      if (g_strcmp0(parent_path, ancestor_path) == 0) {
        g_free(parent_path);
        g_free(source_path);
        g_free(destination_path);
        g_free(ancestor_path);
        return TRUE;
      }
      g_free(ancestor_path);
      ancestor_path = parent_path;
    }
  }
  g_free(source_path);
  g_free(destination_path);
  source_filesystem_id = (char *)g_file_info_get_attribute_string(source_info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
  source_file_id = (char *)g_file_info_get_attribute_string(source_info, G_FILE_ATTRIBUTE_ID_FILE);
  if (source_filesystem_id == 0 || source_file_id == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Could not identify the transfer source filesystem and file");
    return FALSE;
  }
  ancestor = g_object_ref(destination);
  while (ancestor != 0) {
    query_error = 0;
    ancestor_info = g_file_query_info(ancestor, G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_ID_FILE, G_FILE_QUERY_INFO_NONE, cancellable, &query_error);
    if (ancestor_info == 0) {
      if (g_error_matches(query_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
        g_clear_error(&query_error);
        parent = g_file_get_parent(ancestor);
        g_object_unref(ancestor);
        ancestor = parent;
        continue;
      }
      g_object_unref(ancestor);
      g_propagate_error(error, query_error);
      return FALSE;
    }
    ancestor_filesystem_id = (char *)g_file_info_get_attribute_string(ancestor_info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
    ancestor_file_id = (char *)g_file_info_get_attribute_string(ancestor_info, G_FILE_ATTRIBUTE_ID_FILE);
    if (ancestor_filesystem_id == 0 || ancestor_file_id == 0) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Could not identify a transfer destination ancestor");
      g_object_unref(ancestor_info);
      g_object_unref(ancestor);
      return FALSE;
    }
    if (g_strcmp0(source_filesystem_id, ancestor_filesystem_id) == 0 && g_strcmp0(source_file_id, ancestor_file_id) == 0) {
      *inside = TRUE;
      g_object_unref(ancestor_info);
      g_object_unref(ancestor);
      return TRUE;
    }
    g_object_unref(ancestor_info);
    parent = g_file_get_parent(ancestor);
    g_object_unref(ancestor);
    ancestor = parent;
  }
  return TRUE;
}

static gboolean
trout_file_transfer_entry(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error)
{
  GFileInfo *source_info;
  GFileInfo *destination_info;
  GFileType source_type;
  GFileType destination_type;
  GFileCopyFlags flags;
  trout_file_collision_choice choice;
  goffset source_size;
  goffset destination_size;
  gboolean destination_exists;
  gboolean succeeded;
  gboolean destination_inside_source;
  guint attempt;
  trout_file_transfer_progress progress;
  if (!trout_file_transfer_wait(callbacks, cancellable, error)) return FALSE;
  source_info = g_file_query_info(source, "standard::type,standard::size," G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_ID_FILE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (source_info == 0) return FALSE;
  source_type = g_file_info_get_file_type(source_info);
  source_size = g_file_info_get_size(source_info);
  if (source_type == G_FILE_TYPE_DIRECTORY) {
    if (!trout_file_transfer_destination_is_inside_source(source, destination, source_info, cancellable, &destination_inside_source, error)) {
      g_object_unref(source_info);
      return FALSE;
    }
    if (destination_inside_source) {
      g_object_unref(source_info);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE, "Cannot transfer a directory into itself or one of its subdirectories");
      return FALSE;
    }
  }
  g_object_unref(source_info);
  attempt = 0;
  while (attempt < 3) {
    if (!trout_file_transfer_wait(callbacks, cancellable, error)) return FALSE;
    destination_exists = FALSE;
    destination_info = trout_file_transfer_query_destination(destination, cancellable, &destination_exists, error);
    if (!destination_exists && *error != 0) return FALSE;
    destination_type = destination_info != 0 ? g_file_info_get_file_type(destination_info) : G_FILE_TYPE_UNKNOWN;
    destination_size = destination_info != 0 ? g_file_info_get_size(destination_info) : 0;
    if (destination_exists && trout_file_transfer_should_merge(source_type, destination_type)) {
      g_object_unref(destination_info);
      return trout_file_transfer_directory(source, destination, source_root, destination_root, operation, cancellable, callbacks, error);
    }
    flags = G_FILE_COPY_NONE;
    if (destination_exists) {
      choice = callbacks->collision(source, destination, source_root, destination_root, source_type, destination_type, source_size, destination_size, callbacks->user_data);
      if (choice == trout_file_collision_skip) {
        g_object_unref(destination_info);
        return TRUE;
      }
      if (choice == trout_file_collision_cancel) {
        g_object_unref(destination_info);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Transfer cancelled");
        return FALSE;
      }
      if (destination_type == G_FILE_TYPE_DIRECTORY) {
        g_object_unref(destination_info);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Refusing to replace a directory because it may contain files");
        return FALSE;
      }
      if (source_type == G_FILE_TYPE_REGULAR && destination_type == G_FILE_TYPE_REGULAR) flags = G_FILE_COPY_OVERWRITE;
      else {
        g_object_unref(destination_info);
        if (!g_file_delete(destination, cancellable, error)) return FALSE;
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
      return trout_file_transfer_directory(source, destination, source_root, destination_root, operation, cancellable, callbacks, error);
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
trout_file_transfer_directory(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error)
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
    if (!trout_file_transfer_entry(source_child, destination_child, source_root, destination_root, operation, cancellable, callbacks, error)) {
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
  if (operation == trout_file_operation_move) {
    if (!trout_file_transfer_wait(callbacks, cancellable, error)) return FALSE;
    if (!g_file_delete(source, cancellable, error)) {
      if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY)) {
        g_clear_error(error);
        return TRUE;
      }
      return FALSE;
    }
  }
  return TRUE;
}
