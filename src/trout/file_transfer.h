#ifndef trout_file_transfer_h_included
#define trout_file_transfer_h_included

#include <gio/gio.h>

typedef enum {
  trout_file_operation_copy = 1,
  trout_file_operation_move = 2,
  trout_file_operation_rename = 3,
  trout_file_operation_delete = 4
} trout_file_operation;

typedef enum {
  trout_file_collision_skip = 1,
  trout_file_collision_replace = 2,
  trout_file_collision_cancel = 3
} trout_file_collision_choice;

typedef enum {
  trout_file_transfer_complete = 1,
  trout_file_transfer_partial = 2,
  trout_file_transfer_skipped = 3,
  trout_file_transfer_cancelled = 4,
  trout_file_transfer_failed = 5
} trout_file_transfer_result;

typedef struct trout_file_transfer_pair trout_file_transfer_pair;
typedef struct trout_file_transfer_callbacks trout_file_transfer_callbacks;
typedef struct trout_file_transfer_access trout_file_transfer_access;

struct trout_file_transfer_access {
  GHashTable *read_filesystems;
  GHashTable *write_filesystems;
  GPtrArray *read_paths;
  GPtrArray *write_paths;
  GPtrArray *rename_paths;
  GPtrArray *staging_directories;
  trout_file_operation operation;
  gboolean paths_complete;
  gboolean exclusive;
};

struct trout_file_transfer_callbacks {
  gpointer user_data;
  gboolean (*wait)(gpointer user_data);
  trout_file_collision_choice (*collision)(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, GFileType source_type, GFileType destination_type, goffset source_size, goffset destination_size, gpointer user_data);
  /* Monotonic, request-wide work units, not bytes. Both arguments use the same scale. */
  void (*progress)(GFile *source, goffset current, goffset total, gpointer user_data);
};

/* A delete pair has source == destination; only source is used. */
trout_file_transfer_pair *trout_file_transfer_pair_new(GFile *source, GFile *destination);
trout_file_transfer_pair *trout_file_transfer_pair_new_rename(GFile *source, const char *display_name, GError **error);
GFile *trout_file_transfer_pair_get_source(trout_file_transfer_pair *pair);
GFile *trout_file_transfer_pair_get_destination(trout_file_transfer_pair *pair);
void trout_file_transfer_pair_destroy(gpointer user_data);
trout_file_transfer_access *trout_file_transfer_access_new(GPtrArray *operations, trout_file_operation operation);
void trout_file_transfer_access_destroy(trout_file_transfer_access *access);
gboolean trout_file_transfer_access_conflicts(trout_file_transfer_access *first, trout_file_transfer_access *second);
/* Failed and cancelled operations set error. Earlier committed entries are not rolled back. */
trout_file_transfer_result trout_file_transfer_execute_full(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error);
/* Compatibility wrapper: TRUE only for a complete transfer. */
gboolean trout_file_transfer_execute(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error);
gboolean trout_file_transfer_should_merge(GFileType source_type, GFileType destination_type);

#endif
