#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "file_transfer.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define trout_transfer_attributes "standard::name,standard::type,standard::size,id::filesystem,id::file,unix::device,unix::inode,unix::mode,unix::is-mountpoint,time::modified,time::modified-usec,time::modified-nsec,time::access,time::access-usec,time::access-nsec"
#define trout_transfer_unit ((goffset)1000000)
#define trout_transfer_flags (G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA)

struct trout_file_transfer_pair {
  GFile *source;
  GFile *destination;
  char *display_name;
};

typedef struct trout_transfer_access_node trout_transfer_access_node;
struct trout_transfer_access_node {
  GFile *file;
  GFileInfo *info;
};

typedef struct trout_transfer_path_claim trout_transfer_path_claim;
struct trout_transfer_path_claim {
  char *path;
  gboolean recursive;
};

typedef struct trout_transfer_node trout_transfer_node;
struct trout_transfer_node {
  GFile *source;
  GFile *destination;
  GFileInfo *info;
  GFileInfo *destination_info;
  trout_transfer_node *parent;
  gboolean preflight_blocked;
  gboolean omitted;
  gboolean skipped;
  gboolean created;
  gboolean moved;
};

typedef struct {
  const trout_file_transfer_callbacks *callbacks;
  GCancellable *cancellable;
  GHashTable *protected_ids;
  GFile *source_root;
  GFile *destination_root;
  GFile *progress_source;
  guint pair_index;
  guint pair_count;
  guint node_index;
  guint node_count;
  gint64 last_progress;
  goffset last_current;
  gboolean completed;
  gboolean skipped;
  gboolean deleting;
  gboolean moving;
} trout_transfer_context;

static gboolean
trout_transfer_wait(trout_transfer_context *context, GError **error)
{
  if ((context->callbacks == 0 || context->callbacks->wait == 0 || context->callbacks->wait(context->callbacks->user_data)) &&
      !g_cancellable_set_error_if_cancelled(context->cancellable, error)) return TRUE;
  if (*error == 0) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Transfer cancelled");
  return FALSE;
}

static void
trout_transfer_progress(trout_transfer_context *context, double fraction, gboolean force)
{
  gint64 now;
  goffset current;
  if (context->callbacks == 0 || context->callbacks->progress == 0 || context->progress_source == 0) return;
  now = g_get_monotonic_time();
  if (!force && now - context->last_progress < 100000) return;
  context->last_progress = now;
  current = (goffset)context->pair_index * trout_transfer_unit;
  if (context->node_count != 0) current += (goffset)(trout_transfer_unit * (context->node_index + CLAMP(fraction, 0.0, 1.0)) / context->node_count);
  if (current < context->last_current) current = context->last_current;
  context->last_current = current;
  context->callbacks->progress(context->progress_source, current, (goffset)context->pair_count * trout_transfer_unit, context->callbacks->user_data);
}

static void
trout_transfer_file_progress(goffset current, goffset total, gpointer data)
{
  trout_transfer_context *context = data;
  if (context->callbacks != 0 && context->callbacks->wait != 0 && !context->callbacks->wait(context->callbacks->user_data)) {
    if (context->cancellable != 0) g_cancellable_cancel(context->cancellable);
    return;
  }
  if (!g_cancellable_is_cancelled(context->cancellable)) trout_transfer_progress(context, total > 0 ? (double)current / total : 0.0, FALSE);
}

static GFileInfo *
trout_transfer_query(GFile *file, GFileQueryInfoFlags flags, GCancellable *cancellable, gboolean allow_missing, GError **error)
{
  GFileInfo *info = g_file_query_info(file, trout_transfer_attributes, flags, cancellable, error);
  if (info == 0 && allow_missing && (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) || g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))) g_clear_error(error);
  return info;
}

/* Prefer local device/inode, including the final symlink itself. Remote backends
 * must provide stable file and filesystem IDs; otherwise alias checks fail closed. */
static char *
trout_transfer_identity(GFileInfo *info, GError **error)
{
  const char *filesystem;
  const char *file;
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE))
    return g_strdup_printf("unix:%" G_GUINT32_FORMAT ":%" G_GUINT64_FORMAT, g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE), g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE));
  filesystem = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
  file = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE);
  if (filesystem != 0 && file != 0) return g_strdup_printf("gio:%" G_GSIZE_FORMAT ":%s%s", strlen(filesystem), filesystem, file);
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Filesystem does not expose stable file identities for a safe transfer");
  return 0;
}

static gboolean
trout_transfer_same_info(GFileInfo *first, GFileInfo *second, gboolean *same, GError **error)
{
  char *a;
  char *b;
  *same = FALSE;
  if (first == 0 || second == 0) {
    *same = first == second;
    return TRUE;
  }
  a = trout_transfer_identity(first, error);
  if (a == 0) return FALSE;
  b = trout_transfer_identity(second, error);
  if (b != 0) {
    *same = strcmp(a, b) == 0 && g_file_info_get_file_type(first) == g_file_info_get_file_type(second);
    /* Directory timestamps change as children are added/removed. For leaf
     * entries also detect ordinary in-place changes during a long copy. */
    if (*same && g_file_info_get_file_type(first) != G_FILE_TYPE_DIRECTORY) {
      *same = g_file_info_get_size(first) == g_file_info_get_size(second) &&
          g_file_info_get_attribute_uint64(first, G_FILE_ATTRIBUTE_TIME_MODIFIED) == g_file_info_get_attribute_uint64(second, G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
          g_file_info_get_attribute_uint32(first, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) == g_file_info_get_attribute_uint32(second, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) &&
          g_file_info_get_attribute_uint32(first, "time::modified-nsec") == g_file_info_get_attribute_uint32(second, "time::modified-nsec");
    }
  }
  g_free(a);
  if (b == 0) return FALSE;
  g_free(b);
  return TRUE;
}

static gboolean
trout_transfer_revalidate(GFile *file, GFileInfo *expected, GCancellable *cancellable, GError **error)
{
  GFileInfo *actual;
  gboolean same;
  actual = trout_transfer_query(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, TRUE, error);
  if (*error != 0) return FALSE;
  if (!trout_transfer_same_info(expected, actual, &same, error)) {
    g_clear_object(&actual);
    return FALSE;
  }
  g_clear_object(&actual);
  if (!same) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "A transfer entry changed after inspection; refusing to continue");
  return same;
}

static gboolean
trout_transfer_revalidate_parent(trout_transfer_node *node, trout_transfer_context *context, GError **error)
{
  while (node->parent != 0) {
    node = node->parent;
    if (!trout_transfer_revalidate(node->source, node->info, context->cancellable, error)) return FALSE;
    if (!context->deleting && !trout_transfer_revalidate(node->destination, node->destination_info, context->cancellable, error)) return FALSE;
  }
  return TRUE;
}

static gboolean
trout_transfer_check_id(GFileInfo *info, GHashTable *ids, GError **error)
{
  char *id = trout_transfer_identity(info, error);
  gboolean overlap;
  if (id == 0) return FALSE;
  overlap = g_hash_table_contains(ids, id);
  g_free(id);
  if (overlap) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE, "Source and destination overlap or refer to the same filesystem object");
  return !overlap;
}

/* Check both lexical ancestors (including bind mounts) and resolved ancestors
 * of the deepest existing local parent. Never resolve the destination leaf:
 * a symlink at that leaf is an entry to replace, not a directory to enter. */
static gboolean
trout_transfer_check_ancestors(GFile *file, GHashTable *ids, GCancellable *cancellable, GError **error)
{
  GFile *ancestor;
  GFile *parent;
  GFile *resolved = 0;
  GFileInfo *info;
  char *path;
  char *canonical;
  gboolean succeeded = TRUE;
  ancestor = g_file_get_parent(file);
  while (ancestor != 0) {
    info = trout_transfer_query(ancestor, G_FILE_QUERY_INFO_NONE, cancellable, TRUE, error);
    if (*error != 0 || (info != 0 && !trout_transfer_check_id(info, ids, error))) succeeded = FALSE;
    if (succeeded && info != 0 && resolved == 0) {
      path = g_file_get_path(ancestor);
      if (path != 0) {
        canonical = realpath(path, 0);
        if (canonical != 0) {
          resolved = g_file_new_for_path(canonical);
          free(canonical);
        } else {
          int error_number = errno;
          g_set_error(error, G_IO_ERROR, g_io_error_from_errno(error_number), "Could not resolve transfer ancestor: %s", g_strerror(error_number));
          succeeded = FALSE;
        }
        g_free(path);
      }
    }
    g_clear_object(&info);
    parent = succeeded ? g_file_get_parent(ancestor) : 0;
    g_object_unref(ancestor);
    ancestor = parent;
  }
  ancestor = resolved;
  while (succeeded && ancestor != 0) {
    info = trout_transfer_query(ancestor, G_FILE_QUERY_INFO_NONE, cancellable, FALSE, error);
    succeeded = info != 0 && trout_transfer_check_id(info, ids, error);
    g_clear_object(&info);
    parent = succeeded ? g_file_get_parent(ancestor) : 0;
    g_object_unref(ancestor);
    ancestor = parent;
  }
  g_clear_object(&ancestor);
  return succeeded;
}

static gboolean
trout_transfer_check_destination(GFile *destination, GFileInfo *info, trout_transfer_context *context, GError **error)
{
  if (info != 0 && !trout_transfer_check_id(info, context->protected_ids, error)) return FALSE;
  return trout_transfer_check_ancestors(destination, context->protected_ids, context->cancellable, error);
}

static GHashTable *
trout_transfer_ids_new(void)
{
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);
}

static void
trout_transfer_node_destroy(gpointer data)
{
  trout_transfer_node *node = data;
  g_object_unref(node->source);
  g_object_unref(node->destination);
  g_object_unref(node->info);
  g_clear_object(&node->destination_info);
  g_free(node);
}

static trout_transfer_node *
trout_transfer_node_new(GFile *source, GFile *destination, GFileInfo *info, trout_transfer_node *parent)
{
  trout_transfer_node *node = g_new0(trout_transfer_node, 1);
  node->source = g_object_ref(source);
  node->destination = g_object_ref(destination);
  node->info = g_object_ref(info);
  node->parent = parent;
  return node;
}

/* Snapshot the complete tree before modifying it. Breadth-first order gives
 * parent-before-child execution and reverse-order directory finalization.
 * Only one enumerator is open at a time; aliases/cycles are rejected. */
static gboolean
trout_transfer_scan(GPtrArray *nodes, GHashTable *ids, trout_transfer_context *context, GError **error)
{
  guint index;
  GFileEnumerator *enumerator;
  GFileInfo *info;
  GFileInfo *child_info;
  GFile *child;
  GFile *destination;
  trout_transfer_node *node;
  char *id;
  const char *name;
  for (index = 0; index < nodes->len; index += 1) {
    node = g_ptr_array_index(nodes, index);
    if (!trout_transfer_wait(context, error)) return FALSE;
    if (g_file_info_get_file_type(node->info) != G_FILE_TYPE_DIRECTORY) continue;
    if ((context->deleting || context->moving) && (g_file_info_get_attribute_boolean(node->info, "unix::is-mountpoint") ||
        (node->parent != 0 && g_file_info_has_attribute(node->info, G_FILE_ATTRIBUTE_UNIX_DEVICE) &&
        g_file_info_has_attribute(node->parent->info, G_FILE_ATTRIBUTE_UNIX_DEVICE) &&
        g_file_info_get_attribute_uint32(node->info, G_FILE_ATTRIBUTE_UNIX_DEVICE) != g_file_info_get_attribute_uint32(node->parent->info, G_FILE_ATTRIBUTE_UNIX_DEVICE)))) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Refusing to recursively move or delete a mounted directory");
      return FALSE;
    }
    if (!trout_transfer_revalidate(node->source, node->info, context->cancellable, error)) return FALSE;
    enumerator = g_file_enumerate_children(node->source, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, error);
    if (enumerator == 0) return FALSE;
    while (trout_transfer_wait(context, error) && (info = g_file_enumerator_next_file(enumerator, context->cancellable, error)) != 0) {
      name = g_file_info_get_name(info);
      child = g_file_get_child(node->source, name);
      destination = g_file_get_child(node->destination, name);
      g_object_unref(info);
      child_info = trout_transfer_query(child, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, FALSE, error);
      if (child_info != 0) {
        id = trout_transfer_identity(child_info, error);
        if (id != 0) {
          if (g_file_info_get_file_type(child_info) == G_FILE_TYPE_DIRECTORY && g_hash_table_contains(ids, id)) {
            g_free(id);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE, "Repeated directory identity in the source tree");
          } else {
            g_hash_table_add(ids, id);
            g_ptr_array_add(nodes, trout_transfer_node_new(child, destination, child_info, node));
          }
        }
        g_object_unref(child_info);
      }
      g_object_unref(child);
      g_object_unref(destination);
      if (*error != 0) break;
    }
    if (*error == 0) g_file_enumerator_close(enumerator, context->cancellable, error);
    else g_file_enumerator_close(enumerator, 0, 0);
    g_object_unref(enumerator);
    if (*error != 0) return FALSE;
  }
  return TRUE;
}

static gboolean
trout_transfer_move_fallback(GError *error)
{
  return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE);
}

/* A local rename replaces a non-directory entry atomically, including a
 * symlink, and leaves the old destination intact on ordinary failure. */
static gboolean
trout_transfer_native_move(GFile *source, GFile *destination, gboolean replace, trout_transfer_context *context, GError **error)
{
  char *source_path;
  char *destination_path;
  int result;
  int error_number;
  if (replace) {
    source_path = g_file_get_path(source);
    destination_path = g_file_get_path(destination);
    if (source_path != 0 && destination_path != 0) {
      if (g_cancellable_set_error_if_cancelled(context->cancellable, error)) {
        g_free(source_path);
        g_free(destination_path);
        return FALSE;
      }
      result = rename(source_path, destination_path);
      error_number = errno;
      g_free(source_path);
      g_free(destination_path);
      if (result == 0) return TRUE;
      /* EXDEV is the only ordinary local rename failure that permits copy/delete fallback. */
      g_set_error(error, G_IO_ERROR, error_number == EXDEV ? G_IO_ERROR_NOT_SUPPORTED : g_io_error_from_errno(error_number), "Could not replace destination: %s", g_strerror(error_number));
      return FALSE;
    }
    g_free(source_path);
    g_free(destination_path);
  }
  return g_file_move(source, destination, trout_transfer_flags | G_FILE_COPY_NO_FALLBACK_FOR_MOVE | (replace ? G_FILE_COPY_OVERWRITE : 0), context->cancellable, trout_transfer_file_progress, context, error);
}

/* Do not unlink a destination before a successful copy. Publish a completed
 * sibling using a native move; if the backend cannot do that, fail safely. */
static gboolean
trout_transfer_copy_leaf(trout_transfer_node *node, GFileInfo *destination_info, trout_transfer_context *context, GError **error)
{
  GFile *parent;
  GFile *temporary;
  char *uuid;
  char *name;
  gboolean succeeded;
  parent = g_file_get_parent(node->destination);
  if (parent == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Cannot stage a copy at a filesystem root");
    return FALSE;
  }
  uuid = g_uuid_string_random();
  name = g_strdup_printf(".trout-transfer-%s", uuid);
  temporary = g_file_get_child(parent, name);
  g_free(uuid);
  g_free(name);
  g_object_unref(parent);
  succeeded = g_file_copy(node->source, temporary, trout_transfer_flags, context->cancellable, trout_transfer_file_progress, context, error);
  if (!succeeded) {
    if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS)) g_file_delete(temporary, 0, 0);
  } else {
    node->destination_info = trout_transfer_query(temporary, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, FALSE, error);
    succeeded = node->destination_info != 0 && trout_transfer_wait(context, error) &&
        trout_transfer_revalidate(node->source, node->info, context->cancellable, error) &&
        trout_transfer_revalidate_parent(node, context, error) &&
        trout_transfer_revalidate(node->destination, destination_info, context->cancellable, error) &&
        trout_transfer_check_destination(node->destination, destination_info, context, error) &&
        trout_transfer_native_move(temporary, node->destination, destination_info != 0, context, error);
    if (!succeeded) g_file_delete(temporary, 0, 0);
    else succeeded = trout_transfer_revalidate(node->destination, node->destination_info, context->cancellable, error);
  }
  g_object_unref(temporary);
  return succeeded;
}

static gboolean
trout_transfer_make_directory(GFile *destination, GCancellable *cancellable, GError **error)
{
  char *path;
  int result;
  int error_number;
  if (g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
  path = g_file_get_path(destination);
  if (path == 0) return g_file_make_directory(destination, cancellable, error);
  /* Keep newly copied private directories private while populating them. */
  result = mkdir(path, 0700);
  error_number = errno;
  g_free(path);
  if (result == 0) return TRUE;
  g_set_error(error, G_IO_ERROR, g_io_error_from_errno(error_number), "Could not create destination directory: %s", g_strerror(error_number));
  return FALSE;
}

static gboolean
trout_transfer_directory_metadata(trout_transfer_node *node, GCancellable *cancellable, GError **error)
{
  static const char *attributes[] = {
    G_FILE_ATTRIBUTE_TIME_ACCESS, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, "time::access-nsec",
    G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, "time::modified-nsec",
    G_FILE_ATTRIBUTE_UNIX_MODE
  };
  GFileAttributeInfoList *settable;
  GFileInfo *metadata;
  guint index;
  GFileAttributeType type;
  gpointer value;
  gboolean succeeded;
  settable = g_file_query_settable_attributes(node->destination, cancellable, error);
  if (settable == 0) return FALSE;
  metadata = g_file_info_new();
  for (index = 0; index < G_N_ELEMENTS(attributes); index += 1) {
    if (g_file_attribute_info_list_lookup(settable, attributes[index]) != 0 &&
        g_file_info_get_attribute_data(node->info, attributes[index], &type, &value, 0))
      g_file_info_set_attribute(metadata, attributes[index], type, value);
  }
  succeeded = g_file_set_attributes_from_info(node->destination, metadata, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  g_object_unref(metadata);
  g_file_attribute_info_list_unref(settable);
  return succeeded;
}

static gboolean
trout_transfer_entry(trout_transfer_node *node, trout_file_operation operation, trout_transfer_context *context, GError **error)
{
  GFileInfo *destination_info;
  GFileType source_type;
  GFileType destination_type;
  trout_file_collision_choice choice;
  gboolean succeeded = FALSE;
  if (node->parent != 0 && (node->parent->omitted || node->parent->moved)) {
    node->omitted = TRUE;
    return TRUE;
  }
  if (!trout_transfer_wait(context, error) || !trout_transfer_revalidate_parent(node, context, error) ||
      !trout_transfer_revalidate(node->source, node->info, context->cancellable, error)) return FALSE;
  destination_info = trout_transfer_query(node->destination, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, TRUE, error);
  if (*error != 0) return FALSE;
  if (!trout_transfer_check_destination(node->destination, destination_info, context, error)) goto done;
  source_type = g_file_info_get_file_type(node->info);
  destination_type = destination_info != 0 ? g_file_info_get_file_type(destination_info) : G_FILE_TYPE_UNKNOWN;
  if (destination_info != 0) {
    if (!trout_file_transfer_should_merge(source_type, destination_type)) {
      if (context->callbacks == 0 || context->callbacks->collision == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Destination already exists; no collision policy supplied");
        goto done;
      }
      choice = context->callbacks->collision(node->source, node->destination, context->source_root, context->destination_root, source_type, destination_type, g_file_info_get_size(node->info), g_file_info_get_size(destination_info), context->callbacks->user_data);
      if (choice == trout_file_collision_skip) {
        context->skipped = node->skipped = node->omitted = TRUE;
        succeeded = TRUE;
        goto done;
      }
      if (choice == trout_file_collision_cancel) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Transfer cancelled");
        goto done;
      }
      if (choice != trout_file_collision_replace || source_type == G_FILE_TYPE_DIRECTORY || destination_type == G_FILE_TYPE_DIRECTORY) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Refusing to replace a directory with a non-directory or vice versa");
        goto done;
      }
    }
    if (!trout_transfer_wait(context, error) || !trout_transfer_revalidate_parent(node, context, error) ||
        !trout_transfer_revalidate(node->source, node->info, context->cancellable, error) ||
        !trout_transfer_revalidate(node->destination, destination_info, context->cancellable, error) ||
        !trout_transfer_check_destination(node->destination, destination_info, context, error)) goto done;
  }
  if (operation == trout_file_operation_move && (destination_info == 0 || source_type != G_FILE_TYPE_DIRECTORY)) {
    if (trout_transfer_native_move(node->source, node->destination, destination_info != 0, context, error)) {
      context->completed = node->moved = TRUE;
      succeeded = TRUE;
      goto done;
    }
    if (!trout_transfer_move_fallback(*error)) goto done;
    g_clear_error(error);
    if (!trout_transfer_revalidate(node->source, node->info, context->cancellable, error) || !trout_transfer_revalidate(node->destination, destination_info, context->cancellable, error)) goto done;
  }
  if (source_type == G_FILE_TYPE_DIRECTORY) {
    if (destination_info == 0) {
      if (!trout_transfer_make_directory(node->destination, context->cancellable, error)) goto done;
      node->created = TRUE;
      context->completed = TRUE;
      node->destination_info = trout_transfer_query(node->destination, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, FALSE, error);
      if (node->destination_info == 0) goto done;
      if (g_file_info_get_file_type(node->destination_info) != G_FILE_TYPE_DIRECTORY) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "New destination directory changed during creation");
        goto done;
      }
    } else node->destination_info = g_object_ref(destination_info);
    succeeded = TRUE;
  } else if (source_type == G_FILE_TYPE_REGULAR || source_type == G_FILE_TYPE_SYMBOLIC_LINK) {
    succeeded = trout_transfer_copy_leaf(node, destination_info, context, error);
    if (succeeded) {
      context->completed = TRUE;
      if (operation == trout_file_operation_move) succeeded = trout_transfer_wait(context, error) && trout_transfer_revalidate_parent(node, context, error) && trout_transfer_revalidate(node->source, node->info, context->cancellable, error) && trout_transfer_revalidate(node->destination, node->destination_info, context->cancellable, error) && g_file_delete(node->source, context->cancellable, error);
    }
  } else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Copying special filesystem objects is not supported");
done:
  g_clear_object(&destination_info);
  return succeeded;
}

static gboolean
trout_transfer_finish_directory(trout_transfer_node *node, trout_file_operation operation, trout_transfer_context *context, GError **error)
{
  GFileInfo *info;
  gboolean succeeded;
  if (node->skipped && node->parent != 0) node->parent->skipped = TRUE;
  if (node->omitted || node->moved || g_file_info_get_file_type(node->info) != G_FILE_TYPE_DIRECTORY) return TRUE;
  if (!trout_transfer_wait(context, error) || !trout_transfer_revalidate_parent(node, context, error) ||
      !trout_transfer_revalidate(node->source, node->info, context->cancellable, error) ||
      !trout_transfer_revalidate(node->destination, node->destination_info, context->cancellable, error)) return FALSE;
  info = trout_transfer_query(node->destination, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, FALSE, error);
  if (info == 0) return FALSE;
  succeeded = trout_transfer_check_destination(node->destination, info, context, error);
  if (succeeded && g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Destination directory changed during transfer");
    succeeded = FALSE;
  }
  g_object_unref(info);
  if (!succeeded) return FALSE;
  if (node->created && !trout_transfer_directory_metadata(node, context->cancellable, error)) return FALSE;
  if (operation == trout_file_operation_move && !node->skipped) {
    if (!g_file_delete(node->source, context->cancellable, error)) return FALSE;
    context->completed = TRUE;
  } else if (!node->skipped) context->completed = TRUE;
  return TRUE;
}

/* Fast path for a native move to an unused destination. In particular, do not
 * enumerate or recreate a directory merely to rename/move it on one filesystem. */
static gboolean
trout_transfer_native_root(trout_transfer_node *node, trout_transfer_context *context, gboolean *handled, GError **error)
{
  GFileInfo *info;
  *handled = FALSE;
  info = trout_transfer_query(node->destination, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, TRUE, error);
  if (*error != 0) return FALSE;
  if (info != 0) {
    g_object_unref(info);
    return TRUE;
  }
  if (!trout_transfer_wait(context, error) || !trout_transfer_revalidate(node->source, node->info, context->cancellable, error) || !trout_transfer_check_destination(node->destination, 0, context, error)) return FALSE;
  if (trout_transfer_native_move(node->source, node->destination, FALSE, context, error)) {
    *handled = context->completed = TRUE;
    return TRUE;
  }
  if (!trout_transfer_move_fallback(*error)) return FALSE;
  g_clear_error(error);
  return TRUE;
}

/* Validate all selected roots before any mutation, including the reverse
 * containment case (a destination directory contains a selected source). */
static gboolean
trout_transfer_validate_roots(GPtrArray *roots, trout_transfer_context *context, GError **error)
{
  guint index;
  guint other;
  trout_transfer_node *node;
  trout_transfer_node *source;
  GFileInfo *info;
  GHashTable *destination_ids;
  char *id;
  gboolean succeeded;
  for (index = 0; index < roots->len; index += 1) {
    node = g_ptr_array_index(roots, index);
    if (!trout_transfer_check_ancestors(node->source, context->protected_ids, context->cancellable, error)) return FALSE;
    info = trout_transfer_query(node->destination, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, TRUE, error);
    if (*error != 0) return FALSE;
    succeeded = trout_transfer_check_destination(node->destination, info, context, error);
    if (succeeded && info != 0 && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
      destination_ids = trout_transfer_ids_new();
      id = trout_transfer_identity(info, error);
      if (id == 0) succeeded = FALSE;
      else {
        g_hash_table_add(destination_ids, id);
        for (other = 0; succeeded && other < roots->len; other += 1) {
          source = g_ptr_array_index(roots, other);
          succeeded = trout_transfer_check_ancestors(source->source, destination_ids, context->cancellable, error);
        }
      }
      g_hash_table_unref(destination_ids);
    }
    g_clear_object(&info);
    if (!succeeded) return FALSE;
  }
  return TRUE;
}

static gboolean
trout_transfer_run_plan(GPtrArray *nodes, trout_file_operation operation, trout_transfer_context *context, GError **error)
{
  guint index;
  trout_transfer_node *node;
  context->node_count = nodes->len;
  for (index = 0; index < nodes->len; index += 1) {
    node = g_ptr_array_index(nodes, index);
    context->node_index = index;
    context->progress_source = node->source;
    trout_transfer_progress(context, 0.0, FALSE);
    if (!trout_transfer_entry(node, operation, context, error)) return FALSE;
  }
  for (index = nodes->len; index > 0; index -= 1) {
    node = g_ptr_array_index(nodes, index - 1);
    if (!trout_transfer_finish_directory(node, operation, context, error)) return FALSE;
  }
  return TRUE;
}

static gboolean
trout_transfer_run_delete_plan(GPtrArray *nodes, trout_transfer_context *context, GError **error)
{
  guint index;
  trout_transfer_node *node;
  context->node_count = nodes->len;
  for (index = nodes->len; index > 0; index -= 1) {
    node = g_ptr_array_index(nodes, index - 1);
    context->progress_source = node->source;
    context->node_index = nodes->len - index;
    trout_transfer_progress(context, 0.0, FALSE);
    if (!trout_transfer_wait(context, error) || !trout_transfer_revalidate_parent(node, context, error) ||
        !trout_transfer_revalidate(node->source, node->info, context->cancellable, error) ||
        !g_file_delete(node->source, context->cancellable, error)) return FALSE;
    context->completed = TRUE;
  }
  return TRUE;
}

static trout_file_transfer_result
trout_transfer_execute(GPtrArray *operations, trout_file_operation operation, trout_transfer_context *context, GError **error)
{
  GPtrArray *roots;
  GPtrArray *nodes;
  GPtrArray *plans;
  guint child_index;
  trout_file_transfer_pair *pair;
  trout_transfer_node *node;
  GFileInfo *info;
  GFile *renamed;
  char *id;
  guint index;
  gboolean handled;
  gboolean succeeded = TRUE;
  roots = g_ptr_array_new_with_free_func(trout_transfer_node_destroy);
  context->pair_count = operations->len;
  if (operations->len != 0) {
    pair = g_ptr_array_index(operations, 0);
    context->progress_source = pair->source;
    trout_transfer_progress(context, 0.0, FALSE);
  }
  if (operation == trout_file_operation_rename) {
    if (operations->len != 1 || ((trout_file_transfer_pair *)g_ptr_array_index(operations, 0))->display_name == 0) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Rename requires one display-name operation");
      succeeded = FALSE;
    } else if (trout_transfer_wait(context, error)) {
      pair = g_ptr_array_index(operations, 0);
      renamed = g_file_set_display_name(pair->source, pair->display_name, context->cancellable, error);
      succeeded = renamed != 0;
      g_clear_object(&renamed);
    } else succeeded = FALSE;
    goto done;
  }
  context->deleting = operation == trout_file_operation_delete;
  context->moving = operation == trout_file_operation_move;
  if (operation != trout_file_operation_copy && operation != trout_file_operation_move && !context->deleting) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Unknown transfer operation");
    succeeded = FALSE;
    goto done;
  }
  for (index = 0; index < operations->len; index += 1) {
    pair = g_ptr_array_index(operations, index);
    if (!trout_transfer_wait(context, error)) { succeeded = FALSE; goto done; }
    info = trout_transfer_query(pair->source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, FALSE, error);
    if (info == 0) { succeeded = FALSE; goto done; }
    id = trout_transfer_identity(info, error);
    if (id == 0 || g_hash_table_contains(context->protected_ids, id)) {
      if (id != 0) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE, "Selected sources refer to the same filesystem object");
      g_free(id);
      g_object_unref(info);
      succeeded = FALSE;
      goto done;
    }
    g_hash_table_add(context->protected_ids, id);
    g_ptr_array_add(roots, trout_transfer_node_new(pair->source, pair->destination, info, 0));
    g_object_unref(info);
  }
  if (context->deleting) {
    for (index = 0; index < roots->len; index += 1) {
      GFile *parent;
      node = g_ptr_array_index(roots, index);
      parent = g_file_get_parent(node->source);
      if (parent == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Refusing to delete a filesystem root");
        succeeded = FALSE;
        goto done;
      }
      g_object_unref(parent);
      if (!trout_transfer_check_ancestors(node->source, context->protected_ids, context->cancellable, error)) { succeeded = FALSE; goto done; }
    }
  } else if (!trout_transfer_validate_roots(roots, context, error)) { succeeded = FALSE; goto done; }
  /* Keep the single-root native move fast. Multi-root transfers snapshot all
   * source trees first so aliases across different selections are protected. */
  if (roots->len == 1 && operation == trout_file_operation_move) {
    node = g_ptr_array_index(roots, 0);
    context->progress_source = node->source;
    context->node_count = 1;
    if (!trout_transfer_native_root(node, context, &handled, error)) { succeeded = FALSE; goto done; }
    if (handled) {
      trout_transfer_progress(context, 1.0, TRUE);
      goto done;
    }
  }
  /* Build independent flattened plans, but protect every selected source
   * identity before preflighting or executing any of the plans. */
  plans = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
  for (index = 0; succeeded && index < roots->len; index += 1) {
    node = g_ptr_array_index(roots, index);
    nodes = g_ptr_array_new_with_free_func(trout_transfer_node_destroy);
    g_ptr_array_add(nodes, trout_transfer_node_new(node->source, node->destination, node->info, 0));
    g_ptr_array_add(plans, nodes);
    succeeded = trout_transfer_scan(nodes, context->protected_ids, context, error);
  }
  for (index = 0; !context->deleting && succeeded && index < plans->len; index += 1) {
    nodes = g_ptr_array_index(plans, index);
    for (child_index = 0; succeeded && child_index < nodes->len; child_index += 1) {
      node = g_ptr_array_index(nodes, child_index);
      if (!trout_transfer_wait(context, error)) { succeeded = FALSE; break; }
      if (node->parent != 0 && node->parent->preflight_blocked) {
        node->preflight_blocked = TRUE;
        continue;
      }
      info = trout_transfer_query(node->destination, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, context->cancellable, TRUE, error);
      succeeded = *error == 0 && trout_transfer_check_destination(node->destination, info, context, error);
      if (info != 0 && g_file_info_get_file_type(node->info) == G_FILE_TYPE_DIRECTORY && g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) node->preflight_blocked = TRUE;
      g_clear_object(&info);
    }
  }
  for (index = 0; succeeded && index < plans->len; index += 1) {
    nodes = g_ptr_array_index(plans, index);
    node = g_ptr_array_index(nodes, 0);
    context->source_root = node->source;
    context->destination_root = node->destination;
    context->pair_index = index;
    succeeded = context->deleting ? trout_transfer_run_delete_plan(nodes, context, error) : trout_transfer_run_plan(nodes, operation, context, error);
  }
  if (succeeded && roots->len != 0) {
    context->node_index = context->node_count - 1;
    trout_transfer_progress(context, 1.0, TRUE);
  }
  g_ptr_array_unref(plans);
done:
  g_ptr_array_unref(roots);
  if (!succeeded) return g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? trout_file_transfer_cancelled : trout_file_transfer_failed;
  if (context->skipped) return context->completed ? trout_file_transfer_partial : trout_file_transfer_skipped;
  return trout_file_transfer_complete;
}

gboolean
trout_file_transfer_should_merge(GFileType source_type, GFileType destination_type)
{
  return source_type == G_FILE_TYPE_DIRECTORY && destination_type == G_FILE_TYPE_DIRECTORY;
}

static char *
trout_transfer_filesystem_key(GFileInfo *info)
{
  const char *filesystem;
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE))
    return g_strdup_printf("unix:%" G_GUINT32_FORMAT, g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE));
  filesystem = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
  if (filesystem != 0) return g_strdup_printf("gio:%s", filesystem);
  return 0;
}

static GFileInfo *
trout_transfer_filesystem_query(GFile *file)
{
  GError *error = 0;
  GFileInfo *info = g_file_query_info(file, trout_transfer_attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, 0, &error);
  g_clear_error(&error);
  return info;
}

static void
trout_transfer_path_claim_destroy(gpointer user_data)
{
  trout_transfer_path_claim *claim;
  claim = user_data;
  g_free(claim->path);
  g_free(claim);
}

static gboolean
trout_transfer_path_parents_are_plain(trout_file_transfer_access *access, GFile *file)
{
  GFile *ancestor;
  GFile *parent;
  GFileInfo *info;
  gboolean is_plain;
  ancestor = g_file_get_parent(file);
  while (ancestor != 0) {
    info = g_file_query_info(ancestor, "standard::type,unix::is-mountpoint", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, 0, 0);
    parent = g_file_get_parent(ancestor);
    is_plain = info != 0 && g_file_info_get_file_type(info) != G_FILE_TYPE_SYMBOLIC_LINK &&
        g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT);
    if (!is_plain) access->exclusive = TRUE;
    else if (g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT) && parent != 0) is_plain = FALSE;
    g_clear_object(&info);
    g_object_unref(ancestor);
    if (!is_plain) {
      g_clear_object(&parent);
      return FALSE;
    }
    ancestor = parent;
  }
  return TRUE;
}

static char *
trout_transfer_access_path(trout_file_transfer_access *access, GFile *file, GFileInfo *info, gboolean require_existing)
{
  GFile *parent;
  char *path;
  char *parent_path;
  char *canonical_parent;
  char *basename;
  char *result;
  path = g_file_get_path(file);
  if (path == 0) {
    access->paths_complete = FALSE;
    return 0;
  }
  if (info == 0 && require_existing) {
    access->paths_complete = FALSE;
    access->exclusive = TRUE;
  }
  if (info != 0 && !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT)) {
    access->paths_complete = FALSE;
    access->exclusive = TRUE;
  } else if (info != 0 && g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT)) access->paths_complete = FALSE;
  if (!trout_transfer_path_parents_are_plain(access, file)) access->paths_complete = FALSE;
  parent = g_file_get_parent(file);
  if (parent == 0) {
    access->paths_complete = FALSE;
    access->exclusive = TRUE;
    result = g_canonicalize_filename(path, 0);
    g_free(path);
    return result;
  }
  parent_path = g_file_get_path(parent);
  canonical_parent = parent_path != 0 ? realpath(parent_path, 0) : 0;
  if (canonical_parent == 0) {
    access->paths_complete = FALSE;
    access->exclusive = TRUE;
    result = g_canonicalize_filename(path, 0);
  } else {
    basename = g_path_get_basename(path);
    result = g_build_filename(canonical_parent, basename, (char *)0);
    g_free(basename);
    free(canonical_parent);
  }
  g_free(parent_path);
  g_object_unref(parent);
  g_free(path);
  return result;
}

static void
trout_transfer_access_add_path(trout_file_transfer_access *access, GPtrArray *paths, GFile *file,
    GFileInfo *info, gboolean recursive, gboolean require_existing)
{
  trout_transfer_path_claim *claim;
  char *path;
  path = trout_transfer_access_path(access, file, info, require_existing);
  if (path != 0) {
    claim = g_new(trout_transfer_path_claim, 1);
    claim->path = path;
    claim->recursive = recursive;
    g_ptr_array_add(paths, claim);
  }
}

static void
trout_transfer_access_add_staging_directory(trout_file_transfer_access *access, GFile *destination)
{
  GFile *parent;
  GFileInfo *info;
  char *path;
  parent = g_file_get_parent(destination);
  if (parent == 0) {
    access->paths_complete = FALSE;
    return;
  }
  info = trout_transfer_filesystem_query(parent);
  path = trout_transfer_access_path(access, parent, info, TRUE);
  if (path != 0) g_ptr_array_add(access->staging_directories, path);
  g_clear_object(&info);
  g_object_unref(parent);
}

static char *
trout_transfer_parent_filesystem_key(GFile *file)
{
  GFile *candidate;
  GFile *parent;
  GFileInfo *info;
  char *key;
  candidate = g_file_get_parent(file);
  if (candidate == 0) candidate = g_object_ref(file);
  key = 0;
  while (candidate != 0 && key == 0) {
    info = trout_transfer_filesystem_query(candidate);
    if (info != 0) {
      key = trout_transfer_filesystem_key(info);
      g_object_unref(info);
    }
    parent = key == 0 ? g_file_get_parent(candidate) : 0;
    g_object_unref(candidate);
    candidate = parent;
  }
  return key;
}

static void
trout_transfer_access_add(GHashTable *filesystems, char *key, trout_file_transfer_access *access)
{
  if (key != 0) g_hash_table_add(filesystems, key);
  else access->exclusive = TRUE;
}

static void
trout_transfer_access_add_tree(trout_file_transfer_access *access, GFile *file, GHashTable *filesystems, GFileInfo *root_info)
{
  GQueue pending;
  GFileEnumerator *enumerator;
  GFileInfo *info;
  trout_transfer_access_node *node;
  trout_transfer_access_node *child_node;
  GFile *child;
  GError *error;
  char *key;
  const char *name;
  g_queue_init(&pending);
  info = root_info != 0 ? g_object_ref(root_info) : trout_transfer_filesystem_query(file);
  if (info == 0) {
    access->exclusive = TRUE;
    access->paths_complete = FALSE;
    return;
  }
  node = g_new(trout_transfer_access_node, 1);
  node->file = g_object_ref(file);
  node->info = info;
  g_queue_push_tail(&pending, node);
  while ((node = g_queue_pop_head(&pending)) != 0) {
    key = trout_transfer_filesystem_key(node->info);
    trout_transfer_access_add(filesystems, key, access);
    if (g_file_info_get_file_type(node->info) == G_FILE_TYPE_DIRECTORY) {
      if (!g_file_info_has_attribute(node->info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT)) {
        access->paths_complete = FALSE;
        access->exclusive = TRUE;
      } else if (g_file_info_get_attribute_boolean(node->info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT)) access->paths_complete = FALSE;
      error = 0;
      enumerator = g_file_enumerate_children(node->file, trout_transfer_attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, 0, &error);
      if (enumerator == 0) {
        access->exclusive = TRUE;
        access->paths_complete = FALSE;
        g_clear_error(&error);
      } else {
        while ((info = g_file_enumerator_next_file(enumerator, 0, &error)) != 0) {
          if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            name = g_file_info_get_name(info);
            child = g_file_get_child(node->file, name);
            child_node = g_new(trout_transfer_access_node, 1);
            child_node->file = child;
            child_node->info = info;
            g_queue_push_tail(&pending, child_node);
          } else {
            key = trout_transfer_filesystem_key(info);
            trout_transfer_access_add(filesystems, key, access);
            g_object_unref(info);
          }
        }
        if (error != 0) {
          access->exclusive = TRUE;
          access->paths_complete = FALSE;
        }
        g_clear_error(&error);
        if (!g_file_enumerator_close(enumerator, 0, &error)) {
          access->exclusive = TRUE;
          access->paths_complete = FALSE;
        }
        g_clear_error(&error);
        g_object_unref(enumerator);
      }
    }
    g_object_unref(node->file);
    g_object_unref(node->info);
    g_free(node);
  }
}

trout_file_transfer_access *
trout_file_transfer_access_new(GPtrArray *operations, trout_file_operation operation)
{
  trout_file_transfer_access *access;
  trout_file_transfer_pair *pair;
  GFileInfo *source_info;
  GFileInfo *destination_info;
  GFileType source_type;
  GFileType destination_type;
  char *destination_key;
  GHashTableIter iterator;
  gpointer key;
  guint index;
  access = g_new0(trout_file_transfer_access, 1);
  access->read_filesystems = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);
  access->write_filesystems = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);
  access->read_paths = g_ptr_array_new_with_free_func(trout_transfer_path_claim_destroy);
  access->write_paths = g_ptr_array_new_with_free_func(trout_transfer_path_claim_destroy);
  access->rename_paths = g_ptr_array_new_with_free_func(trout_transfer_path_claim_destroy);
  access->staging_directories = g_ptr_array_new_with_free_func(g_free);
  access->operation = operation;
  access->paths_complete = TRUE;
  for (index = 0; index < operations->len; index += 1) {
    pair = g_ptr_array_index(operations, index);
    if (operation == trout_file_operation_delete) {
      source_info = trout_transfer_filesystem_query(pair->source);
      trout_transfer_access_add_tree(access, pair->source, access->write_filesystems, 0);
      source_type = source_info != 0 ? g_file_info_get_file_type(source_info) : G_FILE_TYPE_UNKNOWN;
      trout_transfer_access_add_path(access, access->write_paths, pair->source, source_info,
          source_type == G_FILE_TYPE_DIRECTORY, TRUE);
      g_clear_object(&source_info);
      continue;
    }
    if (operation == trout_file_operation_rename) {
      source_info = trout_transfer_filesystem_query(pair->source);
      destination_info = trout_transfer_filesystem_query(pair->destination);
      destination_key = trout_transfer_parent_filesystem_key(pair->source);
      trout_transfer_access_add(access->write_filesystems, destination_key, access);
      destination_key = trout_transfer_parent_filesystem_key(pair->destination);
      trout_transfer_access_add(access->write_filesystems, destination_key, access);
      source_type = source_info != 0 ? g_file_info_get_file_type(source_info) : G_FILE_TYPE_UNKNOWN;
      trout_transfer_access_add_path(access, access->rename_paths, pair->source, source_info,
          source_type == G_FILE_TYPE_DIRECTORY, TRUE);
      trout_transfer_access_add_path(access, access->rename_paths, pair->destination, destination_info,
          source_type == G_FILE_TYPE_DIRECTORY, FALSE);
      g_clear_object(&source_info);
      g_clear_object(&destination_info);
      continue;
    }
    source_info = trout_transfer_filesystem_query(pair->source);
    source_type = source_info != 0 ? g_file_info_get_file_type(source_info) : G_FILE_TYPE_UNKNOWN;
    destination_info = trout_transfer_filesystem_query(pair->destination);
    destination_type = destination_info != 0 ? g_file_info_get_file_type(destination_info) : G_FILE_TYPE_UNKNOWN;
    trout_transfer_access_add_path(access, access->read_paths, pair->source, source_info,
        source_type == G_FILE_TYPE_DIRECTORY, TRUE);
    trout_transfer_access_add_path(access, access->write_paths, pair->destination, destination_info,
        source_type == G_FILE_TYPE_DIRECTORY || destination_type == G_FILE_TYPE_DIRECTORY, FALSE);
    if (source_type != G_FILE_TYPE_DIRECTORY) trout_transfer_access_add_staging_directory(access, pair->destination);
    trout_transfer_access_add_tree(access, pair->source, access->read_filesystems, source_info);
    if (trout_file_transfer_should_merge(source_type, destination_type))
      trout_transfer_access_add_tree(access, pair->destination, access->write_filesystems, destination_info);
    else {
      destination_key = trout_transfer_parent_filesystem_key(pair->destination);
      trout_transfer_access_add(access->write_filesystems, destination_key, access);
    }
    g_clear_object(&source_info);
    g_clear_object(&destination_info);
  }
  g_hash_table_iter_init(&iterator, access->read_filesystems);
  while (g_hash_table_iter_next(&iterator, &key, 0)) {
    if (g_hash_table_contains(access->write_filesystems, key)) g_hash_table_iter_remove(&iterator);
  }
  return access;
}

void
trout_file_transfer_access_destroy(trout_file_transfer_access *access)
{
  g_hash_table_unref(access->read_filesystems);
  g_hash_table_unref(access->write_filesystems);
  g_ptr_array_unref(access->read_paths);
  g_ptr_array_unref(access->write_paths);
  g_ptr_array_unref(access->rename_paths);
  g_ptr_array_unref(access->staging_directories);
  g_free(access);
}

static gboolean
trout_transfer_access_intersects(GHashTable *first, GHashTable *second)
{
  GHashTableIter iterator;
  gpointer key;
  g_hash_table_iter_init(&iterator, first);
  while (g_hash_table_iter_next(&iterator, &key, 0)) {
    if (g_hash_table_contains(second, key)) return TRUE;
  }
  return FALSE;
}

static gboolean
trout_transfer_path_is_within(const char *path, const char *directory)
{
  gsize directory_length;
  if (g_str_equal(path, directory)) return TRUE;
  if (g_str_equal(directory, "/")) return path[0] == '/';
  directory_length = strlen(directory);
  return g_str_has_prefix(path, directory) && path[directory_length] == '/';
}

static gboolean
trout_transfer_path_claims_conflict(trout_transfer_path_claim *first, trout_transfer_path_claim *second)
{
  return g_str_equal(first->path, second->path) ||
      (first->recursive && trout_transfer_path_is_within(second->path, first->path)) ||
      (second->recursive && trout_transfer_path_is_within(first->path, second->path));
}

static gboolean
trout_transfer_path_lists_conflict(GPtrArray *first, GPtrArray *second)
{
  trout_transfer_path_claim *first_claim;
  trout_transfer_path_claim *second_claim;
  guint first_index;
  guint second_index;
  for (first_index = 0; first_index < first->len; first_index += 1) {
    first_claim = g_ptr_array_index(first, first_index);
    for (second_index = 0; second_index < second->len; second_index += 1) {
      second_claim = g_ptr_array_index(second, second_index);
      if (trout_transfer_path_claims_conflict(first_claim, second_claim)) return TRUE;
    }
  }
  return FALSE;
}

static gboolean
trout_transfer_rename_conflicts_with_access(trout_file_transfer_access *rename_access, trout_file_transfer_access *other_access)
{
  trout_transfer_path_claim *rename_claim;
  char *parent;
  char *basename;
  guint index;
  if (trout_transfer_path_lists_conflict(rename_access->rename_paths, other_access->read_paths) ||
      trout_transfer_path_lists_conflict(rename_access->rename_paths, other_access->write_paths)) return TRUE;
  for (index = 0; index < rename_access->rename_paths->len; index += 1) {
    rename_claim = g_ptr_array_index(rename_access->rename_paths, index);
    parent = g_path_get_dirname(rename_claim->path);
    basename = g_path_get_basename(rename_claim->path);
    if (g_str_has_prefix(basename, ".trout-transfer-")) {
      guint staging_index;
      for (staging_index = 0; staging_index < other_access->staging_directories->len; staging_index += 1) {
        if (g_str_equal(parent, g_ptr_array_index(other_access->staging_directories, staging_index))) {
          g_free(parent);
          g_free(basename);
          return TRUE;
        }
      }
    }
    g_free(parent);
    g_free(basename);
  }
  return FALSE;
}

static gboolean
trout_transfer_access_path_conflicts(trout_file_transfer_access *first, trout_file_transfer_access *second)
{
  if (first->operation == trout_file_operation_rename && second->operation == trout_file_operation_rename)
    return trout_transfer_path_lists_conflict(first->rename_paths, second->rename_paths);
  if (first->operation == trout_file_operation_rename)
    return trout_transfer_rename_conflicts_with_access(first, second);
  return trout_transfer_rename_conflicts_with_access(second, first);
}

static gboolean
trout_transfer_access_filesystem_conflicts(trout_file_transfer_access *first, trout_file_transfer_access *second)
{
  if (first->exclusive || second->exclusive) return TRUE;
  return trout_transfer_access_intersects(first->read_filesystems, second->write_filesystems) ||
      trout_transfer_access_intersects(first->write_filesystems, second->read_filesystems) ||
      trout_transfer_access_intersects(first->write_filesystems, second->write_filesystems);
}

gboolean
trout_file_transfer_access_conflicts(trout_file_transfer_access *first, trout_file_transfer_access *second)
{
  if (first->operation == trout_file_operation_rename || second->operation == trout_file_operation_rename) {
    if (first->paths_complete && second->paths_complete) return trout_transfer_access_path_conflicts(first, second);
  }
  return trout_transfer_access_filesystem_conflicts(first, second);
}

trout_file_transfer_pair *
trout_file_transfer_pair_new(GFile *source, GFile *destination)
{
  trout_file_transfer_pair *pair = g_new0(trout_file_transfer_pair, 1);
  pair->source = g_object_ref(source);
  pair->destination = g_object_ref(destination);
  return pair;
}

trout_file_transfer_pair *
trout_file_transfer_pair_new_rename(GFile *source, const char *display_name, GError **error)
{
  GFile *parent;
  GFile *destination;
  trout_file_transfer_pair *pair = 0;
  if (display_name == 0 || display_name[0] == '\0' || strchr(display_name, '/') != 0 ||
      strcmp(display_name, ".") == 0 || strcmp(display_name, "..") == 0 || !g_utf8_validate(display_name, -1, 0)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid display name");
    return 0;
  }
  parent = g_file_get_parent(source);
  if (parent == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Cannot rename a filesystem root");
    return 0;
  }
  destination = g_file_get_child_for_display_name(parent, display_name, error);
  if (destination != 0) {
    pair = trout_file_transfer_pair_new(source, destination);
    pair->display_name = g_strdup(display_name);
    g_object_unref(destination);
  }
  g_object_unref(parent);
  return pair;
}

GFile *
trout_file_transfer_pair_get_source(trout_file_transfer_pair *pair)
{
  return pair->source;
}

GFile *
trout_file_transfer_pair_get_destination(trout_file_transfer_pair *pair)
{
  return pair->destination;
}

void
trout_file_transfer_pair_destroy(gpointer data)
{
  trout_file_transfer_pair *pair = data;
  g_object_unref(pair->source);
  g_object_unref(pair->destination);
  g_free(pair->display_name);
  g_free(pair);
}

trout_file_transfer_result
trout_file_transfer_execute_full(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error)
{
  trout_transfer_context context = {0};
  trout_file_transfer_result result;
  GError *local_error = 0;
  context.callbacks = callbacks;
  context.cancellable = cancellable != 0 ? g_object_ref(cancellable) : g_cancellable_new();
  context.protected_ids = trout_transfer_ids_new();
  result = trout_transfer_execute(operations, operation, &context, &local_error);
  if (local_error == 0 && (result == trout_file_transfer_failed || result == trout_file_transfer_cancelled))
    g_set_error_literal(&local_error, G_IO_ERROR, result == trout_file_transfer_cancelled ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED, "Transfer did not complete");
  if (local_error != 0) g_propagate_error(error, local_error);
  g_hash_table_unref(context.protected_ids);
  g_object_unref(context.cancellable);
  return result;
}

gboolean
trout_file_transfer_execute(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error)
{
  trout_file_transfer_result result = trout_file_transfer_execute_full(operations, operation, cancellable, callbacks, error);
  if (result == trout_file_transfer_partial || result == trout_file_transfer_skipped) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Transfer did not complete because entries were skipped");
  return result == trout_file_transfer_complete;
}
