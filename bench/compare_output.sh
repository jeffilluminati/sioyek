#!/usr/bin/env bash
# Checks that two builds of the benchmark (e.g. before and after changing mupdf-patches/) render every page
# and build the text index identically, by comparing the hashes BENCH_PIXMAP_HASHES makes them print.
#
# usage: compare_output.sh bench_a bench_b corpus_dir
#
# corpus_dir holds the documents made by gen_corpus.py and gen_render_tests.py; missing ones are skipped.
# Pages are rendered on one thread: with several, mupdf's store can hand a page an image decoded at a
# different resolution for another page, depending on timing, which changes the output of either build.
set -u
A=$1
B=$2
DIR=$3
fail=0

hashes() {
    BENCH_PIXMAP_HASHES=1 "$1" --phases "$2" --render-pages "$4" --threads 1 --scale "$5" "$DIR/$3" 2>/dev/null |
        grep -E "^(pixmap|index text)" | sort
}

for spec in "shapes_40.pdf 40 1" "shapes_40.pdf 40 2.3" "shapes_40.pdf 40 0.37" \
            "images_24.pdf 24 1" "images_24.pdf 24 0.31" "images_24.pdf 24 1.7" "images_24.pdf 24 3.1" \
            "vector_200.pdf 12 1.5" "text_4000.pdf 10 1" "scan_300.pdf 10 1" "scan_300.pdf 6 0.5" "scan_300.pdf 4 2.2"; do
    set -- $spec
    [ -f "$DIR/$1" ] || continue
    a=$(hashes "$A" render "$1" "$2" "$3")
    b=$(hashes "$B" render "$1" "$2" "$3")
    if [ -n "$a" ] && [ "$a" = "$b" ]; then
        echo "same       $1 at scale $3 ($(echo "$a" | wc -l | tr -d ' ') pages)"
    else
        echo "DIFFERENT  $1 at scale $3"
        diff <(echo "$a") <(echo "$b") | head -6
        fail=1
    fi
done

for doc in text_4000.pdf vector_200.pdf; do
    [ -f "$DIR/$doc" ] || continue
    a=$(hashes "$A" index "$doc" 1 1)
    b=$(hashes "$B" index "$doc" 1 1)
    if [ -n "$a" ] && [ "$a" = "$b" ]; then
        echo "same       $doc text index"
    else
        echo "DIFFERENT  $doc text index"
        fail=1
    fi
done
exit $fail
