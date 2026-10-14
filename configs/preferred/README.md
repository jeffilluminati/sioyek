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
| `Ctrl+d` / `Ctrl+Shift+d` | Start page 1 standalone (default) / paired in column 1 (`first_page_column 0`) |
| `Ctrl+p` | Search recently opened documents |
| `Ctrl+^` | Switch to the previous file |
| `Space` / `Shift+Space` | Scroll down / up without skipping page boundaries |
| `a` / `s` | Fit the row to height / width |
| `A` / `S` | Smart-fit the row to height / width (cropping margins) |
| `C` | Toggle persistent auto-page-crop across pages |
| `i` / `Ctrl+r` | Toggle Gruvbox page recoloring |
| `:` | Open the command selector |

The theme uses a monospace font at size 14. `ui_font` and `status_font` accept a
comma-separated fallback list (`Menlo, DejaVu Sans Mono, monospace`), so Menlo is
used on macOS and DejaVu Sans Mono or the system monospace font on Linux. Set the optional
`default_open_file_path` to your book directory; the shared profile leaves it
unset so the file picker uses its default location.

## Printing

The `p` shortcut invokes the custom `_print` command, which runs the included
`print.sh`. Install it on Sioyek's `PATH` as `sioyek-print`:

```sh
mkdir -p "$HOME/.local/bin"
install -m 755 configs/preferred/print.sh "$HOME/.local/bin/sioyek-print"
```

On macOS it opens Preview's print dialog. On Linux it opens Okular's or Evince's
print dialog if either is installed, and otherwise sends the file straight to the
default CUPS printer with `lp`. Set `SIOYEK_PRINT_CMD` (for example
`SIOYEK_PRINT_CMD="lp -d office"`) to use a different command.
