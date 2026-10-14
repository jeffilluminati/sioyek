#!/bin/sh
# Called by sioyek's _print command (bound to p), since sioyek has no built-in print.
# macOS: opens Preview's print dialog for the current PDF.
# Linux/other: opens a GUI print dialog if one is available, otherwise sends the
# file to the default CUPS printer with lp. Set SIOYEK_PRINT_CMD to override,
# e.g. SIOYEK_PRINT_CMD="lp -d office" or SIOYEK_PRINT_CMD="okular --print".
set -e
file="$1"
if [ -z "$file" ]; then
    echo "usage: $0 FILE.pdf" >&2
    exit 1
fi

if [ -n "$SIOYEK_PRINT_CMD" ]; then
    exec $SIOYEK_PRINT_CMD "$file"
fi

case "$(uname -s)" in
Darwin)
    osascript - "$file" <<'EOF'
on run argv
    tell application "Preview"
        activate
        print (POSIX file (item 1 of argv)) with print dialog
    end tell
end run
EOF
    ;;
*)
    if command -v okular >/dev/null 2>&1; then
        exec okular --print "$file"
    elif command -v evince >/dev/null 2>&1; then
        exec evince --preview "$file"
    fi
    if command -v lp >/dev/null 2>&1; then
        exec lp "$file"
    fi
    echo "sioyek-print: no print command found (install cups or okular, or set SIOYEK_PRINT_CMD)" >&2
    exit 1
    ;;
esac
