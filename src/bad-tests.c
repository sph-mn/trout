/* Replace the existing test/trout.c with this file. The standalone build
 * exercises the identical production transfer tests without GTK. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifdef TROUT_TRANSFER_TEST_STANDALONE
#include "file_transfer.h"
#else
#define trout_test_build
#include "../trout/trout.c"
#endif

#include <errno.h>
#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

/* Record a failed assertion and clean up this fixture; other tests continue. */
#define TROUT_TEST_CHECK(message, condition) do { \
  if (!(condition)) { \
    g_test_message("%s:%d: %s (%s)", __FILE__, __LINE__, message, #condition); \
    g_test_fail(); \
    goto exit; \
  } \
} while (0)

#ifndef TROUT_TRANSFER_TEST_STANDALONE
static void
trout_test_action_config_parse(void)
{
  GPtrArray *actions;
  trout_action *action;
  char configuration[] = "convert cb fd png,webp converter --fast\nfolders c d * open-folder\n";
  actions = trout_action_config_parse(configuration);
  TROUT_TEST_CHECK("action list exists", actions != 0);
  TROUT_TEST_CHECK("action count", actions->len == 2);
  action = g_ptr_array_index(actions, 0);
  TROUT_TEST_CHECK("first action exists", action != 0);
  TROUT_TEST_CHECK("action label", g_strcmp0(action->label, "convert") == 0);
  TROUT_TEST_CHECK("both appearances", action->appearance == (trout_action_appearance_context | trout_action_appearance_button));
  TROUT_TEST_CHECK("both file types", action->file_types == (trout_action_type_directories | trout_action_type_files));
  TROUT_TEST_CHECK("extensions exist", action->extensions != 0);
  TROUT_TEST_CHECK("first extension", g_strcmp0(action->extensions[0], "png") == 0);
  TROUT_TEST_CHECK("second extension", g_strcmp0(action->extensions[1], "webp") == 0);
  TROUT_TEST_CHECK("specific extensions are not wildcard", !action->any_extension);
  TROUT_TEST_CHECK("command exists", action->command != 0);
  TROUT_TEST_CHECK("command executable", g_strcmp0(action->command[0], "converter") == 0);
  TROUT_TEST_CHECK("command argument", g_strcmp0(action->command[1], "--fast") == 0);
  action = g_ptr_array_index(actions, 1);
  TROUT_TEST_CHECK("second action exists", action != 0);
  TROUT_TEST_CHECK("directory action appearance", action->appearance == trout_action_appearance_context);
  TROUT_TEST_CHECK("directory action type", action->file_types == trout_action_type_directories);
  TROUT_TEST_CHECK("wildcard extension", action->any_extension);
exit:
  if (actions != 0) g_ptr_array_unref(actions);
}

static void
trout_test_action_extension_matching(void)
{
  trout_action action = {0};
  GFileInfo *file_info;
  GFile *file;
  char *extensions[] = {"txt", 0};
  file_info = g_file_info_new();
  file = g_file_new_for_path("/tmp/readme.txt");
  action.file_types = trout_action_type_files;
  action.extensions = extensions;
  g_file_info_set_attribute_object(file_info, "standard::file", G_OBJECT(file));
  g_file_info_set_file_type(file_info, G_FILE_TYPE_REGULAR);
  g_file_info_set_name(file_info, "readme.txt");
  TROUT_TEST_CHECK("matching extension", trout_action_matches_file(&action, file_info));
  g_file_info_set_name(file_info, "readme.TXT");
  TROUT_TEST_CHECK("extension matching is case-sensitive", !trout_action_matches_file(&action, file_info));
  g_file_info_set_name(file_info, "readme");
  TROUT_TEST_CHECK("extensionless name does not match", !trout_action_matches_file(&action, file_info));
  g_file_info_set_name(file_info, ".txt");
  TROUT_TEST_CHECK("leading dot is not an extension", !trout_action_matches_file(&action, file_info));
  action.any_extension = TRUE;
  g_file_info_set_name(file_info, "readme");
  TROUT_TEST_CHECK("wildcard matches extensionless name", trout_action_matches_file(&action, file_info));
exit:
  g_object_unref(file_info);
  g_object_unref(file);
}

static void
trout_test_action_file_types(void)
{
  trout_action action = {0};
  GFileInfo *file_info;
  GFile *file;
  GFile *remote_file;
  char *extensions[] = {"*", 0};
  file_info = g_file_info_new();
  file = g_file_new_for_path("/tmp/entry");
  action.any_extension = TRUE;
  action.file_types = trout_action_type_files;
  action.extensions = extensions;
  g_file_info_set_attribute_object(file_info, "standard::file", G_OBJECT(file));
  g_file_info_set_file_type(file_info, G_FILE_TYPE_REGULAR);
  g_file_info_set_name(file_info, "entry");
  TROUT_TEST_CHECK("file action matches file", trout_action_matches_file(&action, file_info));
  action.file_types = trout_action_type_directories;
  TROUT_TEST_CHECK("directory action rejects file", !trout_action_matches_file(&action, file_info));
  g_file_info_set_file_type(file_info, G_FILE_TYPE_DIRECTORY);
  TROUT_TEST_CHECK("directory action matches directory", trout_action_matches_file(&action, file_info));
  action.file_types = trout_action_type_directories | trout_action_type_files;
  remote_file = g_file_new_for_uri("sftp://example.invalid/entry");
  g_file_info_set_attribute_object(file_info, "standard::file", G_OBJECT(remote_file));
  g_object_unref(remote_file);
  TROUT_TEST_CHECK("custom action rejects non-native path", !trout_action_matches_file(&action, file_info));
exit:
  g_object_unref(file_info);
  g_object_unref(file);
}

#endif

/* Fixtures use independent POSIX operations, never the transfer engine, to
 * construct, inspect and clean up trees. Every expected path is enumerated;
 * unexpected paths (including abandoned staging files) are failures. */
typedef struct {
  const char *path;
  char type;
  const char *value;
} trout_test_entry;

#define T_DIR(p) {p, 'd', 0}
#define T_FILE(p, v) {p, 'f', v}
#define T_LINK(p, v) {p, 'l', v}
#define T_HARDLINK(p, v) {p, 'h', v}
#define T_FIFO(p) {p, 'p', 0}
#define T_END {0, 0, 0}

typedef struct {
  const char *source;
  const char *destination;
} trout_test_pair;

typedef struct {
  guint pair;
  const char *source;
  const char *destination;
  GFileType source_type;
  GFileType destination_type;
  trout_file_collision_choice choice;
} trout_test_collision;

#define T_COLL(i, s, d, st, dt, c) {i, s, d, G_FILE_TYPE_##st, G_FILE_TYPE_##dt, trout_file_collision_##c}
#define T_MERGE(s, d) T_COLL(0, s, d, DIRECTORY, DIRECTORY, merge)
#define T_LEAF(s, d, c) T_COLL(0, s, d, REGULAR, REGULAR, c)
#define T_CEND {0, 0, 0, 0, 0, 0}

typedef enum {
  TROUT_TEST_MUTATE_NONE,
  TROUT_TEST_MUTATE_DESTINATION,
  TROUT_TEST_MUTATE_SOURCE,
  TROUT_TEST_CANCEL_AT_COLLISION,
  TROUT_TEST_STOP_BEFORE,
  TROUT_TEST_CANCEL_BEFORE,
  TROUT_TEST_CANCEL_STAGING,
  TROUT_TEST_ADD_DURING_DELETE
} trout_test_mutation;

typedef struct {
  const char *root;
  const char *peer;
  GPtrArray *operations;
  GCancellable *cancellable;
  GError *error;
  const trout_test_collision *collisions;
  gboolean *seen;
  guint expected_collisions;
  guint calls;
  guint progress_calls;
  goffset progress_current;
  goffset progress_total;
  gboolean callback_ok;
  gboolean mutated;
  trout_test_mutation mutation;
} trout_test_fixture;

enum { TROUT_TEST_ANY_IO_ERROR = -1, TROUT_TEST_NO_ERROR = -2 };

typedef struct {
  const char *name;
  trout_file_operation operation;
  const trout_test_entry *before;
  const trout_test_entry *after;
  const trout_test_pair *pairs;
  const trout_test_collision *collisions;
  trout_file_transfer_result result;
  int error_code;
  trout_test_mutation mutation;
  gboolean no_collision_callback;
  gboolean null_error;
  gboolean use_wrapper;
  gboolean cross_device;
} trout_test_scenario;

static char *
trout_test_path(const trout_test_fixture *f, const char *relative)
{
  if (f->peer != 0 && strcmp(relative, "peer") == 0) return g_strdup(f->peer);
  if (f->peer != 0 && strncmp(relative, "peer/", 5) == 0) return g_strdup_printf("%s/%s", f->peer, relative + 5);
  return g_strdup_printf("%s/%s", f->root, relative);
}

static int
trout_test_remove_entry(const char *path, const struct stat *info, int type, struct FTW *position)
{
  (void)info;
  (void)type;
  (void)position;
  return remove(path);
}

static void
trout_test_fixture_clear(trout_test_fixture *f)
{
  if (f->error != 0) {
    g_test_message("GError: domain=%u code=%d: %s", f->error->domain, f->error->code, f->error->message);
    g_clear_error(&f->error);
  }
  if (f->operations != 0) g_ptr_array_unref(f->operations);
  g_clear_object(&f->cancellable);
  g_free(f->seen);
  if (f->root != 0 && nftw(f->root, trout_test_remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
    g_test_message("fixture cleanup failed: %s: %s", f->root, strerror(errno));
    g_test_fail();
  }
  if (f->peer != 0 && nftw(f->peer, trout_test_remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
    g_test_message("peer cleanup failed: %s: %s", f->peer, strerror(errno));
    g_test_fail();
  }
  g_free((char *)f->root);
  g_free((char *)f->peer);
}

static gboolean
trout_test_fixture_init(trout_test_fixture *f)
{
  f->root = g_dir_make_tmp("trout-test-XXXXXX", &f->error);
  if (f->root == 0) return FALSE;
  f->operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  f->cancellable = g_cancellable_new();
  f->callback_ok = TRUE;
  return TRUE;
}

/* A missing distinct writable filesystem is a reported capability skip. */
static gboolean
trout_test_peer_init(trout_test_fixture *f)
{
  struct stat a, b;
  char *peer;
  if (stat(f->root, &a) != 0 || stat("/dev/shm", &b) != 0 || a.st_dev == b.st_dev) {
    g_test_skip("no distinct /dev/shm filesystem");
    return FALSE;
  }
  peer = g_strdup("/dev/shm/trout-test-XXXXXX");
  if (mkdtemp(peer) == 0) {
    g_free(peer);
    g_test_skip("no writable /dev/shm fixture");
    return FALSE;
  }
  f->peer = peer;
  return TRUE;
}

static gboolean
trout_test_write(const char *path, const char *value)
{
  FILE *stream = fopen(path, "wb");
  gboolean ok;
  if (stream == 0) return FALSE;
  ok = fwrite(value, 1, strlen(value), stream) == strlen(value);
  return fclose(stream) == 0 && ok;
}

static gboolean
trout_test_make_tree(trout_test_fixture *f, const trout_test_entry *entries)
{
  const trout_test_entry *e;
  char *path;
  char *target;
  gboolean ok;
  for (e = entries; e->path != 0; e += 1) {
    path = trout_test_path(f, e->path);
    switch (e->type) {
      case 'd': ok = mkdir(path, 0700) == 0; break;
      case 'f': ok = trout_test_write(path, e->value); break;
      case 'l': ok = symlink(e->value, path) == 0; break;
      case 'h':
        target = trout_test_path(f, e->value);
        ok = link(target, path) == 0;
        g_free(target);
        break;
      case 'p': ok = mkfifo(path, 0600) == 0; break;
      default: ok = FALSE; break;
    }
    if (!ok) g_test_message("fixture creation failed: %s: %s", path, strerror(errno));
    g_free(path);
    if (!ok) return FALSE;
  }
  return TRUE;
}

static gboolean
trout_test_check_entry(trout_test_fixture *f, const trout_test_entry *e)
{
  char *path = trout_test_path(f, e->path);
  char *target;
  struct stat st, other;
  gboolean ok = lstat(path, &st) == 0;
  char *contents = 0;
  gsize length = 0;
  ssize_t size;
  if (ok) switch (e->type) {
    case 'd': ok = S_ISDIR(st.st_mode); break;
    case 'f':
      ok = S_ISREG(st.st_mode) && g_file_get_contents(path, &contents, &length, 0) && length == strlen(e->value) && memcmp(contents, e->value, length) == 0;
      break;
    case 'l':
      contents = g_malloc(strlen(e->value) + 2);
      size = S_ISLNK(st.st_mode) ? readlink(path, contents, strlen(e->value) + 1) : -1;
      ok = size >= 0 && (size_t)size == strlen(e->value) && memcmp(contents, e->value, size) == 0;
      break;
    case 'h':
      target = trout_test_path(f, e->value);
      ok = S_ISREG(st.st_mode) && lstat(target, &other) == 0 && st.st_dev == other.st_dev && st.st_ino == other.st_ino;
      g_free(target);
      break;
    case 'p': ok = S_ISFIFO(st.st_mode); break;
    default: ok = FALSE; break;
  }
  if (!ok) g_test_message("tree mismatch: %s (expected type %c, value %s)", e->path, e->type, e->value != 0 ? e->value : "-");
  g_free(contents);
  g_free(path);
  return ok;
}

static gboolean
trout_test_check_children(trout_test_fixture *f, const trout_test_entry *expected, const char *absolute, const char *relative)
{
  DIR *dir = opendir(absolute);
  struct dirent *entry;
  const trout_test_entry *match;
  char *name;
  char *path;
  gboolean ok = dir != 0;
  int saved_errno = 0;
  if (!ok) {
    g_test_message("cannot inspect directory %s: %s", absolute, strerror(errno));
    return FALSE;
  }
  while (ok) {
    errno = 0;
    entry = readdir(dir);
    if (entry == 0) { saved_errno = errno; break; }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    name = relative[0] == '\0' ? g_strdup(entry->d_name) : g_strdup_printf("%s/%s", relative, entry->d_name);
    for (match = expected; match->path != 0 && strcmp(match->path, name) != 0; match += 1) {}
    if (match->path == 0) {
      g_test_message("unexpected entry: %s", name);
      ok = FALSE;
    } else if (match->type == 'd') {
      path = trout_test_path(f, name);
      ok = trout_test_check_children(f, expected, path, name);
      g_free(path);
    }
    g_free(name);
  }
  return closedir(dir) == 0 && saved_errno == 0 && ok;
}

static gboolean
trout_test_check_tree(trout_test_fixture *f, const trout_test_entry *entries)
{
  const trout_test_entry *e;
  for (e = entries; e->path != 0; e += 1)
    if (!trout_test_check_entry(f, e)) return FALSE;
  if (!trout_test_check_children(f, entries, f->root, "")) return FALSE;
  return f->peer == 0 || trout_test_check_children(f, entries, f->peer, "peer");
}

static gboolean
trout_test_add_pair(trout_test_fixture *f, const trout_test_pair *pair, trout_file_operation operation)
{
  char *source_path = trout_test_path(f, pair->source);
  char *destination_path = trout_test_path(f, pair->destination);
  GFile *source = g_file_new_for_path(source_path);
  GFile *destination = g_file_new_for_path(destination_path);
  trout_file_transfer_pair *entry = operation == trout_file_operation_rename ?
    trout_file_transfer_pair_new_rename(source, pair->destination, &f->error) : trout_file_transfer_pair_new(source, destination);
  if (entry != 0) g_ptr_array_add(f->operations, entry);
  g_object_unref(source);
  g_object_unref(destination);
  g_free(source_path);
  g_free(destination_path);
  return entry != 0;
}

static gboolean
trout_test_file_matches(trout_test_fixture *f, GFile *file, const char *relative)
{
  char *path = trout_test_path(f, relative);
  GFile *expected = g_file_new_for_path(path);
  gboolean same = g_file_equal(file, expected);
  g_object_unref(expected);
  g_free(path);
  return same;
}

static gboolean
trout_test_size_matches(GFile *file, GFileType type, goffset size)
{
  char *path;
  struct stat st;
  gboolean ok;
  if (type != G_FILE_TYPE_REGULAR) return TRUE;
  path = g_file_get_path(file);
  ok = path != 0 && lstat(path, &st) == 0 && st.st_size == size;
  g_free(path);
  return ok;
}

static trout_file_collision_choice
trout_test_collision_callback(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, GFileType source_type, GFileType destination_type, goffset source_size, goffset destination_size, gpointer data)
{
  trout_test_fixture *f = data;
  const trout_test_collision *c;
  trout_file_transfer_pair *pair;
  guint i;
  char *path;
  char *backup;
  f->calls += 1;
  for (i = 0; i < f->expected_collisions; i += 1) {
    c = &f->collisions[i];
    if (f->seen[i] || !trout_test_file_matches(f, source, c->source) || !trout_test_file_matches(f, destination, c->destination)) continue;
    pair = g_ptr_array_index(f->operations, c->pair);
    if (!g_file_equal(source_root, trout_file_transfer_pair_get_source(pair)) ||
        !g_file_equal(destination_root, trout_file_transfer_pair_get_destination(pair)) ||
        source_type != c->source_type || destination_type != c->destination_type ||
        !trout_test_size_matches(source, source_type, source_size) || !trout_test_size_matches(destination, destination_type, destination_size)) break;
    f->seen[i] = TRUE;
    if (f->mutation == TROUT_TEST_MUTATE_DESTINATION || f->mutation == TROUT_TEST_MUTATE_SOURCE) {
      path = g_file_get_path(f->mutation == TROUT_TEST_MUTATE_DESTINATION ? destination : source);
      backup = g_strdup_printf("%s.old", path);
      f->mutated = rename(path, backup) == 0 && trout_test_write(path, "racer");
      g_free(path);
      g_free(backup);
      if (!f->mutated) f->callback_ok = FALSE;
    } else if (f->mutation == TROUT_TEST_CANCEL_AT_COLLISION) {
      g_cancellable_cancel(f->cancellable);
      f->mutated = TRUE;
    }
    return c->choice;
  }
  path = g_file_get_path(source);
  backup = g_file_get_path(destination);
  g_test_message("unexpected collision or wrong metadata: %s -> %s, types %d/%d", path, backup, source_type, destination_type);
  g_free(path);
  g_free(backup);
  f->callback_ok = FALSE;
  return trout_file_collision_cancel;
}

static gboolean
trout_test_wait(gpointer data)
{
  trout_test_fixture *f = data;
  char *path;
  DIR *directory;
  struct dirent *entry;
  if (f->mutation == TROUT_TEST_STOP_BEFORE) { f->mutated = TRUE; return FALSE; }
  if (f->mutation == TROUT_TEST_CANCEL_STAGING && !f->mutated) {
    directory = opendir(f->root);
    if (directory == 0) { f->callback_ok = FALSE; return FALSE; }
    while ((entry = readdir(directory)) != 0) {
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
          strcmp(entry->d_name, "src") != 0 && strcmp(entry->d_name, "dst") != 0) {
        f->mutated = TRUE;
        g_cancellable_cancel(f->cancellable);
        break;
      }
    }
    closedir(directory);
  }
  if (f->mutation == TROUT_TEST_ADD_DURING_DELETE && !f->mutated) {
    path = trout_test_path(f, "src/leaf");
    if (access(path, F_OK) != 0 && errno == ENOENT) {
      g_free(path);
      path = trout_test_path(f, "src/new");
      f->mutated = trout_test_write(path, "new");
      if (!f->mutated) f->callback_ok = FALSE;
    }
    g_free(path);
  }
  return !g_cancellable_is_cancelled(f->cancellable);
}

static void
trout_test_progress(GFile *source, goffset current, goffset total, gpointer data)
{
  trout_test_fixture *f = data;
  if (source == 0 || current < 0 || total <= 0 || current > total || current < f->progress_current ||
      (f->progress_calls != 0 && total != f->progress_total)) {
    g_test_message("invalid progress update: %lld/%lld after %lld/%lld", (long long)current, (long long)total, (long long)f->progress_current, (long long)f->progress_total);
    f->callback_ok = FALSE;
  }
  f->progress_calls += 1;
  f->progress_current = current;
  f->progress_total = total;
}

static void
trout_test_run_scenario(gconstpointer data)
{
  const trout_test_scenario *s = data;
  const trout_test_pair *pair;
  trout_test_fixture f = {0};
  trout_file_transfer_callbacks callbacks = {&f, trout_test_wait, trout_test_collision_callback, trout_test_progress};
  trout_file_transfer_result result;
  gboolean wrapper;
  guint i;
  TROUT_TEST_CHECK("fixture created", trout_test_fixture_init(&f));
  if (s->cross_device && !trout_test_peer_init(&f)) goto exit;
  TROUT_TEST_CHECK("initial tree created", trout_test_make_tree(&f, s->before));
  TROUT_TEST_CHECK("initial tree exact", trout_test_check_tree(&f, s->before));
  for (pair = s->pairs; pair->source != 0; pair += 1)
    TROUT_TEST_CHECK("operation added", trout_test_add_pair(&f, pair, s->operation));
  f.collisions = s->collisions;
  if (f.collisions != 0) while (f.collisions[f.expected_collisions].source != 0) f.expected_collisions += 1;
  f.seen = g_new0(gboolean, f.expected_collisions);
  f.mutation = s->mutation;
  if (s->no_collision_callback) callbacks.collision = 0;
  if (s->mutation == TROUT_TEST_CANCEL_BEFORE) { g_cancellable_cancel(f.cancellable); f.mutated = TRUE; }
  if (s->use_wrapper) {
    wrapper = trout_file_transfer_execute(f.operations, s->operation, f.cancellable, &callbacks, &f.error);
    TROUT_TEST_CHECK("boolean wrapper result", wrapper == (s->result == trout_file_transfer_complete));
  } else {
    result = trout_file_transfer_execute_full(f.operations, s->operation, f.cancellable, &callbacks, s->null_error ? 0 : &f.error);
    if (result != s->result) g_test_message("result: expected %d, got %d", s->result, result);
    TROUT_TEST_CHECK("exact result", result == s->result);
  }
  if (s->null_error || s->error_code == TROUT_TEST_NO_ERROR) TROUT_TEST_CHECK("no error", f.error == 0);
  else if (s->error_code == TROUT_TEST_ANY_IO_ERROR) TROUT_TEST_CHECK("GIO error present", f.error != 0 && f.error->domain == G_IO_ERROR);
  else TROUT_TEST_CHECK("exact GIO error", g_error_matches(f.error, G_IO_ERROR, s->error_code));
  g_clear_error(&f.error);
  TROUT_TEST_CHECK("callbacks correct", f.callback_ok);
  TROUT_TEST_CHECK("collision count", f.calls == f.expected_collisions);
  for (i = 0; i < f.expected_collisions; i += 1) TROUT_TEST_CHECK("expected collision delivered", f.seen[i]);
  if (s->mutation != TROUT_TEST_MUTATE_NONE) TROUT_TEST_CHECK("fault injection executed", f.mutated);
  if (f.operations->len != 0) {
    TROUT_TEST_CHECK("progress callback invoked", f.progress_calls > 0);
    if (s->result == trout_file_transfer_complete || s->result == trout_file_transfer_partial || s->result == trout_file_transfer_skipped)
      TROUT_TEST_CHECK("terminal progress reaches total", f.progress_current == f.progress_total);
  }
  TROUT_TEST_CHECK("exact final filesystem tree", trout_test_check_tree(&f, s->after != 0 ? s->after : s->before));
exit:
  trout_test_fixture_clear(&f);
}

/* Each scenario declares the entire source and destination namespace. */
static const trout_test_scenario trout_test_scenarios[] = {
  {
    .name = "/transfer/copy/merge-skip", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_FILE("dst/nested/new", "new"),
      T_DIR("dst/added"),
      T_FILE("dst/added/new", "added"),
      T_DIR("dst/empty"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/nested", "dst/nested", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/collision", "dst/collision", REGULAR, REGULAR, skip),
      T_CEND,
    },
    .result = trout_file_transfer_partial, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/merge-replace", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "source"),
      T_FILE("dst/nested/new", "new"),
      T_DIR("dst/added"),
      T_FILE("dst/added/new", "added"),
      T_DIR("dst/empty"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/nested", "dst/nested", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/collision", "dst/collision", REGULAR, REGULAR, replace),
      T_CEND,
    },
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/merge-root-skip", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, skip), T_CEND},
    .result = trout_file_transfer_skipped, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/merge-root-cancel", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, cancel), T_CEND},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED
  },
  {
    .name = "/transfer/copy/merge-replace-refused", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, replace), T_CEND},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_SUPPORTED
  },
  {
    .name = "/transfer/copy/merge-no-callback", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_EXISTS, .no_collision_callback = TRUE
  },
  {
    .name = "/transfer/copy/merge-empty", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_DIR("src"), T_DIR("dst"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge), T_CEND},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/tree-symlinks", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/a"),
      T_FILE("src/a/file", "data"),
      T_DIR("src/empty"),
      T_LINK("src/link", "a/file"),
      T_LINK("src/dirlink", "a"),
      T_LINK("src/dangling", "missing"),
      T_LINK("src/loop", "."),
      T_DIR("outside"),
      T_FILE("outside/keep", "keep"),
      T_LINK("src/external", "../outside"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/a"),
      T_FILE("src/a/file", "data"),
      T_DIR("src/empty"),
      T_LINK("src/link", "a/file"),
      T_LINK("src/dirlink", "a"),
      T_LINK("src/dangling", "missing"),
      T_LINK("src/loop", "."),
      T_DIR("outside"),
      T_FILE("outside/keep", "keep"),
      T_LINK("src/external", "../outside"),
      T_DIR("dst"),
      T_DIR("dst/a"),
      T_FILE("dst/a/file", "data"),
      T_DIR("dst/empty"),
      T_LINK("dst/link", "a/file"),
      T_LINK("dst/dirlink", "a"),
      T_LINK("dst/dangling", "missing"),
      T_LINK("dst/loop", "."),
      T_LINK("dst/external", "../outside"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/empty-file", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", ""), T_END},
    .after = (const trout_test_entry[]){T_FILE("src", ""), T_FILE("dst", ""), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/leaf-replace", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "new"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/leaf-skip", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, skip), T_CEND},
    .result = trout_file_transfer_skipped, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/leaf-cancel", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, cancel), T_CEND},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED
  },
  {
    .name = "/transfer/copy/leaf-replace-symlink", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_FILE("src", "new"),
      T_FILE("target", "old"),
      T_LINK("dst", "target"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_FILE("src", "new"),
      T_FILE("target", "old"),
      T_FILE("dst", "new"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", REGULAR, SYMBOLIC_LINK, replace),
      T_CEND,
    },
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/symlink-replaces-leaf", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_LINK("src", "missing"), T_FILE("dst", "old"), T_END},
    .after = (const trout_test_entry[]){T_LINK("src", "missing"), T_LINK("dst", "missing"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", SYMBOLIC_LINK, REGULAR, replace),
      T_CEND,
    },
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/symlink-to-directory-replace", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/a", "data"),
      T_LINK("dst", "src"),
      T_FILE("input", "new"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/a", "data"),
      T_FILE("dst", "new"),
      T_FILE("input", "new"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"input", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "input", "dst", REGULAR, SYMBOLIC_LINK, replace),
      T_CEND,
    },
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/file-over-directory-refused", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_DIR("dst"), T_FILE("dst/keep", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, DIRECTORY, replace), T_CEND},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_SUPPORTED
  },
  {
    .name = "/transfer/copy/directory-over-file-refused", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", DIRECTORY, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_SUPPORTED
  },
  {
    .name = "/transfer/copy/directory-over-file-skip", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", DIRECTORY, REGULAR, skip), T_CEND},
    .result = trout_file_transfer_skipped, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/copy/fifo-refused", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FIFO("src"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", SPECIAL, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_SUPPORTED
  },
  {
    .name = "/transfer/copy/invalid-choice", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, merge), T_CEND},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_SUPPORTED
  },
  {
    .name = "/transfer/move/leaf-skip", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, skip), T_CEND},
    .result = trout_file_transfer_skipped, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/move/leaf-replace", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = (const trout_test_entry[]){T_FILE("dst", "new"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/move/merge-partial", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/a"),
      T_FILE("src/a/shared", "new"),
      T_FILE("src/a/unique", "unique"),
      T_DIR("src/b"),
      T_FILE("src/b/file", "b"),
      T_DIR("dst"),
      T_DIR("dst/a"),
      T_FILE("dst/a/shared", "old"),
      T_FILE("dst/keep", "keep"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/a"),
      T_FILE("src/a/shared", "new"),
      T_DIR("dst"),
      T_DIR("dst/a"),
      T_FILE("dst/a/shared", "old"),
      T_FILE("dst/a/unique", "unique"),
      T_FILE("dst/keep", "keep"),
      T_DIR("dst/b"),
      T_FILE("dst/b/file", "b"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/a", "dst/a", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/a/shared", "dst/a/shared", REGULAR, REGULAR, skip),
      T_CEND,
    },
    .result = trout_file_transfer_partial, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/move/merge-complete", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/new", "new"),
      T_FILE("src/collision", "source"),
      T_DIR("src/added"),
      T_FILE("src/added/new", "added"),
      T_DIR("src/empty"),
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "destination"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("dst"),
      T_DIR("dst/nested"),
      T_FILE("dst/nested/keep", "nested keep"),
      T_FILE("dst/keep", "keep"),
      T_FILE("dst/collision", "source"),
      T_FILE("dst/nested/new", "new"),
      T_DIR("dst/added"),
      T_FILE("dst/added/new", "added"),
      T_DIR("dst/empty"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/nested", "dst/nested", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/collision", "dst/collision", REGULAR, REGULAR, replace),
      T_CEND,
    },
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/move/merge-all-skipped", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/a", "new"),
      T_DIR("dst"),
      T_FILE("dst/a", "old"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/a", "dst/a", REGULAR, REGULAR, skip),
      T_CEND,
    },
    .result = trout_file_transfer_skipped, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/move/skip-nested-subtree", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/a"),
      T_FILE("src/a/x", "x"),
      T_DIR("src/b"),
      T_FILE("src/b/y", "y"),
      T_DIR("dst"),
      T_DIR("dst/a"),
      T_FILE("dst/a/keep", "keep"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/a"),
      T_FILE("src/a/x", "x"),
      T_DIR("dst"),
      T_DIR("dst/a"),
      T_FILE("dst/a/keep", "keep"),
      T_DIR("dst/b"),
      T_FILE("dst/b/y", "y"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge),
      T_COLL(0, "src/a", "dst/a", DIRECTORY, DIRECTORY, skip),
      T_CEND,
    },
    .result = trout_file_transfer_partial, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/move/merge-empty", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_DIR("src"), T_DIR("dst"), T_END},
    .after = (const trout_test_entry[]){T_DIR("dst"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", DIRECTORY, DIRECTORY, merge), T_CEND},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/move/symlink", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_LINK("src", "missing"), T_END},
    .after = (const trout_test_entry[]){T_LINK("dst", "missing"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/guard/copy/inside", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/child"),
      T_FILE("src/child/keep", "keep"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src/child/new"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/same-directory", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/keep", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/ancestor", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("dst"),
      T_DIR("dst/src"),
      T_FILE("dst/src/victim", "original"),
      T_DIR("dst/src/src"),
      T_FILE("dst/src/src/victim", "replacement"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"dst/src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/symlink-ancestor", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/child"),
      T_FILE("src/child/keep", "keep"),
      T_LINK("alias", "src/child"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "alias/new"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/same-leaf", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/leaf-alias", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_LINK("src/link", "missing"),
      T_LINK("alias", "src"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src/link", "alias/link"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/hardlink-alias", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_HARDLINK("dst", "src"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/batch-overlap", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("first", "one"), T_FILE("second", "two"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"first", "new"}, {"second", "first"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/nested-source-selection", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "a"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {"src/a", "other"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/duplicate-source-selection", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {"src", "other"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/cross-tree-hardlink", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/leaf", "one"),
      T_DIR("dst"),
      T_HARDLINK("dst/leaf", "src/leaf"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/copy/prefix-sibling", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "a"), T_END},
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/a", "a"),
      T_DIR("src-copy"),
      T_FILE("src-copy/a", "a"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "src-copy"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/guard/move/inside", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/child"),
      T_FILE("src/child/keep", "keep"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src/child/new"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/same-directory", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/keep", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/ancestor", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("dst"),
      T_DIR("dst/src"),
      T_FILE("dst/src/victim", "original"),
      T_DIR("dst/src/src"),
      T_FILE("dst/src/src/victim", "replacement"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"dst/src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/symlink-ancestor", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/child"),
      T_FILE("src/child/keep", "keep"),
      T_LINK("alias", "src/child"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "alias/new"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/same-leaf", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/leaf-alias", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_LINK("src/link", "missing"),
      T_LINK("alias", "src"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src/link", "alias/link"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/hardlink-alias", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_HARDLINK("dst", "src"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/batch-overlap", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("first", "one"), T_FILE("second", "two"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"first", "new"}, {"second", "first"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/nested-source-selection", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "a"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {"src/a", "other"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/duplicate-source-selection", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {"src", "other"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/cross-tree-hardlink", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/leaf", "one"),
      T_DIR("dst"),
      T_HARDLINK("dst/leaf", "src/leaf"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/guard/move/prefix-sibling", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "a"), T_END},
    .after = (const trout_test_entry[]){T_DIR("src-copy"), T_FILE("src-copy/a", "a"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "src-copy"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/batch/complete", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "a"), T_FILE("second", "b"), T_END},
    .after = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/a", "a"),
      T_FILE("second", "b"),
      T_DIR("dst"),
      T_FILE("dst/a", "a"),
      T_FILE("other", "b"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {"second", "other"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/batch/cancel-after-commit", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_FILE("first", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_FILE("third", "three"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_FILE("first", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_FILE("third", "three"),
      T_FILE("dst1", "one"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"first", "dst1"}, {"second", "dst2"}, {"third", "dst3"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(1, "second", "dst2", REGULAR, REGULAR, cancel), T_CEND},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED
  },
  {
    .name = "/transfer/batch/partial", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_FILE("first", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_FILE("first", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_FILE("dst1", "one"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"first", "dst1"}, {"second", "dst2"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(1, "second", "dst2", REGULAR, REGULAR, skip), T_CEND},
    .result = trout_file_transfer_partial, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/batch/missing-source-preflight", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("first", "one"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"first", "dst"}, {"missing", "other"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_FOUND
  },
  {
    .name = "/transfer/batch/empty", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/cancel/pre-cancelled", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED, .mutation = TROUT_TEST_CANCEL_BEFORE
  },
  {
    .name = "/transfer/cancel/wait-rejected", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED, .mutation = TROUT_TEST_STOP_BEFORE
  },
  {
    .name = "/transfer/cancel/during-collision", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED, .mutation = TROUT_TEST_CANCEL_AT_COLLISION
  },
  {
    .name = "/transfer/cancel/staged-replacement", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED, .mutation = TROUT_TEST_CANCEL_STAGING
  },
  {
    .name = "/transfer/race/destination-replaced", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = (const trout_test_entry[]){
      T_FILE("src", "new"),
      T_FILE("dst", "racer"),
      T_FILE("dst.old", "old"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_FAILED, .mutation = TROUT_TEST_MUTATE_DESTINATION
  },
  {
    .name = "/transfer/race/source-replaced", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = (const trout_test_entry[]){
      T_FILE("src", "racer"),
      T_FILE("src.old", "new"),
      T_FILE("dst", "old"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_FAILED, .mutation = TROUT_TEST_MUTATE_SOURCE
  },
  {
    .name = "/transfer/delete/tree", .operation = trout_file_operation_delete,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/file", "data"),
      T_DIR("src/empty"),
      T_LINK("src/link", "../outside"),
      T_LINK("src/dangling", "missing"),
      T_LINK("src/loop", "."),
      T_DIR("outside"),
      T_FILE("outside/keep", "keep"),
      T_END,
    },
    .after = (const trout_test_entry[]){T_DIR("outside"), T_FILE("outside/keep", "keep"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/delete/dangling-link", .operation = trout_file_operation_delete,
    .before = (const trout_test_entry[]){T_LINK("src", "missing"), T_END},
    .after = (const trout_test_entry[]){T_END},
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/delete/directory-link", .operation = trout_file_operation_delete,
    .before = (const trout_test_entry[]){
      T_DIR("outside"),
      T_FILE("outside/keep", "keep"),
      T_LINK("src", "outside"),
      T_END,
    },
    .after = (const trout_test_entry[]){T_DIR("outside"), T_FILE("outside/keep", "keep"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/delete/overlap", .operation = trout_file_operation_delete,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/leaf", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {"src/leaf", "src/leaf"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_WOULD_RECURSE
  },
  {
    .name = "/transfer/delete/new-entry-not-rescanned", .operation = trout_file_operation_delete,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/leaf", "old"), T_END},
    .after = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/new", "new"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_EMPTY, .mutation = TROUT_TEST_ADD_DURING_DELETE
  },
  {
    .name = "/transfer/delete/cancel", .operation = trout_file_operation_delete,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/leaf", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED, .mutation = TROUT_TEST_CANCEL_BEFORE
  },
  {
    .name = "/transfer/delete/missing", .operation = trout_file_operation_delete,
    .before = (const trout_test_entry[]){T_FILE("keep", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_FOUND
  },
  {
    .name = "/transfer/rename/directory", .operation = trout_file_operation_rename,
    .before = (const trout_test_entry[]){T_DIR("src"), T_FILE("src/a", "a"), T_END},
    .after = (const trout_test_entry[]){T_DIR("dst"), T_FILE("dst/a", "a"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/rename/no-merge", .operation = trout_file_operation_rename,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_FILE("src/a", "a"),
      T_DIR("dst"),
      T_FILE("dst/b", "b"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_EXISTS
  },
  {
    .name = "/transfer/rename/no-overwrite", .operation = trout_file_operation_rename,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_EXISTS
  },
  {
    .name = "/transfer/api/null-error", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "src"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = TROUT_TEST_NO_ERROR, .null_error = TRUE
  },
  {
    .name = "/transfer/api/wrapper-complete", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_END},
    .after = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "new"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR, .use_wrapper = TRUE
  },
  {
    .name = "/transfer/api/wrapper-skipped", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("dst", "old"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "dst", REGULAR, REGULAR, skip), T_CEND},
    .result = trout_file_transfer_skipped, .error_code = G_IO_ERROR_FAILED, .use_wrapper = TRUE
  },
  {
    .name = "/transfer/api/wrapper-partial", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_FILE("first", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_FILE("first", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_FILE("dst1", "one"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"first", "dst1"}, {"second", "dst2"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(1, "second", "dst2", REGULAR, REGULAR, skip), T_CEND},
    .result = trout_file_transfer_partial, .error_code = G_IO_ERROR_FAILED, .use_wrapper = TRUE
  },
  {
    .name = "/transfer/cross-device/move-tree", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/nested"),
      T_FILE("src/nested/file", "data"),
      T_LINK("src/dangling", "missing"),
      T_DIR("src/empty"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_DIR("peer/dst"),
      T_DIR("peer/dst/nested"),
      T_FILE("peer/dst/nested/file", "data"),
      T_LINK("peer/dst/dangling", "missing"),
      T_DIR("peer/dst/empty"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"src", "peer/dst"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR, .cross_device = TRUE
  },
  {
    .name = "/transfer/cross-device/move-replace", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_FILE("src", "new"), T_FILE("peer/dst", "old"), T_END},
    .after = (const trout_test_entry[]){T_FILE("peer/dst", "new"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "peer/dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(0, "src", "peer/dst", REGULAR, REGULAR, replace), T_CEND},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR, .cross_device = TRUE
  },
  {
    .name = "/transfer/cross-device/move-replace-symlink", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){T_LINK("src", "missing"), T_FILE("peer/dst", "old"), T_END},
    .after = (const trout_test_entry[]){T_LINK("peer/dst", "missing"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "peer/dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_COLL(0, "src", "peer/dst", SYMBOLIC_LINK, REGULAR, replace),
      T_CEND,
    },
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR, .cross_device = TRUE
  },
  {
    .name = "/transfer/copy/nested-directory-symlink-skip", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){
      T_DIR("src"),
      T_DIR("src/a"),
      T_FILE("src/a/x", "x"),
      T_DIR("dst"),
      T_LINK("dst/a", "../src/a"),
      T_END,
    },
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .collisions = (const trout_test_collision[]){
      T_MERGE("src", "dst"),
      T_COLL(0, "src/a", "dst/a", DIRECTORY, SYMBOLIC_LINK, skip),
      T_CEND,
    },
    .result = trout_file_transfer_skipped, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/batch/move-cancel-after-commit", .operation = trout_file_operation_move,
    .before = (const trout_test_entry[]){
      T_FILE("first", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_FILE("third", "three"),
      T_END,
    },
    .after = (const trout_test_entry[]){
      T_FILE("dst1", "one"),
      T_FILE("second", "new"),
      T_FILE("dst2", "old"),
      T_FILE("third", "three"),
      T_END,
    },
    .pairs = (const trout_test_pair[]){{"first", "dst1"}, {"second", "dst2"}, {"third", "dst3"}, {0, 0}},
    .collisions = (const trout_test_collision[]){T_COLL(1, "second", "dst2", REGULAR, REGULAR, cancel), T_CEND},
    .result = trout_file_transfer_cancelled, .error_code = G_IO_ERROR_CANCELLED
  },
  {
    .name = "/transfer/copy/missing-destination-parent", .operation = trout_file_operation_copy,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "missing/dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_NOT_FOUND
  },
  {
    .name = "/transfer/rename/utf8-name", .operation = trout_file_operation_rename,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = (const trout_test_entry[]){T_FILE("r\xc3\xa9name", "keep"), T_END},
    .pairs = (const trout_test_pair[]){{"src", "r\xc3\xa9name"}, {0, 0}},
    .result = trout_file_transfer_complete, .error_code = TROUT_TEST_NO_ERROR
  },
  {
    .name = "/transfer/api/unknown-operation", .operation = (trout_file_operation)99,
    .before = (const trout_test_entry[]){T_FILE("src", "keep"), T_END},
    .after = 0,
    .pairs = (const trout_test_pair[]){{"src", "dst"}, {0, 0}},
    .result = trout_file_transfer_failed, .error_code = G_IO_ERROR_INVALID_ARGUMENT
  },

};

static void
trout_test_merge_policy(void)
{
  static const GFileType types[] = {
    G_FILE_TYPE_UNKNOWN, G_FILE_TYPE_REGULAR, G_FILE_TYPE_DIRECTORY,
    G_FILE_TYPE_SYMBOLIC_LINK, G_FILE_TYPE_SPECIAL, G_FILE_TYPE_SHORTCUT,
    G_FILE_TYPE_MOUNTABLE
  };
  guint a, b;
  for (a = 0; a < G_N_ELEMENTS(types); a += 1)
    for (b = 0; b < G_N_ELEMENTS(types); b += 1)
      TROUT_TEST_CHECK("only directory/directory is mergeable", trout_file_transfer_should_merge(types[a], types[b]) == (types[a] == G_FILE_TYPE_DIRECTORY && types[b] == G_FILE_TYPE_DIRECTORY));
exit:
  return;
}

static void
trout_test_rename_invalid_names(void)
{
  static const char *names[] = {0, "", ".", "..", "a/b", "\xff"};
  trout_test_fixture f = {0};
  GFile *source = 0;
  char *path = 0;
  trout_file_transfer_pair *pair = 0;
  guint i;
  static const trout_test_entry entries[] = {T_FILE("src", "keep"), T_END};
  TROUT_TEST_CHECK("fixture created", trout_test_fixture_init(&f));
  TROUT_TEST_CHECK("source created", trout_test_make_tree(&f, entries));
  path = trout_test_path(&f, "src");
  source = g_file_new_for_path(path);
  for (i = 0; i < G_N_ELEMENTS(names); i += 1) {
    pair = trout_file_transfer_pair_new_rename(source, names[i], &f.error);
    TROUT_TEST_CHECK("invalid rename rejected", pair == 0);
    TROUT_TEST_CHECK("invalid rename error", g_error_matches(f.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT));
    g_clear_error(&f.error);
  }
  TROUT_TEST_CHECK("source unchanged", trout_test_check_tree(&f, entries));
exit:
  if (pair != 0) trout_file_transfer_pair_destroy(pair);
  g_clear_object(&source);
  g_free(path);
  trout_test_fixture_clear(&f);
}

/* Native moves and rename must retain inode identity, and copy must preserve
 * mode and modification time of new directories. Use whole-second times to
 * avoid assuming nanosecond support on every temporary filesystem. */
static void
trout_test_metadata(gconstpointer data)
{
  const trout_file_operation *operation = data;
  trout_test_fixture f = {0};
  static const trout_test_entry entries[] = {T_DIR("src"), T_DIR("src/a"), T_FILE("src/a/file", "data"), T_END};
  static const trout_test_entry merged_entries[] = {T_DIR("src"), T_DIR("src/a"), T_FILE("src/a/file", "data"), T_DIR("dst"), T_DIR("dst/a"), T_FILE("dst/a/file", "data"), T_END};
  static const trout_test_entry moved_entries[] = {T_DIR("dst"), T_DIR("dst/a"), T_FILE("dst/a/file", "data"), T_END};
  char *s = 0, *d = 0, *nested = 0, *dest_nested = 0;
  struct stat source_stat, dest_stat, nested_stat, dest_nested_stat;
  struct timespec times[2] = {{1234567000, 0}, {1234567100, 0}};
  TROUT_TEST_CHECK("fixture created", trout_test_fixture_init(&f));
  TROUT_TEST_CHECK("tree created", trout_test_make_tree(&f, entries));
  s = trout_test_path(&f, "src"); d = trout_test_path(&f, "dst");
  nested = trout_test_path(&f, "src/a"); dest_nested = trout_test_path(&f, "dst/a");
  TROUT_TEST_CHECK("set root permissions", chmod(s, 0710) == 0);
  TROUT_TEST_CHECK("set child permissions", chmod(nested, 0750) == 0);
  TROUT_TEST_CHECK("set child timestamp", utimensat(AT_FDCWD, nested, times, 0) == 0);
  TROUT_TEST_CHECK("set root timestamp", utimensat(AT_FDCWD, s, times, 0) == 0);
  TROUT_TEST_CHECK("read metadata", stat(s, &source_stat) == 0 && stat(nested, &nested_stat) == 0);
  TROUT_TEST_CHECK("pair created", trout_test_add_pair(&f, &(const trout_test_pair){"src", "dst"}, *operation));
  TROUT_TEST_CHECK("operation completes with NULL callbacks and cancellable", trout_file_transfer_execute_full(f.operations, *operation, 0, 0, &f.error) == trout_file_transfer_complete);
  TROUT_TEST_CHECK("no error", f.error == 0);
  TROUT_TEST_CHECK("destination metadata readable", stat(d, &dest_stat) == 0 && stat(dest_nested, &dest_nested_stat) == 0);
  TROUT_TEST_CHECK("root mode preserved", (source_stat.st_mode & 0777) == (dest_stat.st_mode & 0777));
  TROUT_TEST_CHECK("nested mode preserved", (nested_stat.st_mode & 0777) == (dest_nested_stat.st_mode & 0777));
  TROUT_TEST_CHECK("root modification time preserved", source_stat.st_mtime == dest_stat.st_mtime);
  TROUT_TEST_CHECK("nested modification time preserved", nested_stat.st_mtime == dest_nested_stat.st_mtime);
  if (*operation != trout_file_operation_copy) {
    TROUT_TEST_CHECK("native root inode preserved", source_stat.st_dev == dest_stat.st_dev && source_stat.st_ino == dest_stat.st_ino);
    TROUT_TEST_CHECK("native child inode preserved", nested_stat.st_dev == dest_nested_stat.st_dev && nested_stat.st_ino == dest_nested_stat.st_ino);
  }
  TROUT_TEST_CHECK("exact final tree", trout_test_check_tree(&f, *operation == trout_file_operation_copy ? merged_entries : moved_entries));
exit:
  g_free(s); g_free(d); g_free(nested); g_free(dest_nested);
  trout_test_fixture_clear(&f);
}

static void
trout_test_merge_keeps_directory_mode(void)
{
  trout_test_fixture f = {0};
  static const trout_test_entry entries[] = {T_DIR("src"), T_DIR("src/nested"), T_FILE("src/nested/new", "new"), T_DIR("dst"), T_DIR("dst/nested"), T_FILE("dst/nested/keep", "keep"), T_END};
  static const trout_test_entry after[] = {T_DIR("src"), T_DIR("src/nested"), T_FILE("src/nested/new", "new"), T_DIR("dst"), T_DIR("dst/nested"), T_FILE("dst/nested/keep", "keep"), T_FILE("dst/nested/new", "new"), T_END};
  static const trout_test_collision collisions[] = {T_MERGE("src", "dst"), T_MERGE("src/nested", "dst/nested"), T_CEND};
  trout_file_transfer_callbacks callbacks = {&f, trout_test_wait, trout_test_collision_callback, trout_test_progress};
  char *d = 0, *nested = 0;
  struct stat a, b, c, e;
  TROUT_TEST_CHECK("fixture created", trout_test_fixture_init(&f));
  TROUT_TEST_CHECK("tree created", trout_test_make_tree(&f, entries));
  d = trout_test_path(&f, "dst"); nested = trout_test_path(&f, "dst/nested");
  TROUT_TEST_CHECK("set destination modes", chmod(d, 0751) == 0 && chmod(nested, 0710) == 0);
  TROUT_TEST_CHECK("destination metadata", stat(d, &a) == 0 && stat(nested, &b) == 0);
  f.collisions = collisions; f.expected_collisions = 2; f.seen = g_new0(gboolean, 2);
  TROUT_TEST_CHECK("pair added", trout_test_add_pair(&f, &(const trout_test_pair){"src", "dst"}, trout_file_operation_copy));
  TROUT_TEST_CHECK("merge completes", trout_file_transfer_execute_full(f.operations, trout_file_operation_copy, f.cancellable, &callbacks, &f.error) == trout_file_transfer_complete);
  TROUT_TEST_CHECK("no error", f.error == 0);
  TROUT_TEST_CHECK("both collision callbacks", f.callback_ok && f.calls == 2 && f.seen[0] && f.seen[1]);
  TROUT_TEST_CHECK("destination identity/mode after merge", stat(d, &c) == 0 && stat(nested, &e) == 0 && a.st_dev == c.st_dev && a.st_ino == c.st_ino && b.st_dev == e.st_dev && b.st_ino == e.st_ino && a.st_mode == c.st_mode && b.st_mode == e.st_mode);
  TROUT_TEST_CHECK("exact merged tree", trout_test_check_tree(&f, after));
exit:
  g_free(d); g_free(nested);
  trout_test_fixture_clear(&f);
}

static void
trout_test_binary_copy(void)
{
  trout_test_fixture f = {0};
  unsigned char bytes[65537];
  char *source = 0, *destination = 0, *data = 0;
  gsize length;
  guint i;
  FILE *stream = 0;
  gboolean written;
  TROUT_TEST_CHECK("fixture created", trout_test_fixture_init(&f));
  for (i = 0; i < sizeof(bytes); i += 1) bytes[i] = (unsigned char)(i * 31u + i / 256u);
  source = trout_test_path(&f, "src"); destination = trout_test_path(&f, "dst");
  stream = fopen(source, "wb");
  TROUT_TEST_CHECK("binary file opened", stream != 0);
  written = fwrite(bytes, 1, sizeof(bytes), stream) == sizeof(bytes);
  written = fclose(stream) == 0 && written; stream = 0;
  TROUT_TEST_CHECK("binary data written", written);
  TROUT_TEST_CHECK("pair added", trout_test_add_pair(&f, &(const trout_test_pair){"src", "dst"}, trout_file_operation_copy));
  TROUT_TEST_CHECK("copy completes", trout_file_transfer_execute_full(f.operations, trout_file_operation_copy, 0, 0, &f.error) == trout_file_transfer_complete && f.error == 0);
  TROUT_TEST_CHECK("destination binary data", g_file_get_contents(destination, &data, &length, &f.error) && length == sizeof(bytes) && memcmp(data, bytes, length) == 0);
  g_free(data); data = 0;
  TROUT_TEST_CHECK("source binary data", g_file_get_contents(source, &data, &length, &f.error) && length == sizeof(bytes) && memcmp(data, bytes, length) == 0);
exit:
  if (stream != 0) fclose(stream);
  g_free(data); g_free(source); g_free(destination);
  trout_test_fixture_clear(&f);
}

/* This verifies failure without root privileges, because root can read 0000
 * files. The check is skipped explicitly if the environment cannot exercise it. */
static void
trout_test_unreadable_replacement(gconstpointer data)
{
  const gboolean *cross_device = data;
  trout_test_fixture f = {0};
  trout_file_operation operation = *cross_device ? trout_file_operation_move : trout_file_operation_copy;
  const char *target = *cross_device ? "peer/dst" : "dst";
  const trout_test_entry entries[] = {T_FILE("src", "new"), T_FILE(target, "old"), T_END};
  const trout_test_collision collisions[] = {T_LEAF("src", target, replace), T_CEND};
  trout_file_transfer_callbacks callbacks = {&f, trout_test_wait, trout_test_collision_callback, trout_test_progress};
  char *source = 0;
  TROUT_TEST_CHECK("fixture created", trout_test_fixture_init(&f));
  if (*cross_device && !trout_test_peer_init(&f)) goto exit;
  TROUT_TEST_CHECK("files created", trout_test_make_tree(&f, entries));
  source = trout_test_path(&f, "src");
  TROUT_TEST_CHECK("source made unreadable", chmod(source, 0000) == 0);
  if (access(source, R_OK) == 0) { g_test_skip("current user can read a mode-0000 file; run unprivileged"); goto exit; }
  f.collisions = collisions; f.expected_collisions = 1; f.seen = g_new0(gboolean, 1);
  TROUT_TEST_CHECK("pair added", trout_test_add_pair(&f, &(const trout_test_pair){"src", target}, operation));
  TROUT_TEST_CHECK("transfer fails", trout_file_transfer_execute_full(f.operations, operation, f.cancellable, &callbacks, &f.error) == trout_file_transfer_failed);
  TROUT_TEST_CHECK("permission denied", g_error_matches(f.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED));
  g_clear_error(&f.error);
  TROUT_TEST_CHECK("collision callback verified", f.callback_ok && f.calls == 1 && f.seen[0]);
  TROUT_TEST_CHECK("restore source mode", chmod(source, 0600) == 0);
  TROUT_TEST_CHECK("source and destination unchanged, no staging files", trout_test_check_tree(&f, entries));
exit:
  if (source != 0) chmod(source, 0600);
  g_free(source);
  trout_test_fixture_clear(&f);
}

/* A wide/deep tree exercises traversal completeness and verifies that the
 * transfer itself does not require one open directory descriptor per level. */
static void
trout_test_deep_and_wide(void)
{
  enum { depth = 48, width = 128, count = 1 + depth + width };
  trout_test_fixture f = {0};
  trout_test_entry *before = g_new0(trout_test_entry, count + 1);
  trout_test_entry *after = g_new0(trout_test_entry, 2 * count + 1);
  struct rlimit original, limited;
  gboolean limit_changed = FALSE;
  guint i;
  trout_file_transfer_result result;
  TROUT_TEST_CHECK("fixture created", trout_test_fixture_init(&f));
  before[0] = (trout_test_entry)T_DIR(g_strdup("src"));
  for (i = 1; i <= depth; i += 1)
    before[i] = (trout_test_entry)T_DIR(g_strdup_printf("%s/d", before[i - 1].path));
  for (i = depth + 1; i < count; i += 1)
    before[i] = (trout_test_entry)T_FILE(g_strdup_printf("src/file-%03u", i - depth - 1), "payload");
  for (i = 0; i < count; i += 1) {
    after[i] = before[i];
    after[count + i] = before[i];
    after[count + i].path = g_strdup_printf("dst%s", before[i].path + 3);
  }
  TROUT_TEST_CHECK("source tree created", trout_test_make_tree(&f, before));
  TROUT_TEST_CHECK("copy pair added", trout_test_add_pair(&f, &(const trout_test_pair){"src", "dst"}, trout_file_operation_copy));
  if (getrlimit(RLIMIT_NOFILE, &original) == 0 && original.rlim_cur > 32) {
    limited = original; limited.rlim_cur = 32;
    limit_changed = setrlimit(RLIMIT_NOFILE, &limited) == 0;
  }
  if (!limit_changed) g_test_message("descriptor limit unchanged; traversal still tested");
  result = trout_file_transfer_execute_full(f.operations, trout_file_operation_copy, 0, 0, &f.error);
  if (limit_changed) {
    TROUT_TEST_CHECK("descriptor limit restored", setrlimit(RLIMIT_NOFILE, &original) == 0);
    limit_changed = FALSE;
  }
  TROUT_TEST_CHECK("copy completes", result == trout_file_transfer_complete && f.error == 0);
  TROUT_TEST_CHECK("exact source and destination trees", trout_test_check_tree(&f, after));
  g_ptr_array_set_size(f.operations, 0);
  TROUT_TEST_CHECK("delete pair added", trout_test_add_pair(&f, &(const trout_test_pair){"dst", "dst"}, trout_file_operation_delete));
  TROUT_TEST_CHECK("tree deleted", trout_file_transfer_execute_full(f.operations, trout_file_operation_delete, 0, 0, &f.error) == trout_file_transfer_complete && f.error == 0);
  TROUT_TEST_CHECK("only the intact source tree remains", trout_test_check_tree(&f, before));
exit:
  if (limit_changed) setrlimit(RLIMIT_NOFILE, &original);
  for (i = 0; i < count; i += 1) {
    g_free((char *)before[i].path);
    g_free((char *)after[count + i].path);
  }
  g_free(before); g_free(after);
  trout_test_fixture_clear(&f);
}

int
main(int argc, char **argv)
{
  static const trout_file_operation copy = trout_file_operation_copy;
  static const trout_file_operation move = trout_file_operation_move;
  static const trout_file_operation rename = trout_file_operation_rename;
  static const gboolean local = FALSE, cross_device = TRUE;
  guint i;
  g_test_init(&argc, &argv, 0);
  g_test_set_nonfatal_assertions();
#ifndef TROUT_TRANSFER_TEST_STANDALONE
  g_test_add_func("/action/config-parse", trout_test_action_config_parse);
  g_test_add_func("/action/extension-matching", trout_test_action_extension_matching);
  g_test_add_func("/action/file-types", trout_test_action_file_types);
#endif
  for (i = 0; i < G_N_ELEMENTS(trout_test_scenarios); i += 1)
    g_test_add_data_func(trout_test_scenarios[i].name, &trout_test_scenarios[i], trout_test_run_scenario);
  g_test_add_func("/transfer/api/merge-policy-matrix", trout_test_merge_policy);
  g_test_add_func("/transfer/rename/invalid-names", trout_test_rename_invalid_names);
  g_test_add_data_func("/transfer/metadata/copy", &copy, trout_test_metadata);
  g_test_add_data_func("/transfer/metadata/native-move", &move, trout_test_metadata);
  g_test_add_data_func("/transfer/metadata/rename", &rename, trout_test_metadata);
  g_test_add_func("/transfer/metadata/merge-keeps-destination", trout_test_merge_keeps_directory_mode);
  g_test_add_func("/transfer/copy/binary", trout_test_binary_copy);
  g_test_add_data_func("/transfer/copy/unreadable-replacement", &local, trout_test_unreadable_replacement);
  g_test_add_data_func("/transfer/cross-device/unreadable-replacement", &cross_device, trout_test_unreadable_replacement);
  g_test_add_func("/transfer/tree/deep-and-wide", trout_test_deep_and_wide);
  return g_test_run();
}
