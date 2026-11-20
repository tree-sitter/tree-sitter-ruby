// Incremental-parsing fuzzer, built with -fsanitize=address,undefined by the nightly `edits` job in fuzz.yml.
// For each file it applies EDITS seeded random edits (insert, delete or replace bytes, often next to a quote, heredoc,
// interpolation or operator) in rounds of one to three, each round followed by its undos. After each edit or undo it
// reparses with the edited old tree and compares the result with a fresh parse of the same text. On a mismatch it
// prints a repro, the fewest last edits and the smallest text that still mismatch, and goes to the next file.
// usage: edit_fuzz [-s SEED] [-n EDITS] FILE...   (the same seed and file replay the same edits)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>

const TSLanguage *tree_sitter_ruby(void);

typedef struct {
    uint32_t start, del, ins_len;
    char ins[64];
} Edit;

void __sanitizer_set_death_callback(void (*callback)(void)) __attribute__((weak)); // in the sanitizer runtime

static uint64_t rng;
static struct { // what to replay when a sanitizer stops the program
    const char *path;
    uint64_t seed;
    int edits;
} now;
static unsigned long recoveries; // edits after which the two trees differ only in how they recover from errors

static uint64_t next(void) { // splitmix64
    uint64_t z = (rng += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

static uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }

static const char *const FRAGMENTS[] = {
    "\"", "'", "`", "#{", "}", "{", "(", ")", "[", "]", "|", "/", "%", "%w[", "%q(", "%(", "?", ":", "\\", "#",
    "<<~A\n", "<<-A\n", "<<A\n", "<<~'A'\n", "\nA\n", "A\n", "  A\n", "\n", "\r\n", " ", "\t", ";", ",", "=",
    " +1", " -1", "+", "-", "*", "&", "x = 1\n", "x ", "x", "x +1", "a, b = ", "do", "end", " do |a|\n",
    "def f(a)\n", "def x = ", "if ", "then", "rescue", "->", "=>", "@", "$", "::", ".", "=begin\n", "\n=end\n",
    "__END__\n", "\xc3\xa9", "\xff", "\0",
};

static const char STOPS[] = "\"'`/%<#{}()[]|+-\n\\:?", BYTES[] = "\"'`#{}()[]<>|/%+-*=:;,.?! \n\\$@&";

static Edit random_edit(const char *text, uint32_t len) {
    Edit e = {0};
    e.start = below(len + 1);
    if (below(2)) { // move to the next byte that opens or closes a literal, interpolation, heredoc or operator
        while (e.start < len && !memchr(STOPS, text[e.start], sizeof STOPS - 1)) e.start++;
        if (e.start < len && below(2)) e.start++;
    }
    uint32_t kind = below(3); // insert, delete, replace
    if (kind != 0) e.del = below(4) ? 1 + below(4) : 1 + below(64);
    if (e.del > len - e.start) e.del = len - e.start;
    if (kind == 1) return e;
    uint32_t source = below(3);
    if (source == 0) { // a fragment of Ruby
        const char *f = FRAGMENTS[below(sizeof FRAGMENTS / sizeof *FRAGMENTS)];
        e.ins_len = f[0] ? (uint32_t)strlen(f) : 1; // "\0" inserts a NUL byte
        memcpy(e.ins, f, e.ins_len);
    } else if (source == 1 && len > 0) { // a copy of some bytes of the text
        uint32_t from = below(len);
        e.ins_len = 1 + below(16);
        if (e.ins_len > len - from) e.ins_len = len - from;
        memcpy(e.ins, text + from, e.ins_len);
    } else {
        e.ins[0] = BYTES[below(sizeof BYTES - 1)];
        e.ins_len = 1;
    }
    return e;
}

static char *apply(const char *text, uint32_t len, const Edit *e, uint32_t *out_len) {
    *out_len = len - e->del + e->ins_len;
    char *out = malloc(*out_len + 1);
    memcpy(out, text, e->start);
    memcpy(out + e->start, e->ins, e->ins_len);
    memcpy(out + e->start + e->ins_len, text + e->start + e->del, len - e->start - e->del);
    return out;
}

static TSPoint point_at(const char *text, uint32_t byte) {
    TSPoint p = {0, 0};
    for (uint32_t i = 0; i < byte; i++) {
        if (text[i] == '\n') p.row++, p.column = 0;
        else p.column++;
    }
    return p;
}

// Edits a copy of `old` (the tree of `before`) by `e`, reparses `after` with it and from scratch, and returns the
// incremental tree. *bad is set when the two trees differ and one of them has no error (two error recoveries may
// differ, as they start from different parse states). *inc and *fresh get both trees as S-expressions, when not NULL.
static TSTree *reparse(TSParser *parser, const TSTree *old, const char *before, const Edit *e, const char *after,
                       uint32_t after_len, int *bad, char **inc, char **fresh) {
    TSInputEdit edit = {
        e->start, e->start + e->del, e->start + e->ins_len,
        point_at(before, e->start), point_at(before, e->start + e->del), point_at(after, e->start + e->ins_len),
    };
    TSTree *edited = ts_tree_copy(old);
    ts_tree_edit(edited, &edit);
    TSTree *tree = ts_parser_parse_string(parser, edited, after, after_len);
    TSTree *scratch = ts_parser_parse_string(parser, NULL, after, after_len);
    TSNode a = ts_tree_root_node(tree), b = ts_tree_root_node(scratch);
    char *sa = ts_node_string(a), *sb = ts_node_string(b);
    int differ = strcmp(sa, sb) != 0;
    *bad = differ && !(ts_node_has_error(a) && ts_node_has_error(b));
    recoveries += differ && !*bad;
    if (inc) *inc = sa, *fresh = sb;
    else free(sa), free(sb);
    ts_tree_delete(edited);
    ts_tree_delete(scratch);
    return tree;
}

// Applies `e` to *text and reparses, replacing *tree with the incremental tree. Returns whether they mismatch.
static int step(TSParser *parser, TSTree **tree, char **text, uint32_t *len, const Edit *e, char **inc, char **fresh) {
    uint32_t after_len;
    char *after = apply(*text, *len, e, &after_len);
    int bad;
    TSTree *next_tree = reparse(parser, *tree, *text, e, after, after_len, &bad, inc, fresh);
    ts_tree_delete(*tree);
    free(*text);
    *tree = next_tree, *text = after, *len = after_len;
    return bad;
}

// Parses `text` from scratch, then applies the edits one by one; returns whether the last reparse mismatches.
static int fails(TSParser *parser, const char *text, uint32_t len, const Edit *edits, int count, char **inc,
                 char **fresh) {
    char *t = malloc(len + 1);
    memcpy(t, text, len);
    TSTree *tree = ts_parser_parse_string(parser, NULL, t, len);
    int bad = 0;
    for (int i = 0; i < count; i++) {
        if (inc && i > 0) free(*inc), free(*fresh);
        bad = step(parser, &tree, &t, &len, &edits[i], inc, fresh);
    }
    ts_tree_delete(tree);
    free(t);
    return bad;
}

// Removes chunks of `text` that no edit touches, then bytes of the last edit, while the mismatch stays.
static void shrink(TSParser *parser, char *text, uint32_t *len, Edit *edits, int count) {
    Edit *s = malloc(count * sizeof *s);
    for (uint32_t chunk = *len / 2 ? *len / 2 : 1; chunk > 0; chunk /= 2) {
        for (uint32_t at = 0; at + chunk <= *len;) {
            memcpy(s, edits, count * sizeof *s);
            int clear = 1; // follow the chunk through the edits, moving those after it
            for (uint32_t p = at, i = 0; clear && i < (uint32_t)count; i++) {
                if (p + chunk <= s[i].start) s[i].start -= chunk;
                else if (p >= s[i].start + s[i].del) p = p - s[i].del + s[i].ins_len;
                else clear = 0;
            }
            char *t = malloc(*len - chunk + 1);
            memcpy(t, text, at);
            memcpy(t + at, text + at + chunk, *len - at - chunk);
            if (clear && fails(parser, t, *len - chunk, s, count, NULL, NULL)) {
                memcpy(text, t, *len - chunk);
                memcpy(edits, s, count * sizeof *s);
                *len -= chunk;
            } else {
                at += chunk;
            }
            free(t);
        }
    }
    free(s);
    Edit *last = &edits[count - 1];
    for (uint32_t i = 0; i < last->ins_len;) {
        Edit saved = *last;
        memmove(last->ins + i, last->ins + i + 1, --last->ins_len - i);
        if (!fails(parser, text, *len, edits, count, NULL, NULL)) *last = saved, i++;
    }
    while (last->del > 0) {
        last->del--;
        if (!fails(parser, text, *len, edits, count, NULL, NULL)) {
            last->del++;
            break;
        }
    }
}

static char *replay(const char *text, uint32_t *len, const Edit *edits, int count) {
    char *t = malloc(*len + 1);
    memcpy(t, text, *len);
    for (int i = 0; i < count; i++) {
        char *next_text = apply(t, *len, &edits[i], len);
        free(t);
        t = next_text;
    }
    return t;
}

static void print_bytes(const char *s, uint32_t n) { // as a Ruby string literal
    putchar('"');
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c == '#') printf("\\%c", c);
        else if (c == '\n') printf("\\n");
        else if (c < 0x20 || c >= 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
    printf("\"\n");
}

// Prints the fewest last edits of `history` that still mismatch after a fresh parse, on the smallest text.
static void report(TSParser *parser, const char *path, uint64_t seed, int done, const char *original, uint32_t len,
                   Edit *history, int count) {
    unsigned long counted = recoveries;
    printf("MISMATCH seed=%llu file=%s (replay: edit_fuzz -s %llu -n %d %s)\n", (unsigned long long)seed, path,
           (unsigned long long)seed, done, path);
    int m = 1;
    char *text;
    for (;; m++) {
        uint32_t n = len;
        text = replay(original, &n, history, count - m);
        if (m == count || fails(parser, text, n, history + count - m, m, NULL, NULL)) {
            len = n;
            break;
        }
        free(text);
    }
    Edit *edits = history + count - m;
    shrink(parser, text, &len, edits, m);
    char *inc, *fresh;
    fails(parser, text, len, edits, m, &inc, &fresh);
    printf("text:        ");
    print_bytes(text, len);
    for (int i = 0; i < m; i++) {
        printf("edit %d:      at byte %u delete %u insert ", i + 1, edits[i].start, edits[i].del);
        print_bytes(edits[i].ins, edits[i].ins_len);
    }
    size_t d = 0; // the trees from a little before their first difference
    while (inc[d] && inc[d] == fresh[d]) d++;
    d = d > 200 ? d - 200 : 0;
    printf("incremental: %.600s\nfresh:       %.600s\n\n", inc + d, fresh + d);
    free(text), free(inc), free(fresh);
    recoveries = counted;
}

static void on_death(void) {
    fprintf(stderr, "while editing %s, replay: edit_fuzz -s %llu -n %d %s\n", now.path, (unsigned long long)now.seed,
            now.edits, now.path);
}

static int fuzz_file(TSParser *parser, const char *path, uint64_t seed, int edits) {
    now.path = path, now.seed = seed, now.edits = edits;
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    uint32_t len = (uint32_t)ftell(file), original_len = len;
    fseek(file, 0, SEEK_SET);
    char *original = malloc(len + 1), *text = malloc(len + 1);
    if (fread(original, 1, len, file) != len) {
        fprintf(stderr, "%s: read failed\n", path);
        exit(2);
    }
    fclose(file);
    memcpy(text, original, len);
    rng = seed;
    for (const char *c = path; *c; c++) rng = (rng ^ (unsigned char)*c) * 0x100000001b3; // one sequence per file
    Edit *history = malloc(2 * (size_t)edits * sizeof *history + 1);
    int count = 0, done = 0, bad = 0;
    TSTree *tree = ts_parser_parse_string(parser, NULL, text, len);
    while (done < edits && !bad) {
        // A few edits, then their undos in reverse order: the undos reparse the original text from broken trees
        Edit undo[3];
        int n = 0, k = 1 + (int)below(3);
        for (; n < k && done < edits && !bad; n++, done++) {
            Edit e = random_edit(text, len);
            undo[n] = (Edit){e.start, e.ins_len, e.del, {0}};
            memcpy(undo[n].ins, text + e.start, e.del);
            history[count++] = e;
            bad = step(parser, &tree, &text, &len, &e, NULL, NULL);
        }
        while (n-- > 0 && !bad) {
            history[count++] = undo[n];
            bad = step(parser, &tree, &text, &len, &undo[n], NULL, NULL);
        }
    }
    if (bad) report(parser, path, seed, done, original, original_len, history, count);
    ts_tree_delete(tree);
    free(original), free(text), free(history);
    return bad;
}

int main(int argc, char **argv) {
    uint64_t seed = 1;
    int edits = 100, i = 1;
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (!strcmp(argv[i], "-s")) seed = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "-n")) edits = atoi(argv[i + 1]);
        else break;
    }
    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(on_death);
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_ruby());
    int status = 0, first = i;
    for (; i < argc; i++) status |= fuzz_file(parser, argv[i], seed, edits);
    fprintf(stderr, "%d files, %d edits each: %lu reparses differ from a fresh parse only in error recovery\n",
            argc - first, edits, recoveries);
    ts_parser_delete(parser);
    return status;
}
