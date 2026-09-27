#!/usr/bin/env bash
# Applies sioyek's patches to the mupdf submodule. They only make mupdf faster, what it renders
# is unchanged. Running this again is fine: patches that are already applied are skipped. When
# a patch doesn't apply anymore (e.g. after updating mupdf), this fails so it gets updated.
set -e
cd "$(dirname "$0")/../mupdf"
for patch in ../mupdf-patches/*.patch; do
    if git apply --reverse --check "$patch" 2>/dev/null; then
        continue
    fi
    git apply "$patch"
    echo "applied $(basename "$patch") to mupdf"
done
