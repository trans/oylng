#!/bin/sh
# seed.sh DATA CORPUS — each of the suite's streams as a fuzz input, with a
# first byte of 0 (default options; see fuzz_parser.c)
set -e
find "$1" -name in.yaml | while read -r f; do
    id=$(echo "$f" | sed "s|^$1/||; s|/in.yaml\$||; s|/|_|g")
    { printf '\000'; cat "$f"; } > "$2/$id"
done
