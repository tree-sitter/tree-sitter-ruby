#!/usr/bin/env bash
# Prints how much of the hand-written scanner (src/scanner.c, src/locals.h) the tests run: the inputs of test/corpus,
# test/highlight and test/tags, parsed by script/sanitize.c built with --coverage. src/parser.c is generated: left out.
# usage: script/coverage.sh [BADGE.json]   (the `coverage` job in ci.yml; playground.yml publishes BADGE.json for the
# README badge; the annotated *.gcov sources stay in the printed directory)
# Fails, after writing the badge, below 97.5 % of lines or 92.0 % of branches (min_lines, min_branches): what the tests
# reached, rounded down to 0.5. Raise these floors when the tests reach more.
set -euo pipefail

min_lines=97.5
min_branches=92.0

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
awk -v files="$(ls "$out"/inputs | wc -l)" -v totals="$out/totals" '
    /^File / { name = $2; gsub(/\x27/, "", name); sub(/.*\/src\//, "src/", name); keep = name ~ /^src\/(scanner\.c|locals\.h)$/ }
    keep && /^Lines executed:/ { split($0, a, /[:% ]+/); lp[name] = a[3]; ln[name] = a[5]; lines += a[3] * a[5] / 100; lall += a[5] }
    keep && /^Taken at least once:/ { split($0, a, /[:% ]+/); bp[name] = a[5]; bn[name] = a[7]; branches += a[5] * a[7] / 100; ball += a[7] }
    END {
        printf "| %d test inputs | lines | branches |\n|---|---:|---:|\n", files
        for (n in lp) printf "| %s | %.1f %% of %d | %.1f %% of %d |\n", n, lp[n], ln[n], bp[n], bn[n]
        printf "| **total** | **%.1f %%** of %d | **%.1f %%** of %d |\n", 100 * lines / lall, lall, 100 * branches / ball, ball
        printf "%.1f %.1f %f %f\n", 100 * lines / lall, 100 * branches / ball,
            100 * lines / lall, 100 * branches / ball > totals
    }' "$out/gcov.txt" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
echo "annotated sources: $out/scanner.c.gcov $out/locals.h.gcov"
read -r lines branches exact_lines exact_branches <"$out/totals"

# A shields.io endpoint badge, colored by line coverage
if [[ -n ${1:-} ]]; then
    color=$(awk -v l="$lines" 'BEGIN { print (l >= 90 ? "brightgreen" : l >= 80 ? "green" : l >= 70 ? "yellow" : "red") }')
    printf '{"schemaVersion":1,"label":"coverage","message":"lines %s%% · branches %s%%","color":"%s"}\n' \
        "$lines" "$branches" "$color" >"$1"
fi

# Unrounded, so that 92.47 % does not pass a 92.5 % floor
awk -v l="$exact_lines" -v b="$exact_branches" -v ml="$min_lines" -v mb="$min_branches" 'BEGIN {
    hint = "add test/corpus inputs for the gaps in the annotated sources, or lower the floor in script/coverage.sh"
    if (l < ml) printf "::error::Scanner line coverage %.2f %% is below the %s %% floor: %s\n", l, ml, hint
    if (b < mb) printf "::error::Scanner branch coverage %.2f %% is below the %s %% floor: %s\n", b, mb, hint
    exit l < ml || b < mb
}'
