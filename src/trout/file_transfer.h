#ifndef trout_file_transfer_h_included
#define trout_file_transfer_h_included

#include <gio/gio.h>

typedef enum {
  trout_file_operation_copy = 1,
  trout_file_operation_move = 2
} trout_file_operation;

typedef enum {
  trout_file_collision_skip = 1,
  trout_file_collision_replace = 2,
  trout_file_collision_cancel = 3
} trout_file_collision_choice;

typedef struct trout_file_transfer_pair trout_file_transfer_pair;

typedef struct trout_file_transfer_callbacks trout_file_transfer_callbacks;

struct trout_file_transfer_callbacks {
  gpointer user_data;
  gboolean (*wait)(gpointer user_data);
  trout_file_collision_choice (*collision)(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, GFileType source_type, GFileType destination_type, goffset source_size, goffset destination_size, gpointer user_data);
  void (*progress)(GFile *source, goffset current_bytes, goffset total_bytes, gpointer user_data);
};

trout_file_transfer_pair *trout_file_transfer_pair_new(GFile *source, GFile *destination);
void trout_file_transfer_pair_destroy(gpointer user_data);
gboolean trout_file_transfer_execute(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, const trout_file_transfer_callbacks *callbacks, GError **error);
gboolean trout_file_transfer_should_merge(GFileType source_type, GFileType destination_type);

#endif
