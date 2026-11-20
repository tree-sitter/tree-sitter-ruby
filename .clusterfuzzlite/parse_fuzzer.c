// libFuzzer target for ClusterFuzzLite (.clusterfuzzlite/build.sh): parses the input, then deletes a slice of it chosen
// by its first and last bytes and reparses with the edited tree, so fresh and incremental parsing both run under ASan
// and UBSan.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>

const TSLanguage *tree_sitter_ruby(void);

static TSPoint point_at(const uint8_t *text, uint32_t offset) {
    TSPoint point = {0, 0};
    for (uint32_t i = 0; i < offset; i++) {
        if (text[i] == '\n') {
            point.row++;
            point.column = 0;
        } else {
            point.column++;
        }
    }
    return point;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static TSParser *parser;
    if (!parser) {
        parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_ruby());
    }
    TSTree *tree = ts_parser_parse_string(parser, NULL, (const char *)data, (uint32_t)size);
    if (size > 2) {
        uint32_t start = data[0] % (uint32_t)size;
        uint32_t length = data[size - 1] % ((uint32_t)size - start);
        uint8_t *edited = malloc(size - length);
        memcpy(edited, data, start);
        memcpy(edited + start, data + start + length, size - start - length);
        TSInputEdit edit = {
            .start_byte = start,
            .old_end_byte = start + length,
            .new_end_byte = start,
            .start_point = point_at(data, start),
            .old_end_point = point_at(data, start + length),
            .new_end_point = point_at(data, start),
        };
        ts_tree_edit(tree, &edit);
        ts_tree_delete(ts_parser_parse_string(parser, tree, (const char *)edited, (uint32_t)(size - length)));
        free(edited);
    }
    ts_tree_delete(tree);
    return 0;
}
