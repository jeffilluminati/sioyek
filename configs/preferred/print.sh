#!/bin/sh
# Called by sioyek's _print command (bound to p). Opens the macOS print dialog
# for the current PDF via Preview, since sioyek has no built-in print.
osascript - "$1" <<'EOF'
on run argv
    tell application "Preview"
        activate
        print (POSIX file (item 1 of argv)) with print dialog
    end tell
end run
EOF
