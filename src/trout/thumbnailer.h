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
status_t trout_thumbnailer_schedule_file(trout_thumbnailer *thumbnailer, GFile *file);
status_t trout_thumbnailer_schedule_directory(trout_thumbnailer *thumbnailer, GFile *directory);
status_t trout_thumbnailer_bind_picture(trout_thumbnailer *thumbnailer, GFile *file, GtkPicture *picture);
void trout_thumbnailer_unbind_picture(trout_thumbnailer *thumbnailer, GtkPicture *picture);

#endif
