# Trout

Trout is a GTK4 file manager for local directories.

![](other/trout.png)

Features
* Fast with large directories (10k+ files)
* Fast and immediate thumbnail generation
* Custom right-click actions
* Custom action buttons
* Directory copies and moves merge into existing directories with the same name
* Copy and move collisions offer Skip, Replace, and Cancel
* Queued copy and move tasks with progress, pause, resume, and cancel

The first implementation provides asynchronous monitored directory listings, a compact horizontal browser, breadcrumb navigation with an editable location field, Ctrl+wheel zoom, tabs, new windows, hidden file toggling, and an item selection status line.

## File operations

Copying or moving a directory into an existing directory with the same name merges their contents recursively. Moves move files into the existing tree. Colliding files prompt for Skip, Replace, or Cancel. Replacing a directory is refused to protect its contents.

Copy and move requests run in a queue. The active request shows progress and can be paused, resumed, or canceled.

## Custom actions

Custom actions are loaded at startup from `$XDG_CONFIG_HOME/trout/actions` (normally `~/.config/trout/actions`). The file uses LF line endings. Each non-empty line has this format, with single spaces between fields:

```text
label appearance type extensions command argument ...
```

`label` has no spaces. `appearance` is `c` for the entry context menu, `b` for a button in the header bar, or `cb` for both. `type` is `d` for directories, `f` for non-directory entries, or `fd` for both. `extensions` is a comma-separated list without leading dots, matched case-sensitively against the filename suffix after its final dot. Use `*` to match any extension. An action is shown only when every selected entry matches its type and extension filters.

The command and its arguments are split on spaces and run directly without a shell. Configured tokens cannot contain spaces; quoting and escaping are not supported. Trout appends each selected entry's full path as a separate argument, so paths containing spaces remain a single argument. No string interpolation is performed.

## Build
Run `./exe/compile` to create `exe/compiled/trout`.

Trout was inspired by Thunar to improve on various issues Thunar never fixed:
* Slow Thumbnail creation only when the file comes into view, which is too late
* Replacing directories, requiring a local copy for all files, instead of merging and just doing a local move
* Excessive slow-downs on large directories
* Not continuing the file copy queue after one item was manually paused
