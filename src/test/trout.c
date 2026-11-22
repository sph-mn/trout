#define trout_test_build
#include "../trout/trout.c"

#define status_t trout_test_status_t
#include <trout/sph/test.h>
#undef status_t

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
trout_test_copy_merge_policy(void)
{
  trout_test_status_t status = {0};
  status.group = "";
  test_helper_assert("copy merges existing directories", trout_file_copy_should_merge(G_FILE_TYPE_DIRECTORY, G_FILE_TYPE_DIRECTORY, trout_file_operation_copy));
  test_helper_assert("copy does not merge a directory into a file", !trout_file_copy_should_merge(G_FILE_TYPE_DIRECTORY, G_FILE_TYPE_REGULAR, trout_file_operation_copy));
  test_helper_assert("copy does not merge files", !trout_file_copy_should_merge(G_FILE_TYPE_REGULAR, G_FILE_TYPE_REGULAR, trout_file_operation_copy));
  test_helper_assert("move does not merge directories", !trout_file_copy_should_merge(G_FILE_TYPE_DIRECTORY, G_FILE_TYPE_DIRECTORY, trout_file_operation_move));
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
  test_helper_assert("nested tree deleted", trout_file_delete_tree(remove_file, 0, &error));
  test_helper_assert("nested tree no longer exists", !g_file_test(remove_directory, G_FILE_TEST_EXISTS));
  target_directory = g_build_filename(directory, "target", (char *)0);
  target_file = g_build_filename(target_directory, "file", (char *)0);
  link_path = g_build_filename(directory, "link", (char *)0);
  test_helper_assert("link target directory created", g_mkdir_with_parents(target_directory, 0700) == 0);
  test_helper_assert("link target file created", g_file_set_contents(target_file, "preserved", -1, &error));
  g_clear_error(&error);
  link_file = g_file_new_for_path(link_path);
  test_helper_assert("symbolic link created", g_file_make_symbolic_link(link_file, target_directory, 0, &error));
  test_helper_assert("symbolic link deleted", trout_file_delete_tree(link_file, 0, &error));
  test_helper_assert("symbolic link no longer exists", !g_file_test(link_path, G_FILE_TEST_IS_SYMLINK));
  test_helper_assert("link target remains", g_file_get_contents(target_file, &contents, 0, &error));
  test_helper_assert("link target contents remain", g_strcmp0(contents, "preserved") == 0);
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_file_delete_tree(root_file, 0, &error);
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
} trout_test_file_transfer_response;

gboolean
trout_test_file_transfer_wait(gpointer user_data)
{
  (void)user_data;
  return TRUE;
}

trout_file_collision_choice
trout_test_file_transfer_collision(GFile *source, GFile *destination, GFileType source_type, GFileType destination_type, gpointer user_data)
{
  trout_test_file_transfer_response *response;
  response = user_data;
  response->source_type = source_type;
  response->destination_type = destination_type;
  response->collision_count += 1;
  (void)source;
  (void)destination;
  return response->choice;
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
  response.choice = trout_file_collision_skip;
  test_helper_assert("directory merge succeeds", trout_test_file_transfer_run(operations, trout_file_operation_copy, cancellable, &response, &error));
  test_helper_assert("one nested file collision was reported", response.collision_count == 1);
  test_helper_assert("directory collision merged", response.source_type == G_FILE_TYPE_REGULAR && response.destination_type == G_FILE_TYPE_REGULAR);
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
    trout_file_delete_tree(root_file, 0, &error);
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
  test_helper_assert("directory replacement succeeds", trout_test_file_transfer_run(operations, trout_file_operation_copy, cancellable, &response, &error));
  test_helper_assert("directory collision was reported", response.collision_count == 1);
  test_helper_assert("directory replaced by a file", g_file_test(destination_directory, G_FILE_TEST_IS_REGULAR));
  test_helper_assert("old directory contents removed", !g_file_test(destination_nested_file, G_FILE_TEST_EXISTS));
  g_clear_pointer(&contents, g_free);
  g_ptr_array_set_size(operations, 0);
  trout_test_file_transfer_add(operations, source_directory, directory_destination);
  response.collision_count = 0;
  test_helper_assert("directory replaces a file", trout_test_file_transfer_run(operations, trout_file_operation_copy, cancellable, &response, &error));
  test_helper_assert("file collision was reported for a directory", response.collision_count == 1);
  test_helper_assert("file replaced by a directory", g_file_test(directory_destination, G_FILE_TEST_IS_DIR));
  test_helper_assert("directory contents copied", g_file_get_contents(directory_destination_file, &contents, 0, &error));
  test_helper_assert("directory contents match", g_strcmp0(contents, "new directory") == 0);
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
    trout_file_delete_tree(root_file, 0, &error);
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
  char *destination_directory;
  char *destination_directory_file;
  char *destination_old_file;
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
  destination_directory = 0;
  destination_directory_file = 0;
  destination_old_file = 0;
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
  destination_directory = g_build_filename(directory, "destination-directory", (char *)0);
  destination_directory_file = g_build_filename(destination_directory, "new.txt", (char *)0);
  destination_old_file = g_build_filename(destination_directory, "old.txt", (char *)0);
  test_helper_assert("source file created", g_file_set_contents(source_file, "new file", -1, &error));
  test_helper_assert("destination file created", g_file_set_contents(destination_file, "old file", -1, &error));
  test_helper_assert("source directory created", g_mkdir_with_parents(source_directory, 0700) == 0);
  test_helper_assert("source directory file created", g_file_set_contents(source_directory_file, "new directory", -1, &error));
  test_helper_assert("destination directory created", g_mkdir_with_parents(destination_directory, 0700) == 0);
  test_helper_assert("destination directory file created", g_file_set_contents(destination_old_file, "old directory", -1, &error));
  g_clear_error(&error);
  operations = g_ptr_array_new_with_free_func(trout_file_transfer_pair_destroy);
  cancellable = g_cancellable_new();
  trout_test_file_transfer_add(operations, source_file, destination_file);
  response.choice = trout_file_collision_skip;
  test_helper_assert("move skip succeeds", trout_test_file_transfer_run(operations, trout_file_operation_move, cancellable, &response, &error));
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
  response.choice = trout_file_collision_skip;
  response.collision_count = 0;
  test_helper_assert("directory move skip succeeds", trout_test_file_transfer_run(operations, trout_file_operation_move, cancellable, &response, &error));
  test_helper_assert("directory move collision was reported", response.collision_count == 1);
  test_helper_assert("directory move types were reported", response.source_type == G_FILE_TYPE_DIRECTORY && response.destination_type == G_FILE_TYPE_DIRECTORY);
  test_helper_assert("skipped source directory remains", g_file_test(source_directory, G_FILE_TEST_IS_DIR));
  test_helper_assert("skipped destination directory remains", g_file_test(destination_directory, G_FILE_TEST_IS_DIR));
  response.choice = trout_file_collision_replace;
  response.collision_count = 0;
  test_helper_assert("directory move replacement succeeds", trout_test_file_transfer_run(operations, trout_file_operation_move, cancellable, &response, &error));
  test_helper_assert("directory move replacement collision was reported", response.collision_count == 1);
  test_helper_assert("directory move source removed", !g_file_test(source_directory, G_FILE_TEST_EXISTS));
  test_helper_assert("directory move destination has new file", g_file_get_contents(destination_directory_file, &contents, 0, &error));
  test_helper_assert("directory move contents match", g_strcmp0(contents, "new directory") == 0);
  g_clear_pointer(&contents, g_free);
  test_helper_assert("directory move replaced old contents", !g_file_test(destination_old_file, G_FILE_TEST_EXISTS));
exit:
  g_clear_error(&error);
  if (directory != 0) {
    root_file = g_file_new_for_path(directory);
    trout_file_delete_tree(root_file, 0, &error);
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
  g_free(destination_directory);
  g_free(destination_directory_file);
  g_free(destination_old_file);
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
  test_helper_test(trout_test_copy_merge_policy);
  test_helper_test(trout_test_file_delete_tree);
  test_helper_test(trout_test_file_transfer_merges_directories);
  test_helper_test(trout_test_file_transfer_replace_and_cancel);
  test_helper_test(trout_test_file_transfer_move_collisions);
exit:
  test_helper_display_summary;
  return status.id;
}
