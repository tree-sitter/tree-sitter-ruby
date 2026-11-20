#!/bin/bash -eu
# Builds the libFuzzer target that ClusterFuzzLite runs with ASan and UBSan (.github/workflows/cflite_*.yml), with a
# Ruby dictionary and the input of every corpus, highlight and tag test as seeds.
runtime=$SRC/tree-sitter/lib

$CC $CFLAGS -std=gnu11 -I"$runtime/include" -I"$runtime/src" -c "$runtime/src/lib.c" -o lib.o
$CC $CFLAGS -Isrc -c src/parser.c -o parser.o
$CC $CFLAGS -Isrc -c src/scanner.c -o scanner.o
$CC $CFLAGS -I"$runtime/include" -c .clusterfuzzlite/parse_fuzzer.c -o parse_fuzzer.o
$CXX $CXXFLAGS parse_fuzzer.o parser.o scanner.o lib.o $LIB_FUZZING_ENGINE -o "$OUT/parse_fuzzer"
cp .clusterfuzzlite/ruby.dict "$OUT/parse_fuzzer.dict"

# The input of each test in test/corpus, as script/corpus_inputs.rb splits them (the image has Python, not Ruby)
python3 - <<'EOF'
import glob, os, re
os.makedirs("seeds", exist_ok=True)
for path in glob.glob("test/corpus/*.txt"):
    tests = re.split(rb"^={3,}[^\n]*\n[^\n]*\n={3,}[^\n]*\n", open(path, "rb").read(), flags=re.M)[1:]
    for i, test in enumerate(tests):
        divider = max(re.findall(rb"^-{3,}(?=\r?\n)", test, flags=re.M), key=len)
        end = re.search(rb"^" + re.escape(divider) + rb"\r?\n", test, flags=re.M).start()
        with open(f"seeds/{os.path.basename(path)[:-4]}_{i}.rb", "wb") as seed:
            seed.write(test[:end])
EOF
zip -q -j "$OUT/parse_fuzzer_seed_corpus.zip" seeds/* test/highlight/*.rb test/tags/*.rb
