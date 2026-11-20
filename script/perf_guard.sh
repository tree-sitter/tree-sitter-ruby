#!/usr/bin/env bash
# Fails when the parser in src/ runs more than 3 % more instructions than the one in BASE/src over the Ruby files under
# DIR...; valgrind counts the instructions, which unlike time stay the same from run to run on a shared runner.
# usage: script/perf_guard.sh BASE DIR...   (the `perf` job in ci.yml runs it with BASE = a checkout of the PR base)
set -euo pipefail

base=$1
shift
version=$(sed -n '/^name = "tree-sitter"$/{n;s/^version = "\(.*\)"$/\1/p}' Cargo.lock)
lib=$(ls -d ~/.cargo/registry/src/*/tree-sitter-"$version")
out=$(mktemp -d)
mapfile -t files < <(find "$@" -name '*.rb' | sort)

count() { # SRC NAME: build script/sanitize.c against SRC's parser and print its instruction count over the files
    cc -O2 -I"$1" -I"$lib/include" script/sanitize.c "$1/parser.c" "$1/scanner.c" "$lib/src/lib.c" -o "$out/$2"
    valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null --log-file="$out/$2.log" \
        "$out/$2" "${files[@]}"
    sed -n 's/.*I *refs: *//p' "$out/$2.log" | tr -d ,
}

count "$base/src" base >"$out/base.count" &
base_pid=$!
count src head >"$out/head.count" &
head_pid=$!
wait $base_pid
wait $head_pid
read -r before <"$out/base.count"
read -r after <"$out/head.count"
ratio=$(awk -v a="$after" -v b="$before" 'BEGIN { printf "%.4f", a / b }')

printf '| %d files | instructions |\n|---|---:|\n| base | %s |\n| head | %s |\n| head / base | %s |\n' \
    "${#files[@]}" "$before" "$after" "$ratio" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
if awk -v r="$ratio" 'BEGIN { exit !(r > 1.03) }'; then
    echo "::error::the parser runs $ratio times the instructions of the base, over the 1.03 limit"
    exit 1
fi
