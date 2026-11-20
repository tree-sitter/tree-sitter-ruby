# The image ClusterFuzzLite builds the fuzzer in (.github/workflows/cflite_*.yml)
FROM gcr.io/oss-fuzz-base/base-builder@sha256:1f9cf74bc3d7029b528494542f9d9851b256dceed38d3f420989434dc5ae0760
# The tree-sitter runtime at the version Cargo.lock pins, checked against its commit
RUN git clone --quiet --depth 1 --branch v0.27.2 https://github.com/tree-sitter/tree-sitter $SRC/tree-sitter && \
    test "$(git -C $SRC/tree-sitter rev-parse HEAD)" = 3fb7be49145acfb07e33b61ed5689e2b6771b2e9
COPY . $SRC/tree-sitter-ruby
WORKDIR $SRC/tree-sitter-ruby
COPY .clusterfuzzlite/build.sh $SRC/
