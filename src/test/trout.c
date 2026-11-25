#define trout_test_build
#include "../trout/trout.c"

#define status_t trout_test_status_t
#include <trout/sph/test.h>
#undef status_t

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean
trout_test_delete_tree(GFile *file, GError **error)
{
  GFileEnumerator *enumerator;
  GFileInfo *info;
  GFile *child;
  const char *name;
  info = g_file_query_info(file, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, 0, error);
  if (info == 0) return FALSE;
  if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
    g_object_unref(info);
    enumerator = g_file_enumerate_children(file, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, 0, error);
    if (enumerator == 0) return FALSE;
    while ((info = g_file_enumerator_next_file(enumerator, 0, error)) != 0) {
      name = g_file_info_get_name(info);
      child = g_file_get_child(file, name);
      g_object_unref(info);
      if (!trout_test_delete_tree(child, error)) {
        g_object_unref(child);
        g_object_unref(enumerator);
        return FALSE;
      }
      g_object_unref(child);
    }
    if (*error != 0) {
      g_object_unref(enumerator);
      return FALSE;
    }
    if (!g_file_enumerator_close(enumerator, 0, error)) {
      g_object_unref(enumerator);
      return FALSE;
    }
    g_object_unref(enumerator);
  } else {
    g_object_unref(info);
  }
  return g_file_delete(file, 0, error);
}

trout_test_status_t
trout_test_action_config_parse(void)
{
  trout_test_status_t status = {0};
  GPtrArray *actions;
  trout_action *action;
  char configuration[] = "convert cb fd png,webp converter --fast\nfolders c d * open-folder\n";
  status.group = "";
  actions = trout_action_config_parse(configuration);
  test_helper_assert("action count", actions->len == 2);
  action = g_ptr_array_index(actions, 0);
  test_helper_assert("action label", g_strcmp0(action->label, "convert") == 0);
  test_helper_assert("both appearances", action->appearance == (trout_action_appearance_context | trout_action_appearance_button));
  test_helper_assert("both file types", action->file_types == (trout_action_type_directories | trout_action_type_files));
  test_helper_assert("first extension", g_strcmp0(action->extensions[0], "png") == 0);
  test_helper_assert("second extension", g_strcmp0(action->extensions[1], "webp") == 0);
  test_helper_assert("specific extensions are not wildcard", !action->any_extension);
  test_helper_assert("command executable", g_strcmp0(action->command[0], "converter") == 0);
  test_helper_assert("command argument", g_strcmp0(action->command[1], "--fast") == 0);
  action = g_ptr_array_index(actions, 1);
  test_helper_assert("directory action appearance", action->appearance == trout_action_appearance_context);
  test_helper_assert("directory action type", action->file_types == trout_action_type_directories);
  test_helper_assert("wildcard extension", action->any_extension);
exit:
  g_ptr_array_unref(actions);
  return status;
}

trout_test_status_t
trout_test_action_extension_matching(void)
{
  trout_test_status_t status = {0};
  trout_action action = {0};
  GFileInfo *file_info;
  GFile *file;
  char *extensions[] = {"txt", 0};
  status.group = "";
  file_info = g_file_info_new();
  file = g_file_new_for_path("/tmp/readme.txt");
  action.file_types = trout_action_type_files;
  action.extensions = extensions;
  g_file_info_set_attribute_object(file_info, "standard::file", G_OBJECT(file));
  g_file_info_set_file_type(file_info, G_FILE_TYPE_REGULAR);
  g_file_info_set_name(file_info, "readme.txt");
  test_helper_assert("matching extension", trout_action_matches_file(&action, file_info));
  g_file_info_set_name(file_info, "readme.TXT");
  test_helper_assert("extension matching is case-sensitive", !trout_action_matches_file(&action, file_info));
  g_file_info_set_name(file_info, "readme");
  test_helper_assert("extensionless name does not match", !trout_action_matches_file(&action, file_info));
  g_file_info_set_name(file_info, ".txt");
  test_helper_assert("leading dot is not an extension", !trout_action_matches_file(&action, file_info));
  action.any_extension = TRUE;
  test_helper_assert("wildcard matches extensionless name", trout_action_matches_file(&action, file_info));
exit:
  g_object_unref(file_info);
  g_object_unref(file);
  return status;
}

trout_test_status_t
trout_test_action_file_types(void)
{
  trout_test_status_t status = {0};
  trout_action action = {0};
  GFileInfo *file_info;
  GFile *file;
  GFile *remote_file;
  char *extensions[] = {"*", 0};
  status.group = "";
  file_info = g_file_info_new();
  file = g_file_new_for_path("/tmp/entry");
  action.any_extension = TRUE;
  action.file_types = trout_action_type_files;
  action.extensions = extensions;
  g_file_info_set_attribute_object(file_info, "standard::file", G_OBJECT(file));
  g_file_info_set_file_type(file_info, G_FILE_TYPE_REGULAR);
  g_file_info_set_name(file_info, "entry");
  test_helper_assert("file action matches file", trout_action_matches_file(&action, file_info));
  action.file_types = trout_action_type_directories;
  test_helper_assert("directory action rejects file", !trout_action_matches_file(&action, file_info));
  g_file_info_set_file_type(file_info, G_FILE_TYPE_DIRECTORY);
  test_helper_assert("directory action matches directory", trout_action_matches_file(&action, file_info));
  action.file_types = trout_action_type_directories | trout_action_type_files;
  remote_file = g_file_new_for_uri("sftp://example.invalid/entry");
  g_file_info_set_attribute_object(file_info, "standard::file", G_OBJECT(remote_file));
  g_object_unref(remote_file);
  test_helper_assert("custom action rejects non-native path", !trout_action_matches_file(&action, file_info));
exit:
  g_object_unref(file_info);
  g_object_unref(file);
  return status;
}

trout_test_status_t
trout_test_transfer_destination_guard(void)
{
  trout_test_status_t status = {0};
  GFile *source;
  GFile *child;
  GFile *sibling;
  GFile *parent;
  status.group = "";
  source = g_file_new_for_path("/tmp/source");
  child = g_file_new_for_path("/tmp/source/child");
  sibling = g_file_new_for_path("/tmp/source-copy");
  parent = g_file_new_for_path("/tmp");
  test_helper_assert("rejects destination below source", g_file_has_prefix(child, source));
  test_helper_assert("allows sibling destination", !g_file_has_prefix(sibling, source));
  test_helper_assert("allows parent destination", !g_file_has_prefix(parent, source));
exit:
  g_object_unref(source);
  g_object_unref(child);
  g_object_unref(sibling);
  g_object_unref(parent);
  return status;
}

trout_test_status_t
trout_test_transfer_merge_policy(void)
{
  trout_test_status_t status = {0};
  status.group = "";
  test_helper_assert("directory transfers merge existing directories", trout_file_transfer_should_merge(G_FILE_TYPE_DIRECTORY, G_FILE_TYPE_DIRECTORY));
  test_helper_assert("directory transfers do not merge into files", !trout_file_transfer_should_merge(G_FILE_TYPE_DIRECTORY, G_FILE_TYPE_REGULAR));
  test_helper_assert("file transfers do not merge", !trout_file_transfer_should_merge(G_FILE_TYPE_REGULAR, G_FILE_TYPE_REGULAR));
exit:
  return status;
}

trout_test_status_t
trout_test_file_delete_tree(void)
{
  trout_test_status_t status = {0};
  char *directory;
  char *remove_directory;
  char *nested_directory;
  char *nested_file;
  char *target_directory;
  char *target_file;
  char *link_path;
  char *contents;
  GFile *root_file;
  GFile *remove_file;
  GFile *link_file;
  GError *error;
  status.group = "";
  directory = 0;
  remove_directory = 0;
  nested_directory = 0;
  nested_file = 0;
  target_directory = 0;
  target_file = 0;
  link_path = 0;
  contents = 0;
  root_file = 0;
  remove_file = 0;
  link_file = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-delete-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  remove_directory = g_build_filename(directory, "remove", (char *)0);
  nested_directory = g_build_filename(remove_directory, "nested", (char *)0);
  nested_file = g_build_filename(nested_directory, "file", (char *)0);
  test_helper_assert("nested directory created", g_mkdir_with_parents(nested_directory, 0700) == 0);
  test_helper_assert("nested file created", g_file_set_contents(nested_file, "content", -1, &error));
  g_clear_error(&error);
  remove_file = g_file_new_for_path(remove_directory);
  test_helper_assert("nested tree deleted", trout_test_delete_tree(remove_file, &error));
  test_helper_assert("nested tree no longer exists", !g_file_test(remove_directory, G_FILE_TEST_EXISTS));
  target_directory = g_build_filename(directory, "target", (char *)0);
  target_file = g_build_filename(target_directory, "file", (char *)0);
  link_path = g_build_filename(directory, "link", (char *)0);
  test_helper_assert("link target directory created", g_mkdir_with_parents(target_directory, 0700) == 0);
  test_helper_assert("link target file created", g_file_set_contents(target_file, "preserved", -1, &error));
  g_clear_error(&error);
  link_file = g_file_new_for_path(link_path);
  test_helper_assert("symbolic link created", g_file_make_symbolic_link(link_file, target_directory, 0, &error));
  test_helper_assert("symbolic link deleted", trout_test_delete_tree(link_file, &error));
  test_helper_assert("symbolic link no longer exists", !g_file_test(link_path, G_FILE_TEST_IS_SYMLINK));
  test_helper_assert("link target remains", g_file_get_contents(target_file, &contents, 0, &error));
  test_helper_assert("link target contents remain", g_strcmp0(contents, "preserved") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&remove_file);
  g_clear_object(&link_file);
  g_free(directory);
  g_free(remove_directory);
  g_free(nested_directory);
  g_free(nested_file);
  g_free(target_directory);
  g_free(target_file);
  g_free(link_path);
  g_free(contents);
  return status;
}

typedef struct {
  trout_file_collision_choice choice;
  GFileType source_type;
  GFileType destination_type;
  guint collision_count;
  guint directory_collision_count;
  guint file_collision_count;
  gboolean merge_directories;
  gboolean replace_destination_on_collision;
  gboolean callback_ok;
} trout_test_file_transfer_response;

gboolean
trout_test_file_transfer_wait(gpointer user_data)
{
  (void)user_data;
  return TRUE;
}

trout_file_collision_choice
trout_test_file_transfer_collision(GFile *source, GFile *destination, GFile *source_root, GFile *destination_root, GFileType source_type, GFileType destination_type, goffset source_size, goffset destination_size, gpointer user_data)
{
  trout_test_file_transfer_response *response;
  guint choice_index;
  char *destination_path;
  char *backup_path;
  response = user_data;
  choice_index = response->collision_count;
  response->source_type = source_type;
  response->destination_type = destination_type;
  response->collision_count += 1;
  if (trout_file_transfer_should_merge(source_type, destination_type)) response->directory_collision_count += 1;
  else response->file_collision_count += 1;
  if (response->replace_destination_on_collision && choice_index == 0) {
    destination_path = g_file_get_path(destination);
    backup_path = g_strdup_printf("%s.old", destination_path);
    response->callback_ok = rename(destination_path, backup_path) == 0 && g_file_set_contents(destination_path, "racer", -1, 0);
    g_free(destination_path);
    g_free(backup_path);
  }
  (void)source;
  (void)source_root;
  (void)destination_root;
  (void)source_size;
  (void)destination_size;
  if (response->merge_directories && trout_file_transfer_should_merge(source_type, destination_type)) return trout_file_collision_merge;
  return response->choice;
}

trout_file_transfer_result
trout_test_file_transfer_run_full(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, trout_test_file_transfer_response *response, GError **error)
{
  trout_file_transfer_callbacks callbacks;
  callbacks.user_data = response;
  callbacks.wait = trout_test_file_transfer_wait;
  callbacks.collision = trout_test_file_transfer_collision;
  callbacks.progress = 0;
  return trout_file_transfer_execute_full(operations, operation, cancellable, &callbacks, error);
}

gboolean
trout_test_file_transfer_run(GPtrArray *operations, trout_file_operation operation, GCancellable *cancellable, trout_test_file_transfer_response *response, GError **error)
{
  trout_file_transfer_callbacks callbacks;
  callbacks.user_data = response;
  callbacks.wait = trout_test_file_transfer_wait;
  callbacks.collision = trout_test_file_transfer_collision;
  callbacks.progress = 0;
  return trout_file_transfer_execute(operations, operation, cancellable, &callbacks, error);
}

void
trout_test_file_transfer_add(GPtrArray *operations, char *source_path, char *destination_path)
{
  GFile *source;
  GFile *destination;
  trout_file_transfer_pair *pair;
  source = g_file_new_for_path(source_path);
  destination = g_file_new_for_path(destination_path);
  pair = trout_file_transfer_pair_new(source, destination);
  g_object_unref(source);
  g_object_unref(destination);
  g_ptr_array_add(operations, pair);
}

trout_test_status_t
trout_test_file_transfer_merges_directories(void)
{
  trout_test_status_t status = {0};
  trout_test_file_transfer_response response = {0};
  char *directory;
  char *source_directory;
  char *destination_directory;
  char *source_nested;
  char *destination_nested;
  char *source_added_directory;
  char *destination_added_directory;
  char *source_new_file;
  char *source_nested_file;
  char *source_added_file;
  char *source_conflict_file;
  char *destination_keep_file;
  char *destination_nested_file;
  char *destination_added_file;
  char *destination_conflict_file;
  char *destination_new_file;
  char *destination_new_nested_file;
  char *contents;
  GFile *root_file;
  GPtrArray *operations;
  GCancellable *cancellable;
  GError *error;
  trout_file_transfer_result result;
  status.group = "";
  directory = 0;
  source_directory = 0;
  destination_directory = 0;
  source_nested = 0;
  destination_nested = 0;
  source_added_directory = 0;
  destination_added_directory = 0;
  source_new_file = 0;
  source_nested_file = 0;
  source_added_file = 0;
  source_conflict_file = 0;
  destination_keep_file = 0;
  destination_nested_file = 0;
  destination_added_file = 0;
  destination_conflict_file = 0;
  destination_new_file = 0;
  destination_new_nested_file = 0;
  contents = 0;
  root_file = 0;
  operations = 0;
  cancellable = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-merge-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  source_directory = g_build_filename(directory, "source", "bundle", (char *)0);
  destination_directory = g_build_filename(directory, "destination", "bundle", (char *)0);
  source_nested = g_build_filename(source_directory, "nested", (char *)0);
  destination_nested = g_build_filename(destination_directory, "nested", (char *)0);
  source_added_directory = g_build_filename(source_directory, "added", (char *)0);
  destination_added_directory = g_build_filename(destination_directory, "added", (char *)0);
  test_helper_assert("source tree created", g_mkdir_with_parents(source_nested, 0700) == 0);
  test_helper_assert("source-only subtree created", g_mkdir_with_parents(source_added_directory, 0700) == 0);
  test_helper_assert("destination tree created", g_mkdir_with_parents(destination_nested, 0700) == 0);
  source_new_file = g_build_filename(source_directory, "new.txt", (char *)0);
  source_nested_file = g_build_filename(source_nested, "new.txt", (char *)0);
  source_added_file = g_build_filename(source_added_directory, "new.txt", (char *)0);
  source_conflict_file = g_build_filename(source_directory, "collision.txt", (char *)0);
  destination_keep_file = g_build_filename(destination_directory, "keep.txt", (char *)0);
  destination_nested_file = g_build_filename(destination_nested, "keep.txt", (char *)0);
  destination_added_file = g_build_filename(destination_added_directory, "new.txt", (char *)0);
  destination_conflict_file = g_build_filename(destination_directory, "collision.txt", (char *)0);
  destination_new_file = g_build_filename(destination_directory, "new.txt", (char *)0);
  destination_new_nested_file = g_build_filename(destination_nested, "new.txt", (char *)0);
  test_helper_assert("source file created", g_file_set_contents(source_new_file, "new", -1, &error));
  test_helper_assert("nested source file created", g_file_set_contents(source_nested_file, "nested", -1, &error));
  test_helper_assert("source-only nested file created", g_file_set_contents(source_added_file, "added", -1, &error));
  test_helper_assert("conflicting source file created", g_file_set_contents(source_conflict_file, "source", -1, &error));
  test_helper_assert("existing destination file created", g_file_set_contents(destination_keep_file, "keep", -1, &error));
  test_helper_assert("existing nested file created", g_file_set_contents(destination_nested_file, "nested keep", -1, &error));
  test_helper_assert("conflicting destination file created", g_file_set_contents(destination_conflict_file, "destination", -1, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  trout_test_file_transfer_add(operations, source_directory, destination_directory);
  cancellable = g_cancellable_new();
  response.merge_directories = TRUE;
  response.choice = trout_file_collision_skip;
  result = trout_test_file_transfer_run_full(operations, trout_file_operation_copy, cancellable, &response, &error);
  test_helper_assert("directory merge reports a partial result after a skipped file", result == trout_file_transfer_partial && error == 0);
  test_helper_assert("directory and file collisions were reported", response.collision_count == 3);
  test_helper_assert("root and nested directories merged", response.directory_collision_count == 2);
  test_helper_assert("file collision was reported", response.file_collision_count == 1);
  test_helper_assert("new file copied", g_file_get_contents(destination_new_file, &contents, 0, &error));
  test_helper_assert("new file contents match", g_strcmp0(contents, "new") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("nested new file copied", g_file_get_contents(destination_new_nested_file, &contents, 0, &error));
  test_helper_assert("nested new file contents match", g_strcmp0(contents, "nested") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("source-only subtree copied", g_file_get_contents(destination_added_file, &contents, 0, &error));
  test_helper_assert("source-only subtree contents match", g_strcmp0(contents, "added") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("existing root file preserved", g_file_get_contents(destination_keep_file, &contents, 0, &error));
  test_helper_assert("existing root contents preserved", g_strcmp0(contents, "keep") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("existing nested file preserved", g_file_get_contents(destination_nested_file, &contents, 0, &error));
  test_helper_assert("existing nested contents preserved", g_strcmp0(contents, "nested keep") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("skip preserved colliding file", g_file_get_contents(destination_conflict_file, &contents, 0, &error));
  test_helper_assert("skip kept destination contents", g_strcmp0(contents, "destination") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&cancellable);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(source_directory);
  g_free(destination_directory);
  g_free(source_nested);
  g_free(destination_nested);
  g_free(source_added_directory);
  g_free(destination_added_directory);
  g_free(source_new_file);
  g_free(source_nested_file);
  g_free(source_added_file);
  g_free(source_conflict_file);
  g_free(destination_keep_file);
  g_free(destination_nested_file);
  g_free(destination_added_file);
  g_free(destination_conflict_file);
  g_free(destination_new_file);
  g_free(destination_new_nested_file);
  g_free(contents);
  return status;
}

trout_test_status_t
trout_test_file_transfer_replace_and_cancel(void)
{
  trout_test_status_t status = {0};
  trout_test_file_transfer_response response = {0};
  char *directory;
  char *source_file;
  char *destination_file;
  char *destination_directory;
  char *destination_nested;
  char *destination_nested_file;
  char *source_directory;
  char *source_directory_file;
  char *directory_destination;
  char *directory_destination_file;
  char *cancel_source;
  char *cancel_destination;
  char *cancel_source_two;
  char *cancel_destination_two;
  char *contents;
  GFile *root_file;
  GPtrArray *operations;
  GCancellable *cancellable;
  GError *error;
  status.group = "";
  directory = 0;
  source_file = 0;
  destination_file = 0;
  destination_directory = 0;
  destination_nested = 0;
  destination_nested_file = 0;
  source_directory = 0;
  source_directory_file = 0;
  directory_destination = 0;
  directory_destination_file = 0;
  cancel_source = 0;
  cancel_destination = 0;
  cancel_source_two = 0;
  cancel_destination_two = 0;
  contents = 0;
  root_file = 0;
  operations = 0;
  cancellable = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-replace-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  source_file = g_build_filename(directory, "source.txt", (char *)0);
  destination_file = g_build_filename(directory, "destination.txt", (char *)0);
  destination_directory = g_build_filename(directory, "destination-directory", (char *)0);
  destination_nested = g_build_filename(destination_directory, "nested", (char *)0);
  destination_nested_file = g_build_filename(destination_nested, "old.txt", (char *)0);
  source_directory = g_build_filename(directory, "source-directory", (char *)0);
  source_directory_file = g_build_filename(source_directory, "new.txt", (char *)0);
  directory_destination = g_build_filename(directory, "directory-destination", (char *)0);
  directory_destination_file = g_build_filename(directory_destination, "new.txt", (char *)0);
  cancel_source = g_build_filename(directory, "cancel-source.txt", (char *)0);
  cancel_destination = g_build_filename(directory, "cancel-destination.txt", (char *)0);
  cancel_source_two = g_build_filename(directory, "cancel-source-two.txt", (char *)0);
  cancel_destination_two = g_build_filename(directory, "cancel-destination-two.txt", (char *)0);
  test_helper_assert("source created", g_file_set_contents(source_file, "replacement", -1, &error));
  test_helper_assert("existing file created", g_file_set_contents(destination_file, "old file", -1, &error));
  test_helper_assert("existing directory created", g_mkdir_with_parents(destination_nested, 0700) == 0);
  test_helper_assert("existing nested file created", g_file_set_contents(destination_nested_file, "old directory", -1, &error));
  test_helper_assert("source directory created", g_mkdir_with_parents(source_directory, 0700) == 0);
  test_helper_assert("source directory file created", g_file_set_contents(source_directory_file, "new directory", -1, &error));
  test_helper_assert("directory destination file created", g_file_set_contents(directory_destination, "old file", -1, &error));
  test_helper_assert("cancel source created", g_file_set_contents(cancel_source, "cancel source", -1, &error));
  test_helper_assert("cancel destination created", g_file_set_contents(cancel_destination, "cancel destination", -1, &error));
  test_helper_assert("second cancel source created", g_file_set_contents(cancel_source_two, "second source", -1, &error));
  test_helper_assert("second cancel destination created", g_file_set_contents(cancel_destination_two, "second destination", -1, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  trout_test_file_transfer_add(operations, source_file, destination_file);
  cancellable = g_cancellable_new();
  response.choice = trout_file_collision_replace;
  test_helper_assert("file replacement succeeds", trout_test_file_transfer_run(operations, trout_file_operation_copy, cancellable, &response, &error));
  test_helper_assert("file collision was reported", response.collision_count == 1);
  test_helper_assert("file contents replaced", g_file_get_contents(destination_file, &contents, 0, &error));
  test_helper_assert("replacement contents match", g_strcmp0(contents, "replacement") == 0);
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  trout_test_file_transfer_add(operations, source_file, destination_directory);
  response.collision_count = 0;
  test_helper_assert("directory replacement is refused", !trout_test_file_transfer_run(operations, trout_file_operation_copy, cancellable, &response, &error));
  test_helper_assert("directory collision was reported", response.collision_count == 1);
  test_helper_assert("directory replacement refusal is reported", g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED));
  g_clear_error(&error);
  test_helper_assert("destination directory remains", g_file_test(destination_directory, G_FILE_TEST_IS_DIR));
  test_helper_assert("existing directory contents remain", g_file_get_contents(destination_nested_file, &contents, 0, &error));
  test_helper_assert("existing directory contents are unchanged", g_strcmp0(contents, "old directory") == 0);
  g_clear_pointer(&contents, g_free);
  g_clear_error(&error);
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  trout_test_file_transfer_add(operations, source_directory, directory_destination);
  response.collision_count = 0;
  test_helper_assert("directory cannot replace a file", !trout_test_file_transfer_run(operations, trout_file_operation_copy, cancellable, &response, &error));
  test_helper_assert("type collision was reported", response.collision_count == 1);
  test_helper_assert("directory replacement is refused", g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED));
  g_clear_error(&error);
  test_helper_assert("source directory remains", g_file_test(source_directory, G_FILE_TEST_IS_DIR));
  test_helper_assert("destination file remains", g_file_get_contents(directory_destination, &contents, 0, &error));
  test_helper_assert("destination contents remain intact", g_strcmp0(contents, "old file") == 0);
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  trout_test_file_transfer_add(operations, cancel_source, cancel_destination);
  trout_test_file_transfer_add(operations, cancel_source_two, cancel_destination_two);
  response.choice = trout_file_collision_cancel;
  response.collision_count = 0;
  test_helper_assert("cancel stops the transfer", !trout_test_file_transfer_run(operations, trout_file_operation_copy, cancellable, &response, &error));
  test_helper_assert("cancel reports a collision", response.collision_count == 1);
  test_helper_assert("cancel returns cancellation error", g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
  g_clear_error(&error);
  test_helper_assert("cancel preserves destination", g_file_get_contents(cancel_destination, &contents, 0, &error));
  test_helper_assert("cancel leaves destination contents", g_strcmp0(contents, "cancel destination") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("cancel leaves later destination untouched", g_file_get_contents(cancel_destination_two, &contents, 0, &error));
  test_helper_assert("later destination contents stay unchanged", g_strcmp0(contents, "second destination") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&cancellable);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(source_file);
  g_free(destination_file);
  g_free(destination_directory);
  g_free(destination_nested);
  g_free(destination_nested_file);
  g_free(source_directory);
  g_free(source_directory_file);
  g_free(directory_destination);
  g_free(directory_destination_file);
  g_free(cancel_source);
  g_free(cancel_destination);
  g_free(cancel_source_two);
  g_free(cancel_destination_two);
  g_free(contents);
  return status;
}

trout_test_status_t
trout_test_file_transfer_move_collisions(void)
{
  trout_test_status_t status = {0};
  trout_test_file_transfer_response response = {0};
  char *directory;
  char *source_file;
  char *destination_file;
  char *source_directory;
  char *source_directory_file;
  char *source_nested_directory;
  char *source_nested_file;
  char *source_nested_unique_file;
  char *destination_directory;
  char *destination_directory_file;
  char *destination_old_file;
  char *destination_nested_directory;
  char *destination_nested_file;
  char *destination_nested_unique_file;
  char *destination_nested_keep_file;
  char *contents;
  GFile *root_file;
  GPtrArray *operations;
  GCancellable *cancellable;
  GError *error;
  status.group = "";
  directory = 0;
  source_file = 0;
  destination_file = 0;
  source_directory = 0;
  source_directory_file = 0;
  source_nested_directory = 0;
  source_nested_file = 0;
  source_nested_unique_file = 0;
  destination_directory = 0;
  destination_directory_file = 0;
  destination_old_file = 0;
  destination_nested_directory = 0;
  destination_nested_file = 0;
  destination_nested_unique_file = 0;
  destination_nested_keep_file = 0;
  contents = 0;
  root_file = 0;
  operations = 0;
  cancellable = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-move-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  source_file = g_build_filename(directory, "source.txt", (char *)0);
  destination_file = g_build_filename(directory, "destination.txt", (char *)0);
  source_directory = g_build_filename(directory, "source-directory", (char *)0);
  source_directory_file = g_build_filename(source_directory, "new.txt", (char *)0);
  source_nested_directory = g_build_filename(source_directory, "nested", (char *)0);
  source_nested_file = g_build_filename(source_nested_directory, "new.txt", (char *)0);
  source_nested_unique_file = g_build_filename(source_nested_directory, "added.txt", (char *)0);
  destination_directory = g_build_filename(directory, "destination-directory", (char *)0);
  destination_directory_file = g_build_filename(destination_directory, "new.txt", (char *)0);
  destination_old_file = g_build_filename(destination_directory, "old.txt", (char *)0);
  destination_nested_directory = g_build_filename(destination_directory, "nested", (char *)0);
  destination_nested_file = g_build_filename(destination_nested_directory, "new.txt", (char *)0);
  destination_nested_unique_file = g_build_filename(destination_nested_directory, "added.txt", (char *)0);
  destination_nested_keep_file = g_build_filename(destination_nested_directory, "keep.txt", (char *)0);
  test_helper_assert("source file created", g_file_set_contents(source_file, "new file", -1, &error));
  test_helper_assert("destination file created", g_file_set_contents(destination_file, "old file", -1, &error));
  test_helper_assert("source directory created", g_mkdir_with_parents(source_nested_directory, 0700) == 0);
  test_helper_assert("source directory file created", g_file_set_contents(source_directory_file, "new directory", -1, &error));
  test_helper_assert("source nested file created", g_file_set_contents(source_nested_file, "new nested", -1, &error));
  test_helper_assert("source nested unique file created", g_file_set_contents(source_nested_unique_file, "added nested", -1, &error));
  test_helper_assert("destination directory created", g_mkdir_with_parents(destination_nested_directory, 0700) == 0);
  test_helper_assert("destination directory file created", g_file_set_contents(destination_old_file, "old directory", -1, &error));
  test_helper_assert("destination conflicting file created", g_file_set_contents(destination_directory_file, "old conflicting file", -1, &error));
  test_helper_assert("destination nested conflicting file created", g_file_set_contents(destination_nested_file, "old nested", -1, &error));
  test_helper_assert("destination nested keep file created", g_file_set_contents(destination_nested_keep_file, "nested keep", -1, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  cancellable = g_cancellable_new();
  trout_test_file_transfer_add(operations, source_file, destination_file);
  response.choice = trout_file_collision_skip;
  test_helper_assert("move skip is reported", trout_test_file_transfer_run_full(operations, trout_file_operation_move, cancellable, &response, &error) == trout_file_transfer_skipped);
  test_helper_assert("skipped file move has no error", error == 0);
  test_helper_assert("file move collision was reported", response.collision_count == 1);
  test_helper_assert("file move types were reported", response.source_type == G_FILE_TYPE_REGULAR && response.destination_type == G_FILE_TYPE_REGULAR);
  test_helper_assert("skipped source remains", g_file_test(source_file, G_FILE_TEST_IS_REGULAR));
  test_helper_assert("skipped destination remains", g_file_get_contents(destination_file, &contents, 0, &error));
  test_helper_assert("skipped destination contents remain", g_strcmp0(contents, "old file") == 0);
  g_clear_pointer(&contents, g_free);
  response.choice = trout_file_collision_replace;
  response.collision_count = 0;
  test_helper_assert("file move replacement succeeds", trout_test_file_transfer_run(operations, trout_file_operation_move, cancellable, &response, &error));
  test_helper_assert("file move replacement collision was reported", response.collision_count == 1);
  test_helper_assert("file move source removed", !g_file_test(source_file, G_FILE_TEST_EXISTS));
  test_helper_assert("file move destination replaced", g_file_get_contents(destination_file, &contents, 0, &error));
  test_helper_assert("file move contents match", g_strcmp0(contents, "new file") == 0);
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  trout_test_file_transfer_add(operations, source_directory, destination_directory);
  response.collision_count = 0;
  response.directory_collision_count = 0;
  response.file_collision_count = 0;
  response.merge_directories = TRUE;
  response.choice = trout_file_collision_skip;
  test_helper_assert("directory move with skipped conflicts is partial", trout_test_file_transfer_run_full(operations, trout_file_operation_move, cancellable, &response, &error) == trout_file_transfer_partial);
  test_helper_assert("partial directory move has no error", error == 0);
  test_helper_assert("directory and file collisions were reported", response.collision_count == 4);
  test_helper_assert("root and nested directories merged", response.directory_collision_count == 2);
  test_helper_assert("conflicting files were reported", response.file_collision_count == 2);
  test_helper_assert("skipped source directory remains", g_file_test(source_directory, G_FILE_TEST_IS_DIR));
  test_helper_assert("skipped destination directory remains", g_file_test(destination_directory, G_FILE_TEST_IS_DIR));
  test_helper_assert("skipped source file remains", g_file_test(source_directory_file, G_FILE_TEST_IS_REGULAR));
  test_helper_assert("skipped destination file remains", g_file_get_contents(destination_directory_file, &contents, 0, &error));
  test_helper_assert("skipped destination contents remain", g_strcmp0(contents, "old conflicting file") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("skipped nested source file remains", g_file_test(source_nested_file, G_FILE_TEST_IS_REGULAR));
  test_helper_assert("nested unique source file was moved", !g_file_test(source_nested_unique_file, G_FILE_TEST_EXISTS));
  test_helper_assert("nested destination conflict remains", g_file_get_contents(destination_nested_file, &contents, 0, &error));
  test_helper_assert("nested destination contents remain", g_strcmp0(contents, "old nested") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("nested unique file arrives at destination", g_file_get_contents(destination_nested_unique_file, &contents, 0, &error));
  test_helper_assert("nested moved contents match", g_strcmp0(contents, "added nested") == 0);
  g_clear_pointer(&contents, g_free);
  response.collision_count = 0;
  response.choice = trout_file_collision_replace;
  test_helper_assert("directory move replaces conflicting files", trout_test_file_transfer_run_full(operations, trout_file_operation_move, cancellable, &response, &error) == trout_file_transfer_complete);
  test_helper_assert("directory and file replacements were reported", response.collision_count == 4);
  test_helper_assert("directory move source is removed when empty", !g_file_test(source_directory, G_FILE_TEST_EXISTS));
  test_helper_assert("directory move destination remains", g_file_test(destination_directory, G_FILE_TEST_IS_DIR));
  test_helper_assert("directory move installs the source file", g_file_get_contents(destination_directory_file, &contents, 0, &error));
  test_helper_assert("directory move uses source contents", g_strcmp0(contents, "new directory") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("directory move installs the nested source file", g_file_get_contents(destination_nested_file, &contents, 0, &error));
  test_helper_assert("directory move nested contents match", g_strcmp0(contents, "new nested") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("directory move preserves unrelated destination files", g_file_get_contents(destination_old_file, &contents, 0, &error));
  test_helper_assert("directory move preserves old contents", g_strcmp0(contents, "old directory") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("directory move preserves nested destination files", g_file_get_contents(destination_nested_keep_file, &contents, 0, &error));
  test_helper_assert("directory move preserves nested contents", g_strcmp0(contents, "nested keep") == 0);
  g_clear_pointer(&contents, g_free);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&cancellable);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(source_file);
  g_free(destination_file);
  g_free(source_directory);
  g_free(source_directory_file);
  g_free(source_nested_directory);
  g_free(source_nested_file);
  g_free(source_nested_unique_file);
  g_free(destination_directory);
  g_free(destination_directory_file);
  g_free(destination_old_file);
  g_free(destination_nested_directory);
  g_free(destination_nested_file);
  g_free(destination_nested_unique_file);
  g_free(destination_nested_keep_file);
  g_free(contents);
  return status;
}

trout_test_status_t
trout_test_file_transfer_rejects_aliases(void)
{
  trout_test_status_t status = {0};
  trout_test_file_transfer_response response = {0};
  char *directory;
  char *source_directory;
  char *source_child;
  char *source_keep;
  char *alias_path;
  char *alias_destination;
  char *source_file;
  char *hardlink_path;
  char *sibling_destination;
  char *sibling_file;
  char *contents;
  GFile *root_file;
  GFile *alias_file;
  GPtrArray *operations;
  GCancellable *cancellable;
  GError *error;
  trout_file_transfer_result result;
  status.group = "";
  directory = 0;
  source_directory = 0;
  source_child = 0;
  source_keep = 0;
  alias_path = 0;
  alias_destination = 0;
  source_file = 0;
  hardlink_path = 0;
  sibling_destination = 0;
  sibling_file = 0;
  contents = 0;
  root_file = 0;
  alias_file = 0;
  operations = 0;
  cancellable = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-alias-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  source_directory = g_build_filename(directory, "source", (char *)0);
  source_child = g_build_filename(source_directory, "child", (char *)0);
  source_keep = g_build_filename(source_child, "keep", (char *)0);
  alias_path = g_build_filename(directory, "alias", (char *)0);
  alias_destination = g_build_filename(alias_path, "new", (char *)0);
  source_file = g_build_filename(directory, "hardlink-source", (char *)0);
  hardlink_path = g_build_filename(directory, "hardlink-destination", (char *)0);
  sibling_destination = g_build_filename(directory, "source-copy", (char *)0);
  sibling_file = g_build_filename(sibling_destination, "child", "keep", (char *)0);
  test_helper_assert("source child created", g_mkdir_with_parents(source_child, 0700) == 0);
  test_helper_assert("source file created", g_file_set_contents(source_keep, "keep", -1, &error));
  alias_file = g_file_new_for_path(alias_path);
  test_helper_assert("directory alias created", g_file_make_symbolic_link(alias_file, source_child, 0, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  cancellable = g_cancellable_new();
  trout_test_file_transfer_add(operations, source_directory, alias_destination);
  result = trout_test_file_transfer_run_full(operations, trout_file_operation_copy, cancellable, &response, &error);
  test_helper_assert("copy through a directory alias is rejected", result == trout_file_transfer_failed);
  test_helper_assert("directory alias reports recursive overlap", g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE));
  g_clear_error(&error);
  test_helper_assert("source survives alias rejection", g_file_get_contents(source_keep, &contents, 0, &error));
  test_helper_assert("source contents survive alias rejection", g_strcmp0(contents, "keep") == 0);
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  test_helper_assert("hardlink source created", g_file_set_contents(source_file, "shared", -1, &error));
  test_helper_assert("hardlink alias created", link(source_file, hardlink_path) == 0);
  trout_test_file_transfer_add(operations, source_file, hardlink_path);
  result = trout_test_file_transfer_run_full(operations, trout_file_operation_copy, cancellable, &response, &error);
  test_helper_assert("copy onto a hardlink alias is rejected", result == trout_file_transfer_failed);
  test_helper_assert("hardlink alias reports recursive overlap", g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE));
  g_clear_error(&error);
  test_helper_assert("hardlink source remains", g_file_get_contents(source_file, &contents, 0, &error));
  test_helper_assert("hardlink source contents remain", g_strcmp0(contents, "shared") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("hardlink destination remains", g_file_get_contents(hardlink_path, &contents, 0, &error));
  test_helper_assert("hardlink destination contents remain", g_strcmp0(contents, "shared") == 0);
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  trout_test_file_transfer_add(operations, source_directory, sibling_destination);
  result = trout_test_file_transfer_run_full(operations, trout_file_operation_copy, cancellable, &response, &error);
  test_helper_assert("prefix sibling copy completes", result == trout_file_transfer_complete && error == 0);
  test_helper_assert("prefix sibling receives source contents", g_file_get_contents(sibling_file, &contents, 0, &error));
  test_helper_assert("prefix sibling contents match", g_strcmp0(contents, "keep") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&alias_file);
  g_clear_object(&cancellable);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(source_directory);
  g_free(source_child);
  g_free(source_keep);
  g_free(alias_path);
  g_free(alias_destination);
  g_free(source_file);
  g_free(hardlink_path);
  g_free(sibling_destination);
  g_free(sibling_file);
  g_free(contents);
  return status;
}

trout_test_status_t
trout_test_file_transfer_cancel_and_preserve_race(void)
{
  trout_test_status_t status = {0};
  trout_test_file_transfer_response response = {0};
  char *directory;
  char *first_source;
  char *first_destination;
  char *second_source;
  char *second_destination;
  char *third_source;
  char *third_destination;
  char *race_source;
  char *race_destination;
  char *race_backup;
  char *contents;
  GFile *root_file;
  GPtrArray *operations;
  GCancellable *cancellable;
  GError *error;
  trout_file_transfer_result result;
  status.group = "";
  directory = 0;
  first_source = 0;
  first_destination = 0;
  second_source = 0;
  second_destination = 0;
  third_source = 0;
  third_destination = 0;
  race_source = 0;
  race_destination = 0;
  race_backup = 0;
  contents = 0;
  root_file = 0;
  operations = 0;
  cancellable = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-transfer-race-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  first_source = g_build_filename(directory, "first", (char *)0);
  first_destination = g_build_filename(directory, "destination-first", (char *)0);
  second_source = g_build_filename(directory, "second", (char *)0);
  second_destination = g_build_filename(directory, "destination-second", (char *)0);
  third_source = g_build_filename(directory, "third", (char *)0);
  third_destination = g_build_filename(directory, "destination-third", (char *)0);
  race_source = g_build_filename(directory, "race-source", (char *)0);
  race_destination = g_build_filename(directory, "race-destination", (char *)0);
  race_backup = g_strconcat(race_destination, ".old", (char *)0);
  test_helper_assert("first source created", g_file_set_contents(first_source, "first", -1, &error));
  test_helper_assert("second source created", g_file_set_contents(second_source, "second", -1, &error));
  test_helper_assert("second destination created", g_file_set_contents(second_destination, "old", -1, &error));
  test_helper_assert("third source created", g_file_set_contents(third_source, "third", -1, &error));
  test_helper_assert("race source created", g_file_set_contents(race_source, "source", -1, &error));
  test_helper_assert("race destination created", g_file_set_contents(race_destination, "old", -1, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  cancellable = g_cancellable_new();
  trout_test_file_transfer_add(operations, first_source, first_destination);
  trout_test_file_transfer_add(operations, second_source, second_destination);
  trout_test_file_transfer_add(operations, third_source, third_destination);
  response.choice = trout_file_collision_cancel;
  result = trout_test_file_transfer_run_full(operations, trout_file_operation_copy, cancellable, &response, &error);
  test_helper_assert("batch collision cancels remaining work", result == trout_file_transfer_cancelled);
  test_helper_assert("batch cancellation reports its error", g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
  test_helper_assert("batch stops at the first collision", response.collision_count == 1);
  g_clear_error(&error);
  test_helper_assert("earlier committed copy remains", g_file_get_contents(first_destination, &contents, 0, &error));
  test_helper_assert("earlier copy contents match", g_strcmp0(contents, "first") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("first source remains after copying", g_file_test(first_source, G_FILE_TEST_IS_REGULAR));
  test_helper_assert("colliding source remains", g_file_test(second_source, G_FILE_TEST_IS_REGULAR));
  test_helper_assert("colliding destination remains unchanged", g_file_get_contents(second_destination, &contents, 0, &error));
  test_helper_assert("colliding destination contents match", g_strcmp0(contents, "old") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("later destination was not created", !g_file_test(third_destination, G_FILE_TEST_EXISTS));
  g_ptr_array_set_size(operations, 0);
  response.collision_count = 0;
  response.file_collision_count = 0;
  response.choice = trout_file_collision_replace;
  response.replace_destination_on_collision = TRUE;
  response.callback_ok = FALSE;
  trout_test_file_transfer_add(operations, race_source, race_destination);
  result = trout_test_file_transfer_run_full(operations, trout_file_operation_copy, cancellable, &response, &error);
  test_helper_assert("destination changed during the dialog is rejected", result == trout_file_transfer_failed);
  test_helper_assert("destination race reports a changed entry", g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED));
  test_helper_assert("collision callback replaced the destination", response.callback_ok);
  g_clear_error(&error);
  test_helper_assert("racing destination is preserved", g_file_get_contents(race_destination, &contents, 0, &error));
  test_helper_assert("racing destination contents match", g_strcmp0(contents, "racer") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("original destination was retained", g_file_get_contents(race_backup, &contents, 0, &error));
  test_helper_assert("original destination contents match", g_strcmp0(contents, "old") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("race source remains", g_file_get_contents(race_source, &contents, 0, &error));
  test_helper_assert("race source contents remain", g_strcmp0(contents, "source") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&cancellable);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(first_source);
  g_free(first_destination);
  g_free(second_source);
  g_free(second_destination);
  g_free(third_source);
  g_free(third_destination);
  g_free(race_source);
  g_free(race_destination);
  g_free(race_backup);
  g_free(contents);
  return status;
}

trout_test_status_t
trout_test_file_transfer_delete_tree(void)
{
  trout_test_status_t status = {0};
  char *directory;
  char *source_root;
  char *source_directory;
  char *nested_file;
  char *outside_directory;
  char *outside_file;
  char *link_path;
  char *contents;
  GFile *root_file;
  GFile *link_file;
  GPtrArray *operations;
  GError *error;
  trout_file_transfer_result result;
  status.group = "";
  directory = 0;
  source_root = 0;
  source_directory = 0;
  nested_file = 0;
  outside_directory = 0;
  outside_file = 0;
  link_path = 0;
  contents = 0;
  root_file = 0;
  link_file = 0;
  operations = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-transfer-delete-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  source_root = g_build_filename(directory, "source", (char *)0);
  source_directory = g_build_filename(source_root, "nested", (char *)0);
  nested_file = g_build_filename(source_directory, "file", (char *)0);
  outside_directory = g_build_filename(directory, "outside", (char *)0);
  outside_file = g_build_filename(outside_directory, "keep", (char *)0);
  link_path = g_build_filename(source_root, "link", (char *)0);
  test_helper_assert("source tree created", g_mkdir_with_parents(source_directory, 0700) == 0);
  test_helper_assert("source file created", g_file_set_contents(nested_file, "remove", -1, &error));
  test_helper_assert("outside target created", g_mkdir_with_parents(outside_directory, 0700) == 0);
  test_helper_assert("outside file created", g_file_set_contents(outside_file, "preserve", -1, &error));
  link_file = g_file_new_for_path(link_path);
  test_helper_assert("link to outside target created", g_file_make_symbolic_link(link_file, outside_directory, 0, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  trout_test_file_transfer_add(operations, source_root, source_root);
  result = trout_file_transfer_execute_full(operations, trout_file_operation_delete, 0, 0, &error);
  test_helper_assert("production delete removes the selected tree", result == trout_file_transfer_complete && error == 0);
  test_helper_assert("selected tree no longer exists", !g_file_test(source_root, G_FILE_TEST_EXISTS));
  test_helper_assert("symlink target remains outside the deleted tree", g_file_get_contents(outside_file, &contents, 0, &error));
  test_helper_assert("outside target contents remain", g_strcmp0(contents, "preserve") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&link_file);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(source_root);
  g_free(source_directory);
  g_free(nested_file);
  g_free(outside_directory);
  g_free(outside_file);
  g_free(link_path);
  g_free(contents);
  return status;
}

trout_test_status_t
trout_test_file_transfer_cross_device_move(void)
{
  trout_test_status_t status = {0};
  trout_test_file_transfer_response response = {0};
  char *directory;
  char *peer_directory;
  char *source_parent;
  char *destination_parent;
  char *source_directory;
  char *destination_directory;
  char *source_nested;
  char *source_nested_file;
  char *source_empty;
  char *source_link;
  char *destination_nested_file;
  char *destination_empty;
  char *destination_link;
  char *contents;
  struct stat source_stat;
  struct stat destination_stat;
  GFile *root_file;
  GFile *peer_root_file;
  GFile *link_file;
  GPtrArray *operations;
  GCancellable *cancellable;
  GError *error;
  trout_file_transfer_result result;
  status.group = "";
  directory = 0;
  peer_directory = 0;
  source_parent = 0;
  destination_parent = 0;
  source_directory = 0;
  destination_directory = 0;
  source_nested = 0;
  source_nested_file = 0;
  source_empty = 0;
  source_link = 0;
  destination_nested_file = 0;
  destination_empty = 0;
  destination_link = 0;
  contents = 0;
  root_file = 0;
  peer_root_file = 0;
  link_file = 0;
  operations = 0;
  cancellable = 0;
  error = 0;
  if (!g_file_test("/dev/shm", G_FILE_TEST_IS_DIR)) {
    printf("cross-filesystem move skipped: /dev/shm is unavailable\n");
    goto exit;
  }
  directory = g_dir_make_tmp("trout-cross-device-test-XXXXXX", &error);
  test_helper_assert("temporary source directory created", directory != 0);
  peer_directory = g_dir_make_tmp("/dev/shm/trout-cross-device-test-XXXXXX", &error);
  if (peer_directory == 0) {
    printf("cross-filesystem move skipped: /dev/shm is not writable\n");
    g_clear_error(&error);
    goto exit;
  }
  source_parent = g_build_filename(directory, "same-parent", (char *)0);
  destination_parent = g_build_filename(peer_directory, "same-parent", (char *)0);
  test_helper_assert("source parent created", g_mkdir_with_parents(source_parent, 0700) == 0);
  test_helper_assert("destination parent created", g_mkdir_with_parents(destination_parent, 0700) == 0);
  test_helper_assert("source parent device read", stat(source_parent, &source_stat) == 0);
  test_helper_assert("destination parent device read", stat(destination_parent, &destination_stat) == 0);
  if (source_stat.st_dev == destination_stat.st_dev) {
    printf("cross-filesystem move skipped: temporary directories share a filesystem\n");
    goto exit;
  }
  source_directory = g_build_filename(source_parent, "bundle", (char *)0);
  destination_directory = g_build_filename(destination_parent, "bundle", (char *)0);
  source_nested = g_build_filename(source_directory, "nested", (char *)0);
  source_nested_file = g_build_filename(source_nested, "file", (char *)0);
  source_empty = g_build_filename(source_directory, "empty", (char *)0);
  source_link = g_build_filename(source_directory, "dangling", (char *)0);
  destination_nested_file = g_build_filename(destination_directory, "nested", "file", (char *)0);
  destination_empty = g_build_filename(destination_directory, "empty", (char *)0);
  destination_link = g_build_filename(destination_directory, "dangling", (char *)0);
  test_helper_assert("source nested and empty directories created", g_mkdir_with_parents(source_nested, 0700) == 0);
  test_helper_assert("source empty directory created", g_mkdir(source_empty, 0700) == 0);
  test_helper_assert("source file created", g_file_set_contents(source_nested_file, "data", -1, &error));
  link_file = g_file_new_for_path(source_link);
  test_helper_assert("dangling symbolic link created", g_file_make_symbolic_link(link_file, "missing", 0, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  trout_test_file_transfer_add(operations, source_directory, destination_directory);
  cancellable = g_cancellable_new();
  result = trout_test_file_transfer_run_full(operations, trout_file_operation_move, cancellable, &response, &error);
  test_helper_assert("cross-filesystem directory move completes", result == trout_file_transfer_complete && error == 0);
  test_helper_assert("source directory was moved", !g_file_test(source_directory, G_FILE_TEST_EXISTS));
  test_helper_assert("destination file contents match", g_file_get_contents(destination_nested_file, &contents, 0, &error));
  test_helper_assert("destination file is intact", g_strcmp0(contents, "data") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("empty directory was moved", g_file_test(destination_empty, G_FILE_TEST_IS_DIR));
  test_helper_assert("dangling symbolic link was moved", g_file_test(destination_link, G_FILE_TEST_IS_SYMLINK));
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  if (peer_directory != 0) {
    peer_root_file = g_file_new_for_path(peer_directory);
    trout_test_delete_tree(peer_root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&peer_root_file);
  g_clear_object(&link_file);
  g_clear_object(&cancellable);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(peer_directory);
  g_free(source_parent);
  g_free(destination_parent);
  g_free(source_directory);
  g_free(destination_directory);
  g_free(source_nested);
  g_free(source_nested_file);
  g_free(source_empty);
  g_free(source_link);
  g_free(destination_nested_file);
  g_free(destination_empty);
  g_free(destination_link);
  g_free(contents);
  return status;
}

trout_test_status_t
trout_test_file_transfer_rename_collision(void)
{
  trout_test_status_t status = {0};
  char *directory;
  char *source_path;
  char *destination_path;
  char *renamed_path;
  char *contents;
  GFile *root_file;
  GFile *source_file;
  trout_file_transfer_pair *pair;
  GPtrArray *operations;
  GError *error;
  trout_file_transfer_result result;
  status.group = "";
  directory = 0;
  source_path = 0;
  destination_path = 0;
  renamed_path = 0;
  contents = 0;
  root_file = 0;
  source_file = 0;
  pair = 0;
  operations = 0;
  error = 0;
  directory = g_dir_make_tmp("trout-rename-test-XXXXXX", &error);
  test_helper_assert("temporary directory created", directory != 0);
  source_path = g_build_filename(directory, "source", (char *)0);
  destination_path = g_build_filename(directory, "destination", (char *)0);
  renamed_path = g_build_filename(directory, "renamed", (char *)0);
  test_helper_assert("rename source created", g_file_set_contents(source_path, "source", -1, &error));
  test_helper_assert("rename destination created", g_file_set_contents(destination_path, "destination", -1, &error));
  g_clear_error(&error);
  source_file = g_file_new_for_path(source_path);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  pair = trout_file_transfer_pair_new_rename(source_file, "destination", &error);
  test_helper_assert("rename collision operation created", pair != 0);
  g_ptr_array_add(operations, pair);
  pair = 0;
  result = trout_file_transfer_execute_full(operations, trout_file_operation_rename, 0, 0, &error);
  test_helper_assert("rename collision fails safely", result == trout_file_transfer_failed && error != 0);
  g_clear_error(&error);
  test_helper_assert("rename source remains", g_file_get_contents(source_path, &contents, 0, &error));
  test_helper_assert("rename source contents remain", g_strcmp0(contents, "source") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("rename destination remains", g_file_get_contents(destination_path, &contents, 0, &error));
  test_helper_assert("rename destination contents remain", g_strcmp0(contents, "destination") == 0);
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  pair = trout_file_transfer_pair_new_rename(source_file, "renamed", &error);
  test_helper_assert("non-colliding rename operation created", pair != 0);
  g_ptr_array_add(operations, pair);
  pair = 0;
  result = trout_file_transfer_execute_full(operations, trout_file_operation_rename, 0, 0, &error);
  test_helper_assert("non-colliding rename completes", result == trout_file_transfer_complete && error == 0);
  test_helper_assert("rename removes the old source path", !g_file_test(source_path, G_FILE_TEST_EXISTS));
  test_helper_assert("renamed source path exists", g_file_get_contents(renamed_path, &contents, 0, &error));
  test_helper_assert("renamed contents match", g_strcmp0(contents, "source") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_test_delete_tree(root_file, &error);
    g_clear_error(&error);
  }
  g_clear_object(&root_file);
  g_clear_object(&source_file);
  if (pair != 0) trout_file_transfer_pair_destroy(pair);
  if (operations != 0) g_ptr_array_unref(operations);
  g_free(directory);
  g_free(source_path);
  g_free(destination_path);
  g_free(renamed_path);
  g_free(contents);
  return status;
}

int
main(void)
{
  trout_test_status_t status = {0};
  status.group = "";
  test_helper_test(trout_test_action_config_parse);
  test_helper_test(trout_test_action_extension_matching);
  test_helper_test(trout_test_action_file_types);
  test_helper_test(trout_test_transfer_destination_guard);
  test_helper_test(trout_test_transfer_merge_policy);
  test_helper_test(trout_test_file_delete_tree);
  test_helper_test(trout_test_file_transfer_merges_directories);
  test_helper_test(trout_test_file_transfer_replace_and_cancel);
  test_helper_test(trout_test_file_transfer_move_collisions);
  test_helper_test(trout_test_file_transfer_rejects_aliases);
  test_helper_test(trout_test_file_transfer_cancel_and_preserve_race);
  test_helper_test(trout_test_file_transfer_delete_tree);
  test_helper_test(trout_test_file_transfer_cross_device_move);
  test_helper_test(trout_test_file_transfer_rename_collision);
exit:
  test_helper_display_summary;
  return status.id;
}
