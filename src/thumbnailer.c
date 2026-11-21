#include "thumbnailer.h"

#include <errno.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <libopenraw-gnome/gdkpixbuf.h>
#include <libopenraw/rawfile.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <trout/sph/queue.h>
#include <trout/sph/thread-pool.c>

#define trout_thumbnail_kind_image 1
#define trout_thumbnail_kind_raw 2
#define trout_thumbnail_kind_video 3
#define trout_thumbnail_task_scan 4
#define trout_thumbnail_jpeg_quality 11
#define trout_thumbnail_update_batch_size 100
#define trout_thumbnail_update_delay_milliseconds 2000

typedef struct trout_thumbnail_task trout_thumbnail_task;
typedef struct trout_thumbnail_update trout_thumbnail_update;

struct trout_thumbnailer {
  sph_thread_pool_t pool;
  GMutex mutex;
  GHashTable *pending_files;
  GHashTable *pending_directories;
  GHashTable *pictures;
  GQueue updates;
  guint update_source_id;
  char *cache_directory;
  char **raw_extensions;
  gboolean stopping;
  gboolean pool_initialized;
};

struct trout_thumbnail_task {
  sph_thread_pool_task_t pool_task;
  trout_thumbnailer *thumbnailer;
  GFile *file;
  char *uri;
  guint kind;
};

struct trout_thumbnail_update {
  char *uri;
  GdkTexture *texture;
};

char *trout_thumbnailer_video_extensions[] = {
  "mp4", "wmv", "mkv", "avi", "mov", "webm", "m4v", "mpg", "mpeg", "ogv", "flv", "3gp", "ts", "m2ts", 0
};

void trout_thumbnailer_task_run(sph_thread_pool_task_t *pool_task);
void trout_thumbnailer_task_destroy(trout_thumbnail_task *task);
void trout_thumbnailer_scan_directory(trout_thumbnail_task *task);
status_t trout_thumbnailer_process_file(trout_thumbnail_task *task);
status_t trout_thumbnailer_generate_file(trout_thumbnail_task *task, char *path, GFileInfo *file_info, char *cache_path);
status_t trout_thumbnailer_generate_video(char *path, GdkPixbuf **pixbuf_out);
status_t trout_thumbnailer_new_temp_path(trout_thumbnailer *thumbnailer, char **path_out);
status_t trout_thumbnailer_save_pixbuf(trout_thumbnailer *thumbnailer, GdkPixbuf *pixbuf, char *cache_path, char *uri, GFileInfo *file_info);
GdkPixbuf *trout_thumbnailer_scale_pixbuf(GdkPixbuf *pixbuf);
gboolean trout_thumbnailer_cache_valid(char *cache_path, guint64 modified, guint32 modified_usec);
guint trout_thumbnailer_file_kind(trout_thumbnailer *thumbnailer, char *path);
gboolean trout_thumbnailer_has_pictures(trout_thumbnailer *thumbnailer, char *uri);
void trout_thumbnailer_publish_texture(trout_thumbnailer *thumbnailer, char *uri, GdkTexture *texture);
gboolean trout_thumbnailer_deliver_updates(gpointer user_data);
void trout_thumbnailer_update_destroy(trout_thumbnail_update *update);

status_t
trout_thumbnailer_create(trout_thumbnailer **thumbnailer_out)
{
  trout_thumbnailer *thumbnailer;
  guint worker_count;
  int error;
  *thumbnailer_out = 0;
  thumbnailer = g_new0(trout_thumbnailer, 1);
  g_mutex_init(&thumbnailer->mutex);
  thumbnailer->pending_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);
  thumbnailer->pending_directories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);
  thumbnailer->pictures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
  g_queue_init(&thumbnailer->updates);
  thumbnailer->cache_directory = g_build_filename(g_get_user_cache_dir(), "trout", "thumbnails", (char *)0);
  if (g_mkdir_with_parents(thumbnailer->cache_directory, 0700) != 0) {
    g_warning("Could not create thumbnail cache directory %s: %s", thumbnailer->cache_directory, g_strerror(errno));
    trout_thumbnailer_destroy(thumbnailer);
    return trout_status_error;
  }
  thumbnailer->raw_extensions = g_strdupv((char **)or_get_file_extensions());
  worker_count = g_get_num_processors();
  if (worker_count > 2) worker_count -= 2;
  else worker_count = 1;
  if (worker_count > sph_thread_pool_thread_limit) worker_count = sph_thread_pool_thread_limit;
  error = sph_thread_pool_init(worker_count, &thumbnailer->pool);
  if (error != 0) {
    g_warning("Could not start thumbnail workers: %s", g_strerror(error));
    trout_thumbnailer_destroy(thumbnailer);
    return trout_status_error;
  }
  thumbnailer->pool_initialized = TRUE;
  *thumbnailer_out = thumbnailer;
  return trout_status_success;
}

status_t
trout_thumbnailer_stop(trout_thumbnailer *thumbnailer)
{
  int error;
  g_mutex_lock(&thumbnailer->mutex);
  thumbnailer->stopping = TRUE;
  g_mutex_unlock(&thumbnailer->mutex);
  if (!thumbnailer->pool_initialized) return trout_status_success;
  error = sph_thread_pool_finish(&thumbnailer->pool, 0, 0);
  if (error != 0) {
    g_warning("Could not stop thumbnail workers: %s", g_strerror(error));
    return trout_status_error;
  }
  thumbnailer->pool_initialized = FALSE;
  return trout_status_success;
}

void
trout_thumbnailer_destroy(gpointer data)
{
  trout_thumbnailer *thumbnailer;
  trout_thumbnail_update *update;
  thumbnailer = data;
  if (thumbnailer->pool_initialized) trout_thumbnailer_stop(thumbnailer);
  if (thumbnailer->update_source_id != 0) g_source_remove(thumbnailer->update_source_id);
  while (!g_queue_is_empty(&thumbnailer->updates)) {
    update = g_queue_pop_head(&thumbnailer->updates);
    trout_thumbnailer_update_destroy(update);
  }
  g_hash_table_unref(thumbnailer->pending_files);
  g_hash_table_unref(thumbnailer->pending_directories);
  g_hash_table_unref(thumbnailer->pictures);
  g_strfreev(thumbnailer->raw_extensions);
  g_free(thumbnailer->cache_directory);
  g_mutex_clear(&thumbnailer->mutex);
  g_free(thumbnailer);
}

status_t
trout_thumbnailer_schedule_file(trout_thumbnailer *thumbnailer, GFile *file)
{
  trout_thumbnail_task *task;
  char *path;
  char *uri;
  guint kind;
  gboolean stopping;
  int error;
  path = g_file_get_path(file);
  if (path == 0) return trout_status_success;
  kind = trout_thumbnailer_file_kind(thumbnailer, path);
  g_free(path);
  if (kind == 0) return trout_status_success;
  uri = g_file_get_uri(file);
  g_mutex_lock(&thumbnailer->mutex);
  if (thumbnailer->stopping || g_hash_table_contains(thumbnailer->pending_files, uri)) {
    stopping = thumbnailer->stopping;
    g_mutex_unlock(&thumbnailer->mutex);
    g_free(uri);
    return stopping ? trout_status_error : trout_status_success;
  }
  g_hash_table_add(thumbnailer->pending_files, g_strdup(uri));
  g_mutex_unlock(&thumbnailer->mutex);
  task = g_new0(trout_thumbnail_task, 1);
  task->thumbnailer = thumbnailer;
  task->file = g_object_ref(file);
  task->uri = uri;
  task->kind = kind;
  task->pool_task.f = trout_thumbnailer_task_run;
  error = sph_thread_pool_enqueue(&thumbnailer->pool, &task->pool_task);
  if (error != 0) {
    g_mutex_lock(&thumbnailer->mutex);
    g_hash_table_remove(thumbnailer->pending_files, uri);
    g_mutex_unlock(&thumbnailer->mutex);
    trout_thumbnailer_task_destroy(task);
    return trout_status_error;
  }
  return trout_status_success;
}

status_t
trout_thumbnailer_schedule_directory(trout_thumbnailer *thumbnailer, GFile *directory)
{
  trout_thumbnail_task *task;
  char *uri;
  gboolean stopping;
  int error;
  uri = g_file_get_uri(directory);
  g_mutex_lock(&thumbnailer->mutex);
  if (thumbnailer->stopping || g_hash_table_contains(thumbnailer->pending_directories, uri)) {
    stopping = thumbnailer->stopping;
    g_mutex_unlock(&thumbnailer->mutex);
    g_free(uri);
    return stopping ? trout_status_error : trout_status_success;
  }
  g_hash_table_add(thumbnailer->pending_directories, g_strdup(uri));
  g_mutex_unlock(&thumbnailer->mutex);
  task = g_new0(trout_thumbnail_task, 1);
  task->thumbnailer = thumbnailer;
  task->file = g_object_ref(directory);
  task->uri = uri;
  task->kind = trout_thumbnail_task_scan;
  task->pool_task.f = trout_thumbnailer_task_run;
  error = sph_thread_pool_enqueue(&thumbnailer->pool, &task->pool_task);
  if (error != 0) {
    g_mutex_lock(&thumbnailer->mutex);
    g_hash_table_remove(thumbnailer->pending_directories, uri);
    g_mutex_unlock(&thumbnailer->mutex);
    trout_thumbnailer_task_destroy(task);
    return trout_status_error;
  }
  return trout_status_success;
}

status_t
trout_thumbnailer_bind_picture(trout_thumbnailer *thumbnailer, GFile *file, GtkPicture *picture)
{
  GPtrArray *pictures;
  char *uri;
  uri = g_file_get_uri(file);
  g_mutex_lock(&thumbnailer->mutex);
  pictures = g_hash_table_lookup(thumbnailer->pictures, uri);
  if (pictures == 0) {
    pictures = g_ptr_array_new();
    g_hash_table_insert(thumbnailer->pictures, g_strdup(uri), pictures);
  }
  g_ptr_array_add(pictures, picture);
  g_mutex_unlock(&thumbnailer->mutex);
  g_object_set_data_full(G_OBJECT(picture), "trout_thumbnail_uri", g_strdup(uri), g_free);
  if (trout_thumbnailer_schedule_file(thumbnailer, file) != trout_status_success) {
    trout_thumbnailer_unbind_picture(thumbnailer, picture);
    g_free(uri);
    return trout_status_error;
  }
  g_free(uri);
  return trout_status_success;
}

void
trout_thumbnailer_unbind_picture(trout_thumbnailer *thumbnailer, GtkPicture *picture)
{
  GPtrArray *pictures;
  char *uri;
  uri = g_object_get_data(G_OBJECT(picture), "trout_thumbnail_uri");
  g_mutex_lock(&thumbnailer->mutex);
  pictures = g_hash_table_lookup(thumbnailer->pictures, uri);
  g_ptr_array_remove_fast(pictures, picture);
  if (pictures->len == 0) g_hash_table_remove(thumbnailer->pictures, uri);
  g_mutex_unlock(&thumbnailer->mutex);
  g_object_set_data(G_OBJECT(picture), "trout_thumbnail_uri", 0);
  gtk_picture_set_paintable(picture, 0);
}

void
trout_thumbnailer_task_run(sph_thread_pool_task_t *pool_task)
{
  trout_thumbnail_task *task;
  gboolean stopping;
  task = (trout_thumbnail_task *)pool_task;
  g_mutex_lock(&task->thumbnailer->mutex);
  stopping = task->thumbnailer->stopping;
  g_mutex_unlock(&task->thumbnailer->mutex);
  if (!stopping) {
    if (task->kind == trout_thumbnail_task_scan) trout_thumbnailer_scan_directory(task);
    else trout_thumbnailer_process_file(task);
  }
  if (task->kind != trout_thumbnail_task_scan) {
    g_mutex_lock(&task->thumbnailer->mutex);
    g_hash_table_remove(task->thumbnailer->pending_files, task->uri);
    g_mutex_unlock(&task->thumbnailer->mutex);
  } else {
    g_mutex_lock(&task->thumbnailer->mutex);
    g_hash_table_remove(task->thumbnailer->pending_directories, task->uri);
    g_mutex_unlock(&task->thumbnailer->mutex);
  }
  trout_thumbnailer_task_destroy(task);
}

void
trout_thumbnailer_task_destroy(trout_thumbnail_task *task)
{
  g_object_unref(task->file);
  g_free(task->uri);
  g_free(task);
}

void
trout_thumbnailer_scan_directory(trout_thumbnail_task *task)
{
  GFileEnumerator *enumerator;
  GFileInfo *file_info;
  GFile *file;
  GError *error;
  char *name;
  gboolean stopping;
  error = 0;
  enumerator = g_file_enumerate_children(task->file,
      "standard::name,standard::type",
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, 0, &error);
  if (enumerator == 0) {
    if (error != 0) {
      g_debug("Could not enumerate thumbnail directory %s: %s", task->uri, error->message);
      g_error_free(error);
    }
    return;
  }
  while (1) {
    g_mutex_lock(&task->thumbnailer->mutex);
    stopping = task->thumbnailer->stopping;
    g_mutex_unlock(&task->thumbnailer->mutex);
    if (stopping) break;
    file_info = g_file_enumerator_next_file(enumerator, 0, &error);
    if (file_info == 0) break;
    if (g_file_info_get_file_type(file_info) == G_FILE_TYPE_REGULAR ||
        g_file_info_get_file_type(file_info) == G_FILE_TYPE_SYMBOLIC_LINK) {
      name = (char *)g_file_info_get_name(file_info);
      file = g_file_get_child(task->file, name);
      if (trout_thumbnailer_schedule_file(task->thumbnailer, file) != trout_status_success) {
        g_debug("Could not queue thumbnail for %s", name);
      }
      g_object_unref(file);
    }
    g_object_unref(file_info);
  }
  if (error != 0) {
    g_debug("Could not finish enumerating thumbnail directory %s: %s", task->uri, error->message);
    g_error_free(error);
  }
  g_file_enumerator_close(enumerator, 0, 0);
  g_object_unref(enumerator);
}

status_t
trout_thumbnailer_process_file(trout_thumbnail_task *task)
{
  GFileInfo *file_info;
  GdkTexture *texture;
  GError *error;
  char *path;
  char *cache_path;
  char *checksum;
  guint64 modified;
  guint32 modified_usec;
  gboolean cache_is_valid;
  gboolean has_pictures;
  status_t status;
  error = 0;
  texture = 0;
  path = 0;
  cache_path = 0;
  checksum = 0;
  status = trout_status_success;
  file_info = g_file_query_info(task->file,
      "standard::type,time::modified,time::modified-usec",
      G_FILE_QUERY_INFO_NONE, 0, &error);
  if (file_info == 0) {
    if (error != 0) {
      g_debug("Could not inspect thumbnail source %s: %s", task->uri, error->message);
      g_error_free(error);
    }
    return trout_status_error;
  }
  if (g_file_info_get_file_type(file_info) != G_FILE_TYPE_REGULAR) {
    g_object_unref(file_info);
    return trout_status_success;
  }
  path = g_file_get_path(task->file);
  if (path == 0) {
    g_object_unref(file_info);
    return trout_status_error;
  }
  checksum = g_compute_checksum_for_string(G_CHECKSUM_MD5, task->uri, -1);
  cache_path = g_strconcat(task->thumbnailer->cache_directory, "/", checksum, ".jpg", (char *)0);
  g_free(checksum);
  modified = g_file_info_get_attribute_uint64(file_info, "time::modified");
  modified_usec = g_file_info_get_attribute_uint32(file_info, "time::modified-usec");
  cache_is_valid = trout_thumbnailer_cache_valid(cache_path, modified, modified_usec);
  if (!cache_is_valid) {
    status = trout_thumbnailer_generate_file(task, path, file_info, cache_path);
  }
  if (status == trout_status_success) {
    has_pictures = trout_thumbnailer_has_pictures(task->thumbnailer, task->uri);
    if (has_pictures) {
      texture = gdk_texture_new_from_filename(cache_path, &error);
      if (texture == 0 && cache_is_valid) {
        if (error != 0) g_error_free(error);
        error = 0;
        g_unlink(cache_path);
        status = trout_thumbnailer_generate_file(task, path, file_info, cache_path);
        if (status == trout_status_success) texture = gdk_texture_new_from_filename(cache_path, &error);
      }
      if (texture == 0) {
        if (error != 0) {
          g_debug("Could not load generated thumbnail %s: %s", cache_path, error->message);
          g_error_free(error);
        }
        status = trout_status_error;
      } else trout_thumbnailer_publish_texture(task->thumbnailer, task->uri, texture);
    }
  }
  g_free(path);
  g_free(cache_path);
  g_object_unref(file_info);
  return status;
}

status_t
trout_thumbnailer_generate_file(trout_thumbnail_task *task, char *path, GFileInfo *file_info, char *cache_path)
{
  GdkPixbuf *pixbuf;
  GdkPixbuf *oriented;
  GdkPixbuf *scaled;
  GError *error;
  status_t status;
  pixbuf = 0;
  oriented = 0;
  scaled = 0;
  error = 0;
  if (task->kind == trout_thumbnail_kind_raw) {
    pixbuf = or_gdkpixbuf_extract_rotated_thumbnail(path, trout_thumbnail_max_size);
    if (pixbuf == 0) {
      g_debug("Could not extract RAW preview from %s", task->uri);
      return trout_status_error;
    }
  } else if (task->kind == trout_thumbnail_kind_video) {
    status = trout_thumbnailer_generate_video(path, &pixbuf);
    if (status != trout_status_success) return status;
  } else {
    pixbuf = gdk_pixbuf_new_from_file(path, &error);
    if (pixbuf == 0) {
      if (error != 0) {
        g_debug("Could not decode image %s: %s", task->uri, error->message);
        g_error_free(error);
      }
      return trout_status_error;
    }
    oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
    g_object_unref(pixbuf);
    if (oriented == 0) {
      g_debug("Could not apply image orientation for %s", task->uri);
      return trout_status_error;
    }
    pixbuf = oriented;
  }
  scaled = trout_thumbnailer_scale_pixbuf(pixbuf);
  if (scaled == 0) {
    g_debug("Could not scale thumbnail for %s", task->uri);
    g_object_unref(pixbuf);
    return trout_status_error;
  }
  status = trout_thumbnailer_save_pixbuf(task->thumbnailer, scaled, cache_path, task->uri, file_info);
  g_object_unref(scaled);
  g_object_unref(pixbuf);
  return status;
}

status_t
trout_thumbnailer_generate_video(char *path, GdkPixbuf **pixbuf_out)
{
  char *temp_path;
  char *filter;
  char *duration_text;
  char *seek_time;
  char *error_text;
  char *arguments[32];
  char *probe_arguments[10];
  GError *error;
  gint wait_status;
  gboolean spawned;
  gboolean succeeded;
  GdkPixbuf *pixbuf;
  guint argument_count;
  guint probe_argument_count;
  gint64 seek_microseconds;
  int fd;
  error = 0;
  probe_argument_count = 0;
  probe_arguments[probe_argument_count] = "ffprobe"; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = "-v"; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = "error"; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = "-show_entries"; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = "format=duration"; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = "-of"; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = "default=noprint_wrappers=1:nokey=1"; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = path; probe_argument_count += 1;
  probe_arguments[probe_argument_count] = 0;
  duration_text = 0;
  error_text = 0;
  spawned = g_spawn_sync(0, probe_arguments, 0, G_SPAWN_SEARCH_PATH, 0, 0, &duration_text, &error_text, &wait_status, &error);
  if (spawned) succeeded = g_spawn_check_wait_status(wait_status, &error);
  else succeeded = FALSE;
  g_clear_error(&error);
  g_free(error_text);
  if (!succeeded) {
    g_free(duration_text);
    return trout_status_error;
  }
  seek_microseconds = (gint64)(g_ascii_strtod(duration_text, 0) * 250000.0);
  g_free(duration_text);
  seek_time = g_strdup_printf("%" G_GINT64_FORMAT ".%06" G_GINT64_FORMAT,
      seek_microseconds / 1000000, seek_microseconds % 1000000);
  temp_path = g_build_filename(g_get_user_cache_dir(), "trout-video-XXXXXX", (char *)0);
  fd = g_mkstemp(temp_path);
  if (fd < 0) {
    g_warning("Could not create temporary video thumbnail: %s", g_strerror(errno));
    g_free(temp_path);
    g_free(seek_time);
    return trout_status_error;
  }
  if (close(fd) != 0) {
    g_unlink(temp_path);
    g_free(temp_path);
    g_free(seek_time);
    return trout_status_error;
  }
  filter = g_strdup_printf("thumbnail=100,scale=w='min(%u,iw)':h='min(%u,ih)':force_original_aspect_ratio=decrease",
      trout_thumbnail_max_size, trout_thumbnail_max_size);
  argument_count = 0;
  arguments[argument_count] = "ffmpeg"; argument_count += 1;
  arguments[argument_count] = "-nostdin"; argument_count += 1;
  arguments[argument_count] = "-hide_banner"; argument_count += 1;
  arguments[argument_count] = "-loglevel"; argument_count += 1;
  arguments[argument_count] = "error"; argument_count += 1;
  arguments[argument_count] = "-filter_threads"; argument_count += 1;
  arguments[argument_count] = "1"; argument_count += 1;
  arguments[argument_count] = "-threads"; argument_count += 1;
  arguments[argument_count] = "1"; argument_count += 1;
  arguments[argument_count] = "-ss"; argument_count += 1;
  arguments[argument_count] = seek_time; argument_count += 1;
  arguments[argument_count] = "-i"; argument_count += 1;
  arguments[argument_count] = path; argument_count += 1;
  arguments[argument_count] = "-map"; argument_count += 1;
  arguments[argument_count] = "0:V:0"; argument_count += 1;
  arguments[argument_count] = "-vf"; argument_count += 1;
  arguments[argument_count] = filter; argument_count += 1;
  arguments[argument_count] = "-frames:v"; argument_count += 1;
  arguments[argument_count] = "1"; argument_count += 1;
  arguments[argument_count] = "-an"; argument_count += 1;
  arguments[argument_count] = "-threads"; argument_count += 1;
  arguments[argument_count] = "1"; argument_count += 1;
  arguments[argument_count] = "-f"; argument_count += 1;
  arguments[argument_count] = "image2"; argument_count += 1;
  arguments[argument_count] = "-vcodec"; argument_count += 1;
  arguments[argument_count] = "png"; argument_count += 1;
  arguments[argument_count] = "-update"; argument_count += 1;
  arguments[argument_count] = "1"; argument_count += 1;
  arguments[argument_count] = "-y"; argument_count += 1;
  arguments[argument_count] = temp_path; argument_count += 1;
  arguments[argument_count] = 0;
  error_text = 0;
  spawned = g_spawn_sync(0, arguments, 0, G_SPAWN_SEARCH_PATH, 0, 0, 0, &error_text, &wait_status, &error);
  g_free(filter);
  g_free(seek_time);
  if (spawned) succeeded = g_spawn_check_wait_status(wait_status, &error);
  else succeeded = FALSE;
  if (!succeeded) {
    if (error != 0) {
      g_debug("FFmpeg could not create a thumbnail for %s: %s", path, error->message);
      g_error_free(error);
    }
    if (error_text != 0 && error_text[0] != '\0') g_debug("FFmpeg output: %s", error_text);
    g_free(error_text);
    g_unlink(temp_path);
    g_free(temp_path);
    return trout_status_error;
  }
  pixbuf = gdk_pixbuf_new_from_file(temp_path, &error);
  g_free(error_text);
  g_unlink(temp_path);
  g_free(temp_path);
  if (pixbuf == 0) {
    if (error != 0) {
      g_debug("Could not read FFmpeg thumbnail: %s", error->message);
      g_error_free(error);
    }
    return trout_status_error;
  }
  *pixbuf_out = pixbuf;
  return trout_status_success;
}

status_t
trout_thumbnailer_new_temp_path(trout_thumbnailer *thumbnailer, char **path_out)
{
  int fd;
  *path_out = g_build_filename(thumbnailer->cache_directory, "trout-XXXXXX", (char *)0);
  fd = g_mkstemp(*path_out);
  if (fd < 0) {
    g_warning("Could not create temporary thumbnail: %s", g_strerror(errno));
    g_free(*path_out);
    *path_out = 0;
    return trout_status_error;
  }
  if (close(fd) != 0) {
    g_unlink(*path_out);
    g_free(*path_out);
    *path_out = 0;
    return trout_status_error;
  }
  return trout_status_success;
}

status_t
trout_thumbnailer_save_pixbuf(trout_thumbnailer *thumbnailer, GdkPixbuf *pixbuf, char *cache_path, char *uri, GFileInfo *file_info)
{
  GdkPixbuf *opaque_pixbuf;
  GdkPixbuf *save_pixbuf;
  char *temp_path;
  char *quality;
  char *keys[2];
  char *values[2];
  GError *error;
  guint64 modified;
  guint32 modified_usec;
  struct timespec times[2] = {0};
  int width;
  int height;
  gboolean saved;
  status_t status;
  temp_path = 0;
  opaque_pixbuf = 0;
  save_pixbuf = pixbuf;
  error = 0;
  modified = g_file_info_get_attribute_uint64(file_info, "time::modified");
  modified_usec = g_file_info_get_attribute_uint32(file_info, "time::modified-usec");
  quality = g_strdup_printf("%d", trout_thumbnail_jpeg_quality);
  keys[0] = "quality";
  keys[1] = 0;
  values[0] = quality;
  values[1] = 0;
  status = trout_status_success;
  if (gdk_pixbuf_get_has_alpha(pixbuf)) {
    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    opaque_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
    if (opaque_pixbuf == 0) {
      g_debug("Could not prepare opaque thumbnail for %s", uri);
      status = trout_status_error;
    } else {
      gdk_pixbuf_fill(opaque_pixbuf, 0x000000ff);
      gdk_pixbuf_composite(pixbuf, opaque_pixbuf, 0, 0, width, height, 0.0, 0.0, 1.0, 1.0,
          GDK_INTERP_BILINEAR, 255);
      save_pixbuf = opaque_pixbuf;
    }
  }
  if (status == trout_status_success) status = trout_thumbnailer_new_temp_path(thumbnailer, &temp_path);
  if (status == trout_status_success) {
    saved = gdk_pixbuf_savev(save_pixbuf, temp_path, "jpeg", keys, values, &error);
    if (!saved) {
      if (error != 0) {
        g_debug("Could not save thumbnail for %s: %s", uri, error->message);
        g_error_free(error);
      }
      status = trout_status_error;
    } else if (g_rename(temp_path, cache_path) != 0) {
      g_debug("Could not install thumbnail for %s: %s", uri, g_strerror(errno));
      status = trout_status_error;
    }
  }
  if (status == trout_status_success) {
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = (time_t)modified;
    times[1].tv_nsec = modified_usec * 1000;
    if (utimensat(AT_FDCWD, cache_path, times, 0) != 0) {
      g_debug("Could not set thumbnail timestamp for %s: %s", uri, g_strerror(errno));
      g_unlink(cache_path);
      status = trout_status_error;
    }
  }
  if (temp_path != 0) {
    g_unlink(temp_path);
    g_free(temp_path);
  }
  if (opaque_pixbuf != 0) g_object_unref(opaque_pixbuf);
  g_free(quality);
  return status;
}

GdkPixbuf *
trout_thumbnailer_scale_pixbuf(GdkPixbuf *pixbuf)
{
  int source_width;
  int source_height;
  int width;
  int height;
  double scale;
  source_width = gdk_pixbuf_get_width(pixbuf);
  source_height = gdk_pixbuf_get_height(pixbuf);
  scale = (double)trout_thumbnail_max_size / source_width;
  if ((double)trout_thumbnail_max_size / source_height < scale) {
    scale = (double)trout_thumbnail_max_size / source_height;
  }
  if (scale >= 1.0) return g_object_ref(pixbuf);
  width = (int)(source_width * scale + 0.5);
  height = (int)(source_height * scale + 0.5);
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  return gdk_pixbuf_scale_simple(pixbuf, width, height, GDK_INTERP_BILINEAR);
}

gboolean
trout_thumbnailer_cache_valid(char *cache_path, guint64 modified, guint32 modified_usec)
{
  struct stat cache_status;
  gboolean valid;
  valid = g_stat(cache_path, &cache_status) == 0;
  valid = valid && cache_status.st_mtim.tv_sec >= 0 && (guint64)cache_status.st_mtim.tv_sec == modified &&
      (guint32)(cache_status.st_mtim.tv_nsec / 1000) == modified_usec;
  return valid;
}

guint
trout_thumbnailer_file_kind(trout_thumbnailer *thumbnailer, char *path)
{
  char *suffix;
  char **extension;
  suffix = strrchr(path, '.');
  if (suffix == 0 || suffix[1] == '\0') return 0;
  suffix += 1;
  if (g_ascii_strcasecmp(suffix, "jpg") == 0 || g_ascii_strcasecmp(suffix, "jpeg") == 0 ||
      g_ascii_strcasecmp(suffix, "png") == 0 || g_ascii_strcasecmp(suffix, "webp") == 0) return trout_thumbnail_kind_image;
  extension = thumbnailer->raw_extensions;
  while (extension != 0 && *extension != 0) {
    if (g_ascii_strcasecmp(suffix, *extension[0] == '.' ? *extension + 1 : *extension) == 0) return trout_thumbnail_kind_raw;
    extension += 1;
  }
  extension = trout_thumbnailer_video_extensions;
  while (*extension != 0) {
    if (g_ascii_strcasecmp(suffix, *extension) == 0) return trout_thumbnail_kind_video;
    extension += 1;
  }
  return 0;
}

gboolean
trout_thumbnailer_has_pictures(trout_thumbnailer *thumbnailer, char *uri)
{
  gboolean has_pictures;
  g_mutex_lock(&thumbnailer->mutex);
  has_pictures = g_hash_table_contains(thumbnailer->pictures, uri);
  g_mutex_unlock(&thumbnailer->mutex);
  return has_pictures;
}

void
trout_thumbnailer_publish_texture(trout_thumbnailer *thumbnailer, char *uri, GdkTexture *texture)
{
  trout_thumbnail_update *update;
  update = g_new0(trout_thumbnail_update, 1);
  update->uri = g_strdup(uri);
  update->texture = texture;
  g_mutex_lock(&thumbnailer->mutex);
  if (thumbnailer->stopping) {
    g_mutex_unlock(&thumbnailer->mutex);
    trout_thumbnailer_update_destroy(update);
    return;
  }
  g_queue_push_tail(&thumbnailer->updates, update);
  if (thumbnailer->update_source_id == 0) {
    thumbnailer->update_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE,
        trout_thumbnail_update_delay_milliseconds, trout_thumbnailer_deliver_updates, thumbnailer, 0);
  }
  g_mutex_unlock(&thumbnailer->mutex);
}

gboolean
trout_thumbnailer_deliver_updates(gpointer user_data)
{
  trout_thumbnailer *thumbnailer;
  trout_thumbnail_update *update;
  GPtrArray *pictures;
  GPtrArray *picture_snapshot;
  GtkPicture *picture;
  GtkWidget *overlay;
  GtkWidget *image;
  guint count;
  guint index;
  gboolean keep_source;
  GQueue batch = {0};
  thumbnailer = user_data;
  count = 0;
  g_mutex_lock(&thumbnailer->mutex);
  while (count < trout_thumbnail_update_batch_size && !g_queue_is_empty(&thumbnailer->updates)) {
    g_queue_push_tail(&batch, g_queue_pop_head(&thumbnailer->updates));
    count += 1;
  }
  g_mutex_unlock(&thumbnailer->mutex);
  while (!g_queue_is_empty(&batch)) {
    update = g_queue_pop_head(&batch);
    pictures = g_hash_table_lookup(thumbnailer->pictures, update->uri);
    if (pictures != 0) {
      picture_snapshot = g_ptr_array_new_with_free_func(g_object_unref);
      index = 0;
      while (index < pictures->len) {
        picture = g_object_ref(g_ptr_array_index(pictures, index));
        g_ptr_array_add(picture_snapshot, picture);
        index += 1;
      }
      index = 0;
      while (index < picture_snapshot->len) {
        picture = g_ptr_array_index(picture_snapshot, index);
        gtk_picture_set_paintable(picture, GDK_PAINTABLE(update->texture));
        overlay = gtk_widget_get_parent(GTK_WIDGET(picture));
        image = gtk_overlay_get_child(GTK_OVERLAY(overlay));
        gtk_widget_set_visible(image, FALSE);
        index += 1;
      }
      g_ptr_array_unref(picture_snapshot);
    }
    trout_thumbnailer_update_destroy(update);
  }
  g_mutex_lock(&thumbnailer->mutex);
  keep_source = !g_queue_is_empty(&thumbnailer->updates);
  if (!keep_source) thumbnailer->update_source_id = 0;
  g_mutex_unlock(&thumbnailer->mutex);
  return keep_source ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void
trout_thumbnailer_update_destroy(trout_thumbnail_update *update)
{
  g_free(update->uri);
  if (update->texture != 0) g_object_unref(update->texture);
  g_free(update);
}
