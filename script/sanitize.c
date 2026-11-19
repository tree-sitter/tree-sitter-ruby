// Parses each file given on the command line, built with -fsanitize=address,undefined by the CI `sanitizers` job:
// a memory error aborts, and a parse slower than MAX_SECONDS fails, which catches loops and quadratic paths.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <tree_sitter/api.h>

#define MAX_SECONDS 10.0

const TSLanguage *tree_sitter_ruby(void);

int main(int argc, char **argv) {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_ruby());
    int status = 0;
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "rb");
        if (!file) {
            perror(argv[i]);
            return 2;
        }
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        char *text = malloc((size_t)size + 1);
        if (!text || fread(text, 1, (size_t)size, file) != (size_t)size) {
            fprintf(stderr, "%s: read failed\n", argv[i]);
            return 2;
        }
        fclose(file);
        clock_t start = clock();
        TSTree *tree = ts_parser_parse_string(parser, NULL, text, (uint32_t)size);
        double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
        if (seconds > MAX_SECONDS) {
            fprintf(stderr, "%s: %.1f s to parse %ld bytes\n", argv[i], seconds, size);
            status = 1;
        }
        ts_tree_delete(tree);
        free(text);
    }
    ts_parser_delete(parser);
    return status;
}
