#!/usr/bin/env bash
# Prints how much of the hand-written scanner (src/scanner.c, src/locals.h) the tests run: the inputs of test/corpus,
# test/highlight and test/tags, parsed by script/sanitize.c built with --coverage. src/parser.c is generated: left out.
# usage: script/coverage.sh   (the `coverage` job in ci.yml; the annotated *.gcov sources stay in the printed directory)
set -euo pipefail

version=$(sed -n '/^name = "tree-sitter"$/{n;s/^version = "\(.*\)"$/\1/p}' Cargo.lock)
lib=$(ls -d ~/.cargo/registry/src/*/tree-sitter-"$version")
repo=$PWD
out=$(mktemp -d)

ruby script/corpus_inputs.rb "$out/inputs"
cp test/highlight/*.rb test/tags/*.rb "$out/inputs/"
# absolute paths, so that gcov finds the included sources from $out
gcc -std=gnu11 -O0 --coverage -I"$repo/src" -c "$repo/src/scanner.c" -o "$out/scanner.o"
gcc -std=gnu11 -O2 --coverage -Isrc -I"$lib/include" script/sanitize.c src/parser.c "$lib/src/lib.c" "$out/scanner.o" \
    -o "$out/parse"
"$out/parse" "$out"/inputs/*
(cd "$out" && gcov -b -o "$out" "$repo/src/scanner.c" >gcov.txt)

# gcov prints a block per file: "File '...'", "Lines executed:P% of N", ..., "Taken at least once:P% of N"
awk -v files="$(ls "$out"/inputs | wc -l)" '
    /^File / { name = $2; gsub(/\x27/, "", name); sub(/.*\/src\//, "src/", name); keep = name ~ /^src\/(scanner\.c|locals\.h)$/ }
    keep && /^Lines executed:/ { split($0, a, /[:% ]+/); lp[name] = a[3]; ln[name] = a[5]; lines += a[3] * a[5] / 100; lall += a[5] }
    keep && /^Taken at least once:/ { split($0, a, /[:% ]+/); bp[name] = a[5]; bn[name] = a[7]; branches += a[5] * a[7] / 100; ball += a[7] }
    END {
        printf "| %d test inputs | lines | branches |\n|---|---:|---:|\n", files
        for (n in lp) printf "| %s | %.1f %% of %d | %.1f %% of %d |\n", n, lp[n], ln[n], bp[n], bn[n]
        printf "| **total** | **%.1f %%** of %d | **%.1f %%** of %d |\n", 100 * lines / lall, lall, 100 * branches / ball, ball
    }' "$out/gcov.txt" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
echo "annotated sources: $out/scanner.c.gcov $out/locals.h.gcov"
