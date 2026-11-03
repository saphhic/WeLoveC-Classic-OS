# Code editor

Code is a small source editor integrated into the WeLoveC Classic desktop. It
runs as its own ELF at `/userspace/bin/code/code.elf` and is available from the
Start menu.

## Features

- Explorer for browsing directories and opening files.
- Editable text with line numbers, a visible caret, and line/column status.
- Basic syntax colors for C and C++ keywords, comments, strings, numbers, and
  preprocessor lines.
- Open, Save, Save As, and New controls, with prompts for absolute paths.
- Unsaved-change prompts when opening another file, creating a document, or
  closing the app.
- Files up to 4 MiB; binary files open read-only.

Use the mouse to choose files and position the caret. The editor accepts
printable ASCII, Enter, Tab, and Backspace; arrow keys move the caret. The
current keyboard API does not provide Ctrl+letter shortcuts, Home/End, or
Delete, so file operations use the toolbar.

Code is a compact editor rather than a full Visual Studio Code replacement. It
does not provide project workspaces, extensions, compiler integration,
debugging, or language servers. The explorer currently displays the first 32
entries in a directory. The filesystem API also provides `FS_LIST_RANGE` for
programs that need to page through larger directories.
