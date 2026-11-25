#ifndef trout_thumbnailer_h_included
#define trout_thumbnailer_h_included

#include <gtk/gtk.h>

#define trout_status_group 1
#define trout_status_success 0
#define trout_status_error 1
#define trout_thumbnail_max_size 1024

typedef int status_t;
typedef struct trout_thumbnailer trout_thumbnailer;

status_t trout_thumbnailer_create(trout_thumbnailer **thumbnailer_out);
status_t trout_thumbnailer_stop(trout_thumbnailer *thumbnailer);
void trout_thumbnailer_destroy(gpointer data);
status_t trout_thumbnailer_schedule_file_info(trout_thumbnailer *thumbnailer, GFileInfo *file_info);
status_t trout_thumbnailer_schedule_file_named(trout_thumbnailer *thumbnailer, GFile *directory, char *name);
status_t trout_thumbnailer_schedule_directory(trout_thumbnailer *thumbnailer, GFile *directory);
gboolean trout_thumbnailer_file_name_is_thumbnailable(trout_thumbnailer *thumbnailer, char *name);
status_t trout_thumbnailer_bind_picture(trout_thumbnailer *thumbnailer, GFile *file, char *name, GtkPicture *picture);
void trout_thumbnailer_unbind_picture(trout_thumbnailer *thumbnailer, GtkPicture *picture);

#endif
