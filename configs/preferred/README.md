# Preferred configuration

This profile contains the preferred Gruvbox dark theme and Zathura/Vim-style
bindings used with this fork's `preferred-build` branch.

Copy the files into your Sioyek configuration directory, backing up any existing
files first. On Linux and macOS, run these commands from the repository root:

```sh
mkdir -p "$HOME/.config/sioyek"
cp configs/preferred/prefs_user.config "$HOME/.config/sioyek/prefs_user.config"
cp configs/preferred/keys_user.config "$HOME/.config/sioyek/keys_user.config"
```

Restart Sioyek after installation. These bindings require this fork's page-aware
scrolling, horizontal half-page movement, and `set_pages_per_row` commands.

| Shortcut | Action |
| --- | --- |
| `Ctrl+1` | Single-page view |
| `Ctrl+2`–`Ctrl+9` | Select that many pages per row |
| `D` | Toggle single-page view and the last multi-page layout |
| `Ctrl+d` / `Ctrl+Shift+d` | Start page 1 in column 2 / column 1, with the default `first_page_column 0` |
| `Ctrl+p` | Search recently opened documents |
| `Ctrl+^` | Switch to the previous file |
| `Space` / `Shift+Space` | Scroll down / up without skipping page boundaries |
| `a` / `s` | Fit the row to height / width |
| `i` / `Ctrl+r` | Toggle Gruvbox page recoloring |
| `:` | Open the command selector |

The theme uses Menlo at size 14. If Menlo is unavailable, set `ui_font` and
`status_font` to an installed monospace font. Set the optional
`default_open_file_path` to your book directory; the shared profile leaves it
unset so the file picker uses its default location.

## Printing

The `p` shortcut invokes the custom `_print` command. On macOS, the included
`print.sh` opens Preview's print dialog. Install it on Sioyek's `PATH` as
`sioyek-print`:

```sh
mkdir -p "$HOME/.local/bin"
install -m 755 configs/preferred/print.sh "$HOME/.local/bin/sioyek-print"
```

On Linux, replace the `_print` definition in `prefs_user.config` with your chosen
PDF viewer or print command. For example, `new_command _print lp %1` sends the
current file directly to the default printer through CUPS. The included helper
requires macOS and `osascript`.
