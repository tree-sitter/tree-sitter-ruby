#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <string.h>

typedef enum {
    LINE_BREAK,
    NO_LINE_BREAK,

    // Delimited literals
    SIMPLE_SYMBOL,
    STRING_START,
    SYMBOL_START,
    SUBSHELL_START,
    REGEX_START,
    STRING_ARRAY_START,
    SYMBOL_ARRAY_START,
    HEREDOC_BODY_START,
    STRING_CONTENT,
    HEREDOC_CONTENT,
    STRING_END,
    HEREDOC_BODY_END,
    HEREDOC_START,

    // Whitespace-sensitive tokens
    FORWARD_SLASH,
    BLOCK_AMPERSAND,
    SPLAT_STAR,
    UNARY_MINUS,
    UNARY_MINUS_NUM,
    UNARY_PLUS,
    BINARY_MINUS,
    BINARY_STAR,
    SINGLETON_CLASS_LEFT_ANGLE_LEFT_ANGLE,
    HASH_KEY_SYMBOL,
    IDENTIFIER_SUFFIX,
    CONSTANT_SUFFIX,
    HASH_SPLAT_STAR_STAR,
    BINARY_STAR_STAR,
    ELEMENT_REFERENCE_BRACKET,
    SHORT_INTERPOLATION,
    LAMBDA_BRACE,
    COMMENT,
    UNINTERPRETED,
    OPERAND_NEXT,
    LOCAL_VARIABLES,
    PARAMETER_START,
    SCOPE_START,
    SCOPE_END,
    ENDLESS_METHOD,

    NONE
} TokenType;

typedef Array(char) String;

typedef struct {
    TokenType type;
    int32_t open_delimiter;
    int32_t close_delimiter;
    int32_t nesting_depth;
    bool allows_interpolation;
} Literal;

typedef struct {
    String word;
    bool end_word_indentation_allowed;
    bool allows_interpolation;
    bool started;
    // Body content resumes after an interpolation or escape, where no end word can start
    bool mid_line;
} Heredoc;

typedef struct {
    bool has_leading_whitespace;
    Array(Literal) literal_stack;
    Array(Heredoc) open_heredocs;
    // Hashes of the local variables in scope
    Array(uint32_t) locals;
    // Size of `locals` where each open scope started, with HIDES_OUTER for `def`, `class` and `module` and ENDLESS for
    // an endless method
    Array(uint8_t) scopes;
    // First character of the operator after a local variable (the `+` of `x +1`), 0 if none
    char binary_operator;
    // The `def`, `class` or `module` (DEF, CLASS, MODULE) whose scope the last token opened, so that it opens only once
    uint8_t opened;
} Scanner;

// shortcut: past 63 names or 32 nested scopes the new ones are ignored, and read as method calls as on master
#define MAX_LOCALS 63
#define MAX_SCOPES 32
#define HIDES_OUTER 0x80
#define ENDLESS 0x40
#define SAVED_SIZE 0x3F

enum { DEF = 1, CLASS, MODULE };

static const char NON_IDENTIFIER_CHARS[] = {
    '\0', '\n', '\r', '\t', ' ', ':', ';', '`',  '"', '\'', '@', '$', '#', '.', ',', '|', '^', '&',
    '<',  '=',  '>',  '+',  '-', '*', '/', '\\', '%', '?',  '!', '~', '(', ')', '[', ']', '{', '}',
};

// ASCII only, like Ruby: the isw* functions depend on the host process locale
static inline bool is_space(int32_t c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline bool is_upper(int32_t c) { return c >= 'A' && c <= 'Z'; }
static inline bool is_lower(int32_t c) { return c >= 'a' && c <= 'z'; }
static inline bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

// Heredoc end words are kept as UTF-8, so a codepoint is never cut to its low byte
static inline uint32_t encode_utf8(int32_t c, char *out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    uint32_t n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    out[0] = (char)((0xF00 >> n) | (c >> (6 * (n - 1))));
    for (uint32_t i = 1; i < n; i++) out[i] = (char)(0x80 | ((c >> (6 * (n - 1 - i))) & 0x3F));
    return n;
}

static inline void skip(Scanner *scanner, TSLexer *lexer) {
    scanner->has_leading_whitespace = true;
    lexer->advance(lexer, true);
}

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void reset(Scanner *scanner) {
    array_delete(&scanner->literal_stack);
    for (uint32_t i = 0; i < scanner->open_heredocs.size; i++) {
        array_delete(&array_get(&scanner->open_heredocs, i)->word);
    }
    array_delete(&scanner->open_heredocs);
    // Cleared, not freed: deserialize runs before every scan
    array_clear(&scanner->locals);
    array_clear(&scanner->scopes);
    scanner->binary_operator = 0;
    scanner->opened = 0;
}

static inline bool has_local_state(Scanner *scanner) {
    return scanner->locals.size > 0 || scanner->scopes.size > 0 || scanner->binary_operator != 0 || scanner->opened;
}

static inline unsigned local_state_size(Scanner *scanner) {
    return has_local_state(scanner) ? 4 + scanner->locals.size * sizeof(uint32_t) + scanner->scopes.size : 0;
}

static inline unsigned serialize(Scanner *scanner, char *buffer) {
    unsigned size = 0;

    // shortcut: all state must fit TREE_SITTER_SERIALIZATION_BUFFER_SIZE (1024 bytes); past ~200 open literals or
    // ~1000 bytes of pending heredoc words it is dropped and the rest parses wrong, upgrade if real code gets there
    if (scanner->literal_stack.size * 6 + 2 >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
        return 0;
    }

    buffer[size++] = (char)scanner->literal_stack.size;
    for (uint32_t i = 0; i < scanner->literal_stack.size; i++) {
        Literal *literal = array_get(&scanner->literal_stack, i);
        buffer[size++] = literal->type;
        buffer[size++] = (char)literal->open_delimiter;
        buffer[size++] = (char)literal->close_delimiter;
        buffer[size++] = (char)(literal->nesting_depth & 0xFF);
        buffer[size++] = (char)(literal->nesting_depth >> 8);
        buffer[size++] = (char)literal->allows_interpolation;
    }

    buffer[size++] = (char)scanner->open_heredocs.size;
    for (uint32_t i = 0; i < scanner->open_heredocs.size; i++) {
        Heredoc *heredoc = array_get(&scanner->open_heredocs, i);
        if (size + 4 + sizeof(uint32_t) + heredoc->word.size >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
            return 0;
        }
        buffer[size++] = (char)heredoc->end_word_indentation_allowed;
        buffer[size++] = (char)heredoc->allows_interpolation;
        buffer[size++] = (char)heredoc->started;
        buffer[size++] = (char)heredoc->mid_line;
        memcpy(&buffer[size], &heredoc->word.size, sizeof(uint32_t));
        size += sizeof(uint32_t);
        memcpy(&buffer[size], heredoc->word.contents, heredoc->word.size);
        size += heredoc->word.size;
    }

    // Written only when set, so code without local variables keeps the same state as before. Dropped when it
    // does not fit: local variables only ever add up to that through heredocs or literals opened after them.
    if (has_local_state(scanner) && size + local_state_size(scanner) <= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
        buffer[size++] = scanner->binary_operator;
        buffer[size++] = (char)scanner->opened;
        buffer[size++] = (char)scanner->locals.size;
        if (scanner->locals.size > 0) {
            memcpy(&buffer[size], scanner->locals.contents, scanner->locals.size * sizeof(uint32_t));
            size += scanner->locals.size * sizeof(uint32_t);
        }
        buffer[size++] = (char)scanner->scopes.size;
        if (scanner->scopes.size > 0) {
            memcpy(&buffer[size], scanner->scopes.contents, scanner->scopes.size);
            size += scanner->scopes.size;
        }
    }

    return size;
}

static inline void deserialize(Scanner *scanner, const char *buffer, unsigned length) {
    unsigned size = 0;
    scanner->has_leading_whitespace = false;
    reset(scanner);

    if (length == 0) {
        return;
    }

    uint8_t literal_depth = buffer[size++];
    for (unsigned j = 0; j < literal_depth; j++) {
        Literal literal = {0};
        literal.type = (TokenType)(buffer[size++]);
        literal.open_delimiter = (unsigned char)buffer[size++];
        literal.close_delimiter = (unsigned char)buffer[size++];
        literal.nesting_depth = (unsigned char)buffer[size] | ((unsigned char)buffer[size + 1] << 8);
        size += 2;
        literal.allows_interpolation = buffer[size++];
        array_push(&scanner->literal_stack, literal);
    }

    uint8_t open_heredoc_count = buffer[size++];
    for (unsigned j = 0; j < open_heredoc_count; j++) {
        Heredoc heredoc = {0};
        heredoc.end_word_indentation_allowed = buffer[size++];
        heredoc.allows_interpolation = buffer[size++];
        heredoc.started = buffer[size++];
        heredoc.mid_line = buffer[size++];

        heredoc.word = (String)array_new();
        uint32_t word_length;
        memcpy(&word_length, &buffer[size], sizeof(uint32_t));
        size += sizeof(uint32_t);
        array_reserve(&heredoc.word, word_length);
        memcpy(heredoc.word.contents, &buffer[size], word_length);
        heredoc.word.size = word_length;
        size += word_length;
        array_push(&scanner->open_heredocs, heredoc);
    }

    if (size < length) {
        scanner->binary_operator = buffer[size++];
        scanner->opened = buffer[size++];
        uint8_t count = buffer[size++];
        if (count > 0) {
            array_reserve(&scanner->locals, count);
            memcpy(scanner->locals.contents, &buffer[size], count * sizeof(uint32_t));
            scanner->locals.size = count;
            size += count * sizeof(uint32_t);
        }
        count = buffer[size++];
        if (count > 0) {
            array_reserve(&scanner->scopes, count);
            memcpy(scanner->scopes.contents, &buffer[size], count);
            scanner->scopes.size = count;
            size += count;
        }
    }

    assert(size == length);
}

// A `#` comment ends at a newline, or at the end of an included range (e.g. an ERB tag)
static inline void advance_comment(TSLexer *lexer) {
    advance(lexer);
    while (lexer->lookahead != '\n' && !lexer->eof(lexer) && !lexer->is_at_included_range_start(lexer)) {
        advance(lexer);
    }
}

static inline bool scan_comment(TSLexer *lexer) {
    advance_comment(lexer);
    // At the start of an included range, this marks the end of the previous one
    lexer->mark_end(lexer);
    lexer->result_symbol = COMMENT;
    return true;
}

static inline bool scan_whitespace(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols,
                                   bool *rescue_may_end_line) {
    bool heredoc_body_start_is_valid = scanner->open_heredocs.size > 0 && !scanner->open_heredocs.contents[0].started &&
                                       valid_symbols[HEREDOC_BODY_START];
    bool crossed_newline = false;

    for (;;) {
        if (!valid_symbols[NO_LINE_BREAK] && valid_symbols[LINE_BREAK] && lexer->is_at_included_range_start(lexer)) {
            lexer->mark_end(lexer);
            lexer->result_symbol = LINE_BREAK;
            return true;
        }

        switch (lexer->lookahead) {
            case ' ':
            case '\t':
                skip(scanner, lexer);
                break;
            case '\r':
                if (heredoc_body_start_is_valid) {
                    lexer->result_symbol = HEREDOC_BODY_START;
                    scanner->open_heredocs.contents[0].started = true;
                    return true;
                } else {
                    skip(scanner, lexer);
                    break;
                }
            case '\n':
                if (heredoc_body_start_is_valid) {
                    lexer->result_symbol = HEREDOC_BODY_START;
                    scanner->open_heredocs.contents[0].started = true;
                    return true;
                } else if (!valid_symbols[NO_LINE_BREAK] && valid_symbols[LINE_BREAK] && !crossed_newline) {
                    lexer->mark_end(lexer);
                    advance(lexer);
                    crossed_newline = true;
                } else {
                    // After a hash shorthand (`foo key:`) the value may follow on the next line, unless that
                    // line is a `rescue` clause
                    *rescue_may_end_line = valid_symbols[NO_LINE_BREAK] && valid_symbols[LINE_BREAK];
                    skip(scanner, lexer);
                }
                break;
            case '\\':
                advance(lexer);
                if (lexer->lookahead == '\r') {
                    skip(scanner, lexer);
                }
                if (is_space(lexer->lookahead)) {
                    skip(scanner, lexer);
                } else {
                    return false;
                }
                break;
            default:
                // Not after `__END__` (only there is UNINTERPRETED valid without STRING_START)
                if (lexer->lookahead == '#' && valid_symbols[COMMENT] &&
                    (!valid_symbols[UNINTERPRETED] || valid_symbols[STRING_START])) {
                    // After a newline the token starts at the `#` only if indentation was skipped
                    if (!crossed_newline || lexer->get_column(lexer) > 0) {
                        return scan_comment(lexer);
                    }
                    // So leave a column-0 comment to the lexer, whose `#.*` would not stop at the end of an included
                    // range: there emit the (zero-width) line break first, and the comment is scanned after it
                    advance_comment(lexer);
                    if (!lexer->is_at_included_range_start(lexer)) {
                        return false;
                    }
                    lexer->result_symbol = LINE_BREAK;
                    return true;
                }
                if (crossed_newline) {
                    // Ruby 4.0: a line starting with `||`, `and` or `or` continues the previous one
                    // (`&&` is covered by the `&` case below)
                    if (lexer->lookahead == '|') {
                        advance(lexer);
                        if (lexer->lookahead == '|') {
                            return false;
                        }
                        lexer->result_symbol = LINE_BREAK;
                        return true;
                    }
                    if (lexer->lookahead == 'a' || lexer->lookahead == 'o') {
                        const char *word = lexer->lookahead == 'a' ? "and" : "or";
                        unsigned i = 0;
                        while (word[i] && lexer->lookahead == word[i]) {
                            advance(lexer);
                            i++;
                        }
                        if (!word[i] && !is_lower(lexer->lookahead) && !is_upper(lexer->lookahead) &&
                            !is_digit(lexer->lookahead) && lexer->lookahead != '_' &&
                            lexer->lookahead != '?' && lexer->lookahead != '!' && lexer->lookahead != ':' &&
                            lexer->lookahead < 0x80) {
                            return false;
                        }
                        lexer->result_symbol = LINE_BREAK;
                        return true;
                    }
                    if (lexer->lookahead != '.' && lexer->lookahead != '&' && lexer->lookahead != '#') {
                        lexer->result_symbol = LINE_BREAK;
                    } else if (lexer->lookahead == '.') {
                        // Don't return LINE_BREAK for the call operator (`.`) but do return one for range
                        // operators
                        // (`..` and `...`)
                        advance(lexer);
                        if (!lexer->eof(lexer) && lexer->lookahead == '.') {
                            lexer->result_symbol = LINE_BREAK;
                        } else {
                            return false;
                        }
                    }
                }
                return true;
        }
    }
}

static inline bool scan_operator(TSLexer *lexer) {
    switch (lexer->lookahead) {
        // <, <=, <<, <=>
        case '<':
            advance(lexer);
            if (lexer->lookahead == '<') {
                advance(lexer);
            } else if (lexer->lookahead == '=') {
                advance(lexer);
                if (lexer->lookahead == '>') {
                    advance(lexer);
                }
            }
            return true;

        // >, >=, >>
        case '>':
            advance(lexer);
            if (lexer->lookahead == '>' || lexer->lookahead == '=') {
                advance(lexer);
            }
            return true;

        // ==, ===, =~
        case '=':
            advance(lexer);
            if (lexer->lookahead == '~') {
                advance(lexer);
                return true;
            }
            if (lexer->lookahead == '=') {
                advance(lexer);
                if (lexer->lookahead == '=') {
                    advance(lexer);
                }
                return true;
            }
            return false;

        // +, -, ~, +@, -@, ~@
        case '+':
        case '-':
        case '~':
            advance(lexer);
            if (lexer->lookahead == '@') {
                advance(lexer);
            }
            return true;

        // ..
        case '.':
            advance(lexer);
            if (lexer->lookahead == '.') {
                advance(lexer);
                return true;
            }
            return false;

        // &, ^, |, /, %`
        case '&':
        case '^':
        case '|':
        case '/':
        case '%':
        case '`':
            advance(lexer);
            return true;

        // !, !=, !~
        case '!':
            advance(lexer);
            if (lexer->lookahead == '=' || lexer->lookahead == '~') {
                advance(lexer);
            }
            return true;

        // *, **
        case '*':
            advance(lexer);
            if (lexer->lookahead == '*') {
                advance(lexer);
            }
            return true;

        // [], []=
        case '[':
            advance(lexer);
            if (lexer->lookahead == ']') {
                advance(lexer);
            } else {
                return false;
            }
            if (lexer->lookahead == '=') {
                advance(lexer);
            }
            return true;

        default:
            return false;
    }
}

static inline bool is_iden_char(int32_t c) {
    return c >= 0x80 || memchr(&NON_IDENTIFIER_CHARS, c, sizeof(NON_IDENTIFIER_CHARS)) == NULL;
}

// Local variables start like method names, not like constants
static inline bool is_local_start(int32_t c) { return is_lower(c) || c == '_' || c >= 0x80; }

static inline bool is_one_of(int32_t c, const char *chars) { return c > 0 && c < 0x80 && strchr(chars, c) != NULL; }

// FNV-1a: 32 bits, so that two names in scope practically never collide
static inline uint32_t hash_char(uint32_t hash, int32_t c) { return (hash ^ (uint32_t)c) * 16777619u; }
#define HASH_START 2166136261u

// Keywords are never local variables: `return %=a=` must not declare `return`, nor `def f(if:)` declare `if`
static const char *const KEYWORDS[] = {
    "BEGIN", "END",  "__FILE__", "__LINE__", "alias",  "and",    "begin", "break", "case",  "class",  "def",
    "do",    "else", "elsif",    "end",      "ensure", "false",  "for",   "if",    "in",    "module", "next",
    "nil",   "not",  "or",       "redo",     "rescue", "retry",  "return", "self", "super", "then",   "true",
    "undef", "unless", "until",  "when",     "while",  "yield",
};
#define KEYWORD_MAX_LENGTH 8

// A name read with scan_name: its hash, and its text when it may be a keyword
typedef struct {
    uint32_t hash;
    uint32_t length;
    char text[KEYWORD_MAX_LENGTH + 1];
} Name;

static inline void name_add_char(Name *name, int32_t c) {
    name->hash = hash_char(name->hash, c);
    if (name->length < KEYWORD_MAX_LENGTH) {
        name->text[name->length] = c < 0x80 ? (char)c : '\0';
    }
    name->length++;
}

static inline bool name_is(const Name *name, const char *word) {
    return strlen(word) == name->length && memcmp(name->text, word, name->length) == 0;
}

static inline bool is_keyword(const Name *name) {
    if (name->length > KEYWORD_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (name_is(name, KEYWORDS[i])) {
            return true;
        }
    }
    return false;
}

static inline Name scan_name(TSLexer *lexer) {
    Name name = {.hash = HASH_START};
    while (is_iden_char(lexer->lookahead)) {
        name_add_char(&name, lexer->lookahead);
        advance(lexer);
    }
    return name;
}

// The locals of the innermost `def`, `class` or `module` and of the blocks in it: the outer ones are hidden
static inline bool is_local(Scanner *scanner, uint32_t name) {
    uint32_t start = 0;
    for (uint32_t i = scanner->scopes.size; i > 0; i--) {
        uint8_t scope = *array_get(&scanner->scopes, i - 1);
        if (scope & HIDES_OUTER) {
            start = scope & SAVED_SIZE;
            break;
        }
    }
    for (uint32_t i = start; i < scanner->locals.size; i++) {
        if (*array_get(&scanner->locals, i) == name) {
            return true;
        }
    }
    return false;
}

// Returns whether the name is new
static inline bool add_local(Scanner *scanner, uint32_t name) {
    if (scanner->locals.size == MAX_LOCALS || is_local(scanner, name)) {
        return false;
    }
    array_push(&scanner->locals, name);
    return true;
}

// A zero-width token is returned only when serialize() keeps its state: otherwise the same change would be
// made again at the same place, forever
static inline bool local_state_fits(Scanner *scanner) {
    unsigned size = 2 + scanner->literal_stack.size * 6;
    for (uint32_t i = 0; i < scanner->open_heredocs.size; i++) {
        size += 4 + sizeof(uint32_t) + array_get(&scanner->open_heredocs, i)->word.size;
    }
    return size + local_state_size(scanner) < TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
}

static inline bool emit_zero_width(Scanner *scanner, TSLexer *lexer, TokenType symbol) {
    if (!local_state_fits(scanner)) {
        return false;
    }
    lexer->result_symbol = symbol;
    return true;
}

// `=` (not `==`, `=~` or `=>`), or an operator assignment such as `+=`, `||=` or `<<=` (not `<=`). Also tells
// whether a space follows the first character, as in `x + 1` but not in `x +1`.
static inline bool scan_assignment_operator(TSLexer *lexer, bool *space_after) {
    int32_t op = lexer->lookahead;
    *space_after = true;
    if (op != '=' && !is_one_of(op, "+-*/%^|&<>")) {
        return false;
    }
    advance(lexer);
    *space_after = is_space(lexer->lookahead);
    if (op == '=') {
        return !is_one_of(lexer->lookahead, "=~>");
    }
    if (lexer->lookahead == op && is_one_of(op, "*|&<>")) {
        advance(lexer);
    } else if (op == '<' || op == '>') {
        return false;
    }
    return lexer->lookahead == '=';
}

// `a, (b, *c), @d = ...` declares the plain names, as does `for a, b in` (`for_loop`); anything else (`a.b, c = ...`)
// declares nothing. Reads from after `first`, if any.
// shortcut: past 16 targets the look-ahead gives up, so that `x = a, a, ...` stays linear
static inline bool scan_multiple_assignment(Scanner *scanner, TSLexer *lexer, const Name *first, bool for_loop) {
    uint32_t names[16];
    uint32_t count = 0;
    uint32_t items = 0;
    // Below 0 in `(a, b), c = 1` read from `a`
    int32_t depth = 0;
    if (first) {
        names[count++] = first->hash;
        items++;
    }
    for (;;) {
        int32_t c = lexer->lookahead;
        if (is_one_of(c, " \t,*")) {
            advance(lexer);
        } else if (c == '(' || c == ')') {
            depth += c == '(' ? 1 : -1;
            advance(lexer);
        } else if (items == sizeof(names) / sizeof(names[0])) {
            return false;
        } else if (c == '@' || c == '$' || is_upper(c)) {
            while (lexer->lookahead == '@' || lexer->lookahead == '$') {
                advance(lexer);
            }
            scan_name(lexer);
            items++;
        } else if (is_local_start(c)) {
            Name name = scan_name(lexer);
            if (for_loop && name_is(&name, "in")) {
                break;
            }
            if (is_keyword(&name)) {
                return false;
            }
            names[count++] = name.hash;
            items++;
        } else if (c == '=' && !for_loop && depth <= 0) {
            advance(lexer);
            if (is_one_of(lexer->lookahead, "=~>")) {
                return false;
            }
            break;
        } else {
            return false;
        }
    }
    bool added = false;
    for (uint32_t i = 0; i < count; i++) {
        added |= add_local(scanner, names[i]);
    }
    return added;
}

// `_1` to `_9`, in a block
static inline bool is_numbered_parameter(Scanner *scanner, const Name *name) {
    return name->length == 2 && name->text[0] == '_' && name->text[1] >= '1' && name->text[1] <= '9' &&
           scanner->scopes.size > 0 && !(*array_back(&scanner->scopes) & HIDES_OUTER);
}

// After a name that starts an operand: `x = 1`, `x += 1`, `x, y = ...` and `for x in` declare local variables; in
// `x +1` after one Ruby reads a binary operator, so that operator is kept for the next scan; and any other name
// drops an operator still kept.
static inline bool scan_local_variable(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols, const Name *name,
                                       char kept_operator, uint8_t opened) {
    bool changed = false;
    if (is_keyword(name)) {
        uint8_t kind = name_is(name, "def") ? DEF : name_is(name, "class") ? CLASS : name_is(name, "module") ? MODULE : 0;
        // shortcut: the same keyword again before any other scanner token (`def a; def b`) opens no scope, as if it
        // were this one scanned twice; its variables then stay until the outer scope ends
        if (kind && kind != opened && scanner->scopes.size < MAX_SCOPES) {
            array_push(&scanner->scopes, (uint8_t)(scanner->locals.size | HIDES_OUTER));
            scanner->opened = kind;
            changed = true;
        } else if (name_is(name, "for")) {
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            }
            changed = scan_multiple_assignment(scanner, lexer, NULL, true);
        }
    } else {
        bool has_space = false;
        int32_t op;
        for (;;) {
            op = lexer->lookahead;
            if (op == ' ' || op == '\t') {
                advance(lexer);
                has_space = true;
                continue;
            }
            if (op != '\\') {
                break;
            }
            // A line continuation: `x \` then `+1`
            advance(lexer);
            if (lexer->lookahead == '\r') {
                advance(lexer);
            }
            if (lexer->lookahead != '\n') {
                op = 0;
                break;
            }
            advance(lexer);
            has_space = true;
        }
        if (op == ',') {
            // Only as a statement: in `foo a, b = 1` and `[a, b = 1]` (where `a:` could start a hash) only `b` is
            // assigned
            if (!valid_symbols[HASH_KEY_SYMBOL]) {
                changed = scan_multiple_assignment(scanner, lexer, name, false);
            }
        } else if (op != 0) {
            bool local = has_space && is_one_of(op, "+-*&[/%<") &&
                         (is_local(scanner, name->hash) || is_numbered_parameter(scanner, name));
            bool space_after;
            if (scan_assignment_operator(lexer, &space_after)) {
                changed = add_local(scanner, name->hash);
            } else if (local && (op == '[' || !space_after)) {
                scanner->binary_operator = (char)op;
            }
        }
    }
    if (!changed && scanner->binary_operator == kept_operator) {
        return false;
    }
    return emit_zero_width(scanner, lexer, LOCAL_VARIABLES);
}

// A parameter (`a`, `*a`, `**a`, `&a`, `a:`, `a = 1`) or the variable of `rescue => e` is a local variable, but not
// `rescue => e.x`: returns whether the name is new
static inline bool bind_name(Scanner *scanner, TSLexer *lexer, const Name *name) {
    return !is_keyword(name) && !is_one_of(lexer->lookahead, ".[(?!") && add_local(scanner, name->hash);
}

// Right after `{`, `do` or `->` of a block or lambda: its parameters and new variables are dropped where it ends
static inline bool scan_scope_start(Scanner *scanner, TSLexer *lexer) {
    if (scanner->scopes.size == MAX_SCOPES) {
        return false;
    }
    array_push(&scanner->scopes, (uint8_t)scanner->locals.size);
    if (!emit_zero_width(scanner, lexer, SCOPE_START)) {
        array_pop(&scanner->scopes);
        return false;
    }
    return true;
}

static inline void close_scope(Scanner *scanner) {
    uint8_t saved = array_pop(&scanner->scopes) & SAVED_SIZE;
    if (saved < scanner->locals.size) {
        scanner->locals.size = saved;
    }
}

// An endless method's scope ends with its line or statement: returns whether one did
static inline bool close_endless(Scanner *scanner) {
    if (scanner->scopes.size == 0 || !(*array_back(&scanner->scopes) & ENDLESS)) {
        return false;
    }
    close_scope(scanner);
    return true;
}

// Right after the `end` or `}` of a block, lambda, method, class or module
static inline bool scan_scope_end(Scanner *scanner, TSLexer *lexer) {
    while (close_endless(scanner)) {
    }
    if (scanner->scopes.size > 0) {
        close_scope(scanner);
    } else if (scanner->locals.size > 0) {
        array_clear(&scanner->locals);
    } else {
        return false;
    }
    lexer->result_symbol = SCOPE_END;
    return true;
}

// A word that ends the body of an endless method: `def f = 1 if x`, `class A; def f = 1 end`
static inline bool ends_endless_body(const Name *name, int32_t next) {
    static const char *const WORDS[] = {"end", "else", "elsif", "ensure", "if",  "unless",
                                        "while", "until", "then", "when", "in"};
    if (next == ':' || next == '?' || next == '!') {
        return false;
    }
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        if (name_is(name, WORDS[i])) {
            return true;
        }
    }
    return false;
}

static inline bool scan_symbol_identifier(TSLexer *lexer) {
    if (lexer->lookahead == '@') {
        advance(lexer);
        if (lexer->lookahead == '@') {
            advance(lexer);
        }
    } else if (lexer->lookahead == '$') {
        advance(lexer);
    }

    if (is_iden_char(lexer->lookahead)) {
        advance(lexer);
    } else if (!scan_operator(lexer)) {
        return false;
    }

    while (is_iden_char(lexer->lookahead)) {
        advance(lexer);
    }

    if (lexer->lookahead == '?' || lexer->lookahead == '!') {
        advance(lexer);
    }

    if (lexer->lookahead == '=') {
        lexer->mark_end(lexer);
        advance(lexer);
        if (lexer->lookahead != '>') {
            lexer->mark_end(lexer);
        }
    }

    return true;
}

typedef struct {
    char letter;
    // `%s` is valid only where a simple symbol (`:foo`) is
    TokenType valid_symbol;
    TokenType type;
    bool allows_interpolation;
} PercentLiteral;

static const PercentLiteral PERCENT_LITERALS[] = {
    {'s', SIMPLE_SYMBOL, SYMBOL_START, false},
    {'r', REGEX_START, REGEX_START, true},
    {'x', SUBSHELL_START, SUBSHELL_START, true},
    {'q', STRING_START, STRING_START, false},
    {'Q', STRING_START, STRING_START, true},
    {'w', STRING_ARRAY_START, STRING_ARRAY_START, false},
    {'W', STRING_ARRAY_START, STRING_ARRAY_START, true},
    {'i', SYMBOL_ARRAY_START, SYMBOL_ARRAY_START, false},
    {'I', SYMBOL_ARRAY_START, SYMBOL_ARRAY_START, true},
};

static inline bool scan_open_delimiter(Scanner *scanner, TSLexer *lexer, Literal *literal, const bool *valid_symbols) {
    // After `..`/`...` (NO_LINE_BREAK) and `return`/`break`/`next` (OPERAND_NEXT), Ruby reads an operand even
    // though binary operators are valid there. STRING_CONTENT valid along with STRING_START means error recovery.
    const bool binary_operator_valid =
        valid_symbols[FORWARD_SLASH] &&
        (!(valid_symbols[NO_LINE_BREAK] || valid_symbols[OPERAND_NEXT]) || valid_symbols[STRING_CONTENT]);

    switch (lexer->lookahead) {
        case '"':
            literal->type = STRING_START;
            literal->open_delimiter = literal->close_delimiter = lexer->lookahead;
            literal->allows_interpolation = true;
            advance(lexer);
            return true;

        case '\'':
            literal->type = STRING_START;
            literal->open_delimiter = literal->close_delimiter = lexer->lookahead;
            literal->allows_interpolation = false;
            advance(lexer);
            return true;

        case '`':
            if (!valid_symbols[SUBSHELL_START]) {
                return false;
            }
            literal->type = SUBSHELL_START;
            literal->open_delimiter = literal->close_delimiter = lexer->lookahead;
            literal->allows_interpolation = true;
            advance(lexer);
            return true;

        case '/':
            if (!valid_symbols[REGEX_START]) {
                return false;
            }
            literal->type = REGEX_START;
            literal->open_delimiter = literal->close_delimiter = lexer->lookahead;
            literal->allows_interpolation = true;
            advance(lexer);
            if (binary_operator_valid) {
                if (!scanner->has_leading_whitespace) {
                    return false;
                }
                if (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\n' ||
                    lexer->lookahead == '\r') {
                    return false;
                }
                if (lexer->lookahead == '=') {
                    return false;
                }
            }
            return true;

        case '%':
            advance(lexer);
            // Like Ruby, read `%=` and `%` followed by whitespace as operators wherever a binary operator may follow
            if ((lexer->lookahead == '=' || is_space(lexer->lookahead)) && binary_operator_valid) {
                return false;
            }

            // Without a letter (`%(...)`) it is an interpolating string
            PercentLiteral percent = {0, STRING_START, STRING_START, true};
            for (size_t i = 0; i < sizeof(PERCENT_LITERALS) / sizeof(PERCENT_LITERALS[0]); i++) {
                if (lexer->lookahead == PERCENT_LITERALS[i].letter) {
                    percent = PERCENT_LITERALS[i];
                }
            }
            if (!valid_symbols[percent.valid_symbol]) {
                return false;
            }
            literal->type = percent.type;
            literal->allows_interpolation = percent.allows_interpolation;
            if (percent.letter) {
                advance(lexer);
            }

            switch (lexer->lookahead) {
                case '(':
                    literal->open_delimiter = '(';
                    literal->close_delimiter = ')';
                    break;

                case '[':
                    literal->open_delimiter = '[';
                    literal->close_delimiter = ']';
                    break;

                case '{':
                    literal->open_delimiter = '{';
                    literal->close_delimiter = '}';
                    break;

                case '<':
                    literal->open_delimiter = '<';
                    literal->close_delimiter = '>';
                    break;

                case '\r':
                case '\n':
                case ' ':
                case '\t':
                case '|':
                case '!':
                case '#':
                case '/':
                case '\\':
                case '@':
                case '$':
                case '%':
                case '^':
                case '&':
                case '*':
                case ')':
                case ']':
                case '}':
                case '>':
                case '=':
                case '+':
                case '-':
                case '~':
                case '`':
                case ',':
                case '.':
                case '?':
                case ':':
                case ';':
                case '_':
                case '"':
                case '\'':
                    literal->open_delimiter = lexer->lookahead;
                    literal->close_delimiter = lexer->lookahead;
                    break;
                default:
                    return false;
            }

            advance(lexer);
            return true;

        default:
            return false;
    }
}

static inline void scan_heredoc_word(TSLexer *lexer, Heredoc *heredoc) {
    String word = array_new();
    int32_t quote = 0;

    switch (lexer->lookahead) {
        case '\'':
        case '"':
        case '`':
            quote = lexer->lookahead;
            advance(lexer);
            while (lexer->lookahead != quote && !lexer->eof(lexer)) {
                { char utf8[4]; array_extend(&word, encode_utf8(lexer->lookahead, utf8), utf8); }
                advance(lexer);
            }
            advance(lexer);
            break;

        default:
            if (is_iden_char(lexer->lookahead)) {
                { char utf8[4]; array_extend(&word, encode_utf8(lexer->lookahead, utf8), utf8); }
                advance(lexer);
                while (is_iden_char(lexer->lookahead)) {
                    { char utf8[4]; array_extend(&word, encode_utf8(lexer->lookahead, utf8), utf8); }
                    advance(lexer);
                }
            }
            break;
    }

    heredoc->word = word;
    heredoc->allows_interpolation = quote != '\'';
}

static inline bool scan_short_interpolation(TSLexer *lexer, const bool has_content, const TSSymbol content_symbol) {
    int32_t start = lexer->lookahead;
    if (start == '@' || start == '$') {
        if (has_content) {
            lexer->result_symbol = content_symbol;
            return true;
        }
        lexer->mark_end(lexer);
        advance(lexer);
        bool is_short_interpolation = false;
        if (start == '$') {
            if (lexer->lookahead > 0 && lexer->lookahead < 0x80 &&
                strchr("!@&`'+~=/\\,;.<>*$?:\"", (char)lexer->lookahead) != NULL) {
                is_short_interpolation = true;
            } else {
                if (lexer->lookahead == '-') {
                    advance(lexer);
                    is_short_interpolation = is_iden_char(lexer->lookahead) && !is_digit(lexer->lookahead);
                } else {
                    is_short_interpolation = is_iden_char(lexer->lookahead);
                }
            }
        }
        if (start == '@') {
            if (lexer->lookahead == '@') {
                advance(lexer);
            }
            is_short_interpolation = is_iden_char(lexer->lookahead) && !is_digit(lexer->lookahead);
        }

        if (is_short_interpolation) {
            lexer->result_symbol = SHORT_INTERPOLATION;
            return true;
        }
    }
    return false;
}

// Index of the heredoc whose body is being read (open_heredocs.size if none): heredocs opened in its
// interpolations sit before it, waiting for the end of the line
static inline uint32_t current_heredoc_index(Scanner *scanner) {
    uint32_t index = 0;
    while (index < scanner->open_heredocs.size && !array_get(&scanner->open_heredocs, index)->started) {
        index++;
    }
    return index;
}

static inline bool scan_heredoc_content(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    uint32_t current = current_heredoc_index(scanner);
    if (current == scanner->open_heredocs.size) {
        current = 0;
    }
    Heredoc *heredoc = array_get(&scanner->open_heredocs, current);
    size_t position_in_word = 0;
    bool look_for_heredoc_end = !heredoc->mid_line;
    bool has_content = false;

    for (;;) {
        if (position_in_word == heredoc->word.size) {
            if (!has_content) {
                lexer->mark_end(lexer);
            }
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->eof(lexer)) {
                if (has_content) {
                    heredoc->mid_line = false;
                    lexer->result_symbol = HEREDOC_CONTENT;
                } else {
                    array_delete(&heredoc->word);
                    array_erase(&scanner->open_heredocs, current);
                    lexer->result_symbol = HEREDOC_BODY_END;
                }
                return true;
            }
            has_content = true;
            position_in_word = 0;
        }

        if (lexer->eof(lexer)) {
            lexer->mark_end(lexer);
            if (has_content) {
                lexer->result_symbol = HEREDOC_CONTENT;
            } else {
                array_delete(&heredoc->word);
                array_erase(&scanner->open_heredocs, current);
                lexer->result_symbol = HEREDOC_BODY_END;
            }
            return true;
        }

        char utf8[4];
        uint32_t utf8_length = encode_utf8(lexer->lookahead, utf8);
        if (look_for_heredoc_end && position_in_word + utf8_length <= heredoc->word.size &&
            memcmp(&heredoc->word.contents[position_in_word], utf8, utf8_length) == 0) {
            advance(lexer);
            position_in_word += utf8_length;
        } else {
            position_in_word = 0;
            look_for_heredoc_end = false;

            if (heredoc->allows_interpolation && lexer->lookahead == '\\') {
                if (has_content) {
                    heredoc->mid_line = true;
                    lexer->result_symbol = HEREDOC_CONTENT;
                    return true;
                }
                return false;
            }

            if (heredoc->allows_interpolation && lexer->lookahead == '#') {
                lexer->mark_end(lexer);
                advance(lexer);
                if (lexer->lookahead == '{') {
                    if (has_content) {
                        heredoc->mid_line = true;
                        lexer->result_symbol = HEREDOC_CONTENT;
                        return true;
                    }
                    return false;
                }
                heredoc->mid_line = true;
                if (scan_short_interpolation(lexer, has_content, HEREDOC_CONTENT)) {
                    return true;
                }
                has_content = true;
                lexer->mark_end(lexer);
            } else if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
                // A nested heredoc body starts at the end of the line that opened it
                if (current > 0 && valid_symbols[HEREDOC_BODY_START]) {
                    if (has_content) {
                        lexer->result_symbol = HEREDOC_CONTENT;
                    } else {
                        array_get(&scanner->open_heredocs, 0)->started = true;
                        lexer->result_symbol = HEREDOC_BODY_START;
                    }
                    return true;
                }
                if (lexer->lookahead == '\r') {
                    advance(lexer);
                    if (lexer->lookahead == '\n') {
                        advance(lexer);
                    }
                } else {
                    advance(lexer);
                }
                has_content = true;
                look_for_heredoc_end = true;
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    advance(lexer);
                    if (!heredoc->end_word_indentation_allowed) {
                        look_for_heredoc_end = false;
                    }
                }
                lexer->mark_end(lexer);
            } else {
                has_content = true;
                advance(lexer);
                lexer->mark_end(lexer);
            }
        }
    }
}

static inline bool scan_literal_content(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    Literal *literal = array_back(&scanner->literal_stack);
    bool has_content = false;
    bool stop_on_space = literal->type == SYMBOL_ARRAY_START || literal->type == STRING_ARRAY_START;

    for (;;) {
        if (stop_on_space && is_space(lexer->lookahead)) {
            if (has_content) {
                lexer->mark_end(lexer);
                lexer->result_symbol = STRING_CONTENT;
                return true;
            }
            return false;
        }
        if (lexer->lookahead == literal->close_delimiter) {
            lexer->mark_end(lexer);
            if (literal->nesting_depth == 1) {
                if (has_content) {
                    lexer->result_symbol = STRING_CONTENT;
                } else {
                    advance(lexer);
                    if (literal->type == REGEX_START) {
                        while (is_lower(lexer->lookahead)) {
                            advance(lexer);
                        }
                    }
                    array_pop(&scanner->literal_stack);
                    lexer->result_symbol = STRING_END;
                    lexer->mark_end(lexer);
                }
                return true;
            }
            literal->nesting_depth--;
            advance(lexer);

        } else if (lexer->lookahead == literal->open_delimiter) {
            literal->nesting_depth++;
            advance(lexer);
        } else if (literal->allows_interpolation && lexer->lookahead == '#') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '{') {
                if (has_content) {
                    lexer->result_symbol = STRING_CONTENT;
                    return true;
                }
                return false;
            }
            if (scan_short_interpolation(lexer, has_content, STRING_CONTENT)) {
                return true;
            }
        } else if (lexer->lookahead == '\\') {
            // In a regex, `\d`, `\/`, ... belong to the pattern, not to escape sequences
            if (literal->allows_interpolation && literal->type != REGEX_START) {
                if (has_content) {
                    lexer->mark_end(lexer);
                    lexer->result_symbol = STRING_CONTENT;
                    return true;
                }
                return false;
            }
            advance(lexer);
            advance(lexer);

        } else if (lexer->lookahead == '\n' && scanner->open_heredocs.size > 0 &&
                   !array_get(&scanner->open_heredocs, 0)->started && valid_symbols[HEREDOC_BODY_START]) {
            // A heredoc opened in an interpolation starts its body at the end of this line
            lexer->mark_end(lexer);
            if (has_content) {
                lexer->result_symbol = STRING_CONTENT;
            } else {
                array_get(&scanner->open_heredocs, 0)->started = true;
                lexer->result_symbol = HEREDOC_BODY_START;
            }
            return true;
        } else if (lexer->eof(lexer)) {
            advance(lexer);
            lexer->mark_end(lexer);
            return false;
        } else {
            advance(lexer);
        }

        has_content = true;
    }
}

// After `*` or `**`: `foo *args` is a splat, `a*b` and `a * b` are binary
static inline bool scan_star_operator(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols, TokenType binary,
                                      TokenType splat, bool after_local) {
    if (lexer->lookahead == '=') {
        return false;
    }
    if (valid_symbols[binary] && (!scanner->has_leading_whitespace || after_local)) {
        lexer->result_symbol = binary;
        return true;
    }
    if (valid_symbols[splat] && !is_space(lexer->lookahead)) {
        lexer->result_symbol = splat;
        return true;
    }
    if (valid_symbols[binary]) {
        lexer->result_symbol = binary;
        return true;
    }
    if (valid_symbols[splat]) {
        lexer->result_symbol = splat;
        return true;
    }
    return false;
}

static inline bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    scanner->has_leading_whitespace = false;
    // Set by the LOCAL_VARIABLES token before `x` in `x +1`, cleared by any other token
    const char binary_operator = scanner->binary_operator;
    const uint8_t opened = scanner->opened;
    scanner->binary_operator = 0;
    scanner->opened = 0;

    // STRING_CONTENT is valid along with everything else only in error recovery
    const bool error_recovery = valid_symbols[STRING_CONTENT] && valid_symbols[STRING_START];
    if (valid_symbols[SCOPE_END] && !error_recovery && scan_scope_end(scanner, lexer)) {
        return true;
    }
    if (valid_symbols[SCOPE_START] && !error_recovery && scan_scope_start(scanner, lexer)) {
        return true;
    }

    // Contents of literals, which match any character except for some close delimiter
    if (!valid_symbols[STRING_START]) {
        if ((valid_symbols[STRING_CONTENT] || valid_symbols[STRING_END]) && scanner->literal_stack.size > 0) {
            return scan_literal_content(scanner, lexer, valid_symbols);
        }
        if ((valid_symbols[HEREDOC_CONTENT] || valid_symbols[HEREDOC_BODY_END]) && scanner->open_heredocs.size > 0) {
            return scan_heredoc_content(scanner, lexer, valid_symbols);
        }
    }

    // Whitespace
    lexer->result_symbol = NONE;
    bool rescue_may_end_line = false;
    if (!scan_whitespace(scanner, lexer, valid_symbols, &rescue_may_end_line)) {
        return false;
    }
    if (lexer->result_symbol != NONE) {
        // shortcut: a line break inside an endless method (in a multi-line `if`) also ends its scope
        if (lexer->result_symbol == LINE_BREAK && !error_recovery) {
            while (close_endless(scanner)) {
            }
        }
        return true;
    }
    if (lexer->lookahead == ';' && valid_symbols[LOCAL_VARIABLES] && !error_recovery && close_endless(scanner)) {
        return emit_zero_width(scanner, lexer, LOCAL_VARIABLES);
    }
    // The `=` of an endless method, not of a setter (`def a=(v)`, where a parameter could start)
    if (valid_symbols[ENDLESS_METHOD] && !error_recovery && lexer->lookahead == '=' &&
        (scanner->has_leading_whitespace || !valid_symbols[PARAMETER_START])) {
        lexer->mark_end(lexer);
        advance(lexer);
        if (is_one_of(lexer->lookahead, "=~>")) {
            return false;
        }
        if (scanner->scopes.size > 0 && (*array_back(&scanner->scopes) & HIDES_OUTER)) {
            *array_back(&scanner->scopes) |= ENDLESS;
        }
        return emit_zero_width(scanner, lexer, ENDLESS_METHOD);
    }

    const bool binds = valid_symbols[PARAMETER_START] && valid_symbols[LOCAL_VARIABLES] && !error_recovery;
    if (binds && (lexer->lookahead == '*' || lexer->lookahead == '&')) {
        lexer->mark_end(lexer);
        while (lexer->lookahead == '*' || lexer->lookahead == '&') {
            advance(lexer);
        }
        if (!is_local_start(lexer->lookahead)) {
            return false;
        }
        Name name = scan_name(lexer);
        return bind_name(scanner, lexer, &name) && emit_zero_width(scanner, lexer, LOCAL_VARIABLES);
    }
    // Only where a binary operator may follow
    const bool after_local = binary_operator != 0 && lexer->lookahead == binary_operator &&
                             valid_symbols[BINARY_STAR] && scanner->has_leading_whitespace;

    switch (lexer->lookahead) {
        case '&':
            if (valid_symbols[BLOCK_AMPERSAND] && !after_local) {
                advance(lexer);
                if (lexer->lookahead != '&' && lexer->lookahead != '.' && lexer->lookahead != '=' &&
                    (!is_space(lexer->lookahead) || !valid_symbols[BINARY_STAR])) {
                    lexer->result_symbol = BLOCK_AMPERSAND;
                    return true;
                }
                return false;
            }
            break;

        case '<':
            if (valid_symbols[SINGLETON_CLASS_LEFT_ANGLE_LEFT_ANGLE]) {
                advance(lexer);
                if (lexer->lookahead == '<') {
                    advance(lexer);
                    lexer->result_symbol = SINGLETON_CLASS_LEFT_ANGLE_LEFT_ANGLE;
                    return true;
                }
                return false;
            }
            break;

        case '*':
            if (valid_symbols[SPLAT_STAR] || valid_symbols[BINARY_STAR] || valid_symbols[HASH_SPLAT_STAR_STAR] ||
                valid_symbols[BINARY_STAR_STAR]) {
                advance(lexer);
                if (lexer->lookahead != '*') {
                    return scan_star_operator(scanner, lexer, valid_symbols, BINARY_STAR, SPLAT_STAR, after_local);
                }
                if (!valid_symbols[HASH_SPLAT_STAR_STAR] && !valid_symbols[BINARY_STAR_STAR]) {
                    return false;
                }
                advance(lexer);
                return scan_star_operator(scanner, lexer, valid_symbols, BINARY_STAR_STAR, HASH_SPLAT_STAR_STAR,
                                          after_local);
            }
            break;

        case '+':
            // `foo +x` is a command argument, like `foo -x`; otherwise `+` is left to the internal lexer
            if (valid_symbols[UNARY_PLUS] && valid_symbols[BINARY_STAR] && scanner->has_leading_whitespace &&
                !after_local) {
                advance(lexer);
                if (lexer->lookahead != '=' && !is_space(lexer->lookahead)) {
                    lexer->result_symbol = UNARY_PLUS;
                    return true;
                }
                return false;
            }
            break;

        case '-':
            if (valid_symbols[UNARY_MINUS] || valid_symbols[UNARY_MINUS_NUM] || valid_symbols[BINARY_MINUS]) {
                advance(lexer);
                if (lexer->lookahead != '=' && lexer->lookahead != '>') {
                    if (valid_symbols[UNARY_MINUS_NUM] &&
                        (!valid_symbols[BINARY_STAR] || (scanner->has_leading_whitespace && !after_local)) &&
                        is_digit(lexer->lookahead)) {
                        lexer->result_symbol = UNARY_MINUS_NUM;
                        return true;
                    }
                    if (valid_symbols[UNARY_MINUS] && scanner->has_leading_whitespace && !is_space(lexer->lookahead) &&
                        !after_local) {
                        lexer->result_symbol = UNARY_MINUS;
                    } else if (valid_symbols[BINARY_MINUS]) {
                        lexer->result_symbol = BINARY_MINUS;
                    } else {
                        lexer->result_symbol = UNARY_MINUS;
                    }
                    return true;
                }
                return false;
            }
            break;

        case ':':
            if (valid_symbols[SYMBOL_START]) {
                Literal literal = {0};
                literal.type = SYMBOL_START;
                literal.nesting_depth = 1;
                advance(lexer);

                switch (lexer->lookahead) {
                    case '"':
                    case '\'':
                        literal.open_delimiter = literal.close_delimiter = lexer->lookahead;
                        literal.allows_interpolation = lexer->lookahead == '"';
                        advance(lexer);
                        array_push(&scanner->literal_stack, literal);
                        lexer->result_symbol = SYMBOL_START;
                        return true;

                    default:
                        if (scan_symbol_identifier(lexer)) {
                            lexer->result_symbol = SIMPLE_SYMBOL;
                            return true;
                        }
                }

                return false;
            }
            break;

        case '{':
            // Only where a lambda body can start, not in error recovery (every symbol valid, STRING_CONTENT included)
            if (valid_symbols[LAMBDA_BRACE] && !valid_symbols[STRING_CONTENT]) {
                advance(lexer);
                lexer->result_symbol = LAMBDA_BRACE;
                return true;
            }
            break;

        case '[':
            // Treat a square bracket as an element reference if either:
            // * the bracket is not preceded by any whitespace
            // * an arbitrary expression is not valid at the current position.
            if (valid_symbols[ELEMENT_REFERENCE_BRACKET] &&
                (!scanner->has_leading_whitespace || !valid_symbols[STRING_START] || after_local)) {
                advance(lexer);
                lexer->result_symbol = ELEMENT_REFERENCE_BRACKET;
                return true;
            }
            break;

        default:
            break;
    }

    // Hash keys (`key:`), method names ending in `!` (`foo!`, `Foo!`), and the line break before a `rescue` that
    // follows a hash shorthand
    // A name where an operand starts may declare or use a local variable
    const bool local_start = is_local_start(lexer->lookahead);
    const bool starts_operand = valid_symbols[STRING_START] && valid_symbols[LOCAL_VARIABLES] && !error_recovery &&
                                local_start;
    const bool starts_name = is_iden_char(lexer->lookahead) && !is_digit(lexer->lookahead);
    // Where no operand is expected, a word such as `if` or `end` may end an endless method's body
    const bool ends_statement = !valid_symbols[STRING_START] && valid_symbols[LOCAL_VARIABLES] && !error_recovery &&
                                scanner->scopes.size > 0 && (*array_back(&scanner->scopes) & ENDLESS);
    if (((valid_symbols[HASH_KEY_SYMBOL] || valid_symbols[IDENTIFIER_SUFFIX]) && starts_name) ||
        (valid_symbols[CONSTANT_SUFFIX] && is_upper(lexer->lookahead)) || starts_operand || (binds && local_start) ||
        (binary_operator != 0 && starts_name) || (ends_statement && is_lower(lexer->lookahead))) {
        TokenType validIdentifierSymbol = is_upper(lexer->lookahead) ? CONSTANT_SUFFIX : IDENTIFIER_SUFFIX;
        static const char RESCUE[] = "rescue";
        uint32_t length = 0;
        Name name = {.hash = HASH_START};
        lexer->mark_end(lexer);
        while (is_iden_char(lexer->lookahead)) {
            if (length >= sizeof(RESCUE) - 1 || lexer->lookahead != RESCUE[length]) {
                rescue_may_end_line = false;
            }
            name_add_char(&name, lexer->lookahead);
            advance(lexer);
            length++;
        }

        if (rescue_may_end_line && length == sizeof(RESCUE) - 1 && !is_iden_char(lexer->lookahead) &&
            lexer->lookahead != '?' && lexer->lookahead != '!' && lexer->lookahead != ':') {
            lexer->result_symbol = LINE_BREAK;
            return true;
        }

        if (ends_statement && ends_endless_body(&name, lexer->lookahead)) {
            close_endless(scanner);
            return emit_zero_width(scanner, lexer, LOCAL_VARIABLES);
        }
        if (binds && local_start && bind_name(scanner, lexer, &name)) {
            return emit_zero_width(scanner, lexer, LOCAL_VARIABLES);
        }
        if (starts_operand && (binary_operator != 0 || is_one_of(lexer->lookahead, " \t=,+-*/%^|&<>\\"))) {
            return scan_local_variable(scanner, lexer, valid_symbols, &name, binary_operator, opened);
        }
        // Any other name drops an operator still kept
        if (binary_operator != 0) {
            return emit_zero_width(scanner, lexer, LOCAL_VARIABLES);
        }

        if (valid_symbols[HASH_KEY_SYMBOL] && lexer->lookahead == ':') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead != ':') {
                lexer->result_symbol = HASH_KEY_SYMBOL;
                return true;
            }
        } else if (valid_symbols[validIdentifierSymbol] && lexer->lookahead == '!') {
            advance(lexer);
            lexer->mark_end(lexer);
            if (lexer->lookahead != '=') {
                lexer->result_symbol = validIdentifierSymbol;
                return true;
            }
        }

        return false;
    }

    // Open delimiters for literals, but `x /2`, `x %(y)` and `x <<y` divide and shift when x is a local variable
    if (valid_symbols[STRING_START] && !after_local) {
        Literal literal = {0};
        literal.nesting_depth = 1;

        if (lexer->lookahead == '<') {
            // After an operand `<<` is a shift, unless it starts a command argument (`foo <<EOS`) or Ruby reads an
            // operand there (`1..<<EOS`, `return<<EOS`, see `binary_operator_valid`)
            if (valid_symbols[BINARY_STAR] &&
                (!(valid_symbols[NO_LINE_BREAK] || valid_symbols[OPERAND_NEXT]) || valid_symbols[STRING_CONTENT]) &&
                (!scanner->has_leading_whitespace || !valid_symbols[SPLAT_STAR])) {
                return false;
            }
            advance(lexer);
            if (lexer->lookahead != '<') {
                return false;
            }
            advance(lexer);

            Heredoc heredoc = {0};
            if (lexer->lookahead == '-' || lexer->lookahead == '~') {
                advance(lexer);
                heredoc.end_word_indentation_allowed = true;
            }

            scan_heredoc_word(lexer, &heredoc);
            if (heredoc.word.size == 0) {
                array_delete(&heredoc.word);
                return false;
            }
            // A heredoc opened inside another heredoc's body is read before the rest of that body
            array_insert(&scanner->open_heredocs, current_heredoc_index(scanner), heredoc);
            lexer->result_symbol = HEREDOC_START;
            return true;
        }

        if (scan_open_delimiter(scanner, lexer, &literal, valid_symbols)) {
            array_push(&scanner->literal_stack, literal);
            lexer->result_symbol = literal.type;
            return true;
        }
        return false;
    }

    return false;
}

void *tree_sitter_ruby_external_scanner_create(void) {
    Scanner *scanner = (Scanner *)ts_calloc(1, sizeof(Scanner));
    return scanner;
}

bool tree_sitter_ruby_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    return scan(scanner, lexer, valid_symbols);
}

unsigned tree_sitter_ruby_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    return serialize(scanner, buffer);
}

void tree_sitter_ruby_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    deserialize(scanner, buffer, length);
}

void tree_sitter_ruby_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    reset(scanner);
    array_delete(&scanner->locals);
    array_delete(&scanner->scopes);
    ts_free(scanner);
}
