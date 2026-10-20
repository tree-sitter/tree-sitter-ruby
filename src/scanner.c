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
} Scanner;

const char NON_IDENTIFIER_CHARS[] = {
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

static inline bool scan_open_delimiter(Scanner *scanner, TSLexer *lexer, Literal *literal, const bool *valid_symbols) {
    // After `..` and `...` (NO_LINE_BREAK, see `range`) binary operators are valid only because the range may be
    // endless: Ruby reads an operand there. STRING_CONTENT is valid along with STRING_START only in error recovery.
    const bool binary_operator_valid =
        valid_symbols[FORWARD_SLASH] && (!valid_symbols[NO_LINE_BREAK] || valid_symbols[STRING_CONTENT]);

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
            // Like Ruby, read `%=` as the operator assignment wherever a binary operator may follow
            if (lexer->lookahead == '=' && binary_operator_valid) {
                return false;
            }

            switch (lexer->lookahead) {
                case 's':
                    if (!valid_symbols[SIMPLE_SYMBOL]) {
                        return false;
                    }
                    literal->type = SYMBOL_START;
                    literal->allows_interpolation = false;
                    advance(lexer);
                    break;

                case 'r':
                    if (!valid_symbols[REGEX_START]) {
                        return false;
                    }
                    literal->type = REGEX_START;
                    literal->allows_interpolation = true;
                    advance(lexer);
                    break;

                case 'x':
                    if (!valid_symbols[SUBSHELL_START]) {
                        return false;
                    }
                    literal->type = SUBSHELL_START;
                    literal->allows_interpolation = true;
                    advance(lexer);
                    break;

                case 'q':
                    if (!valid_symbols[STRING_START]) {
                        return false;
                    }
                    literal->type = STRING_START;
                    literal->allows_interpolation = false;
                    advance(lexer);
                    break;

                case 'Q':
                    if (!valid_symbols[STRING_START]) {
                        return false;
                    }
                    literal->type = STRING_START;
                    literal->allows_interpolation = true;
                    advance(lexer);
                    break;

                case 'w':
                    if (!valid_symbols[STRING_ARRAY_START]) {
                        return false;
                    }
                    literal->type = STRING_ARRAY_START;
                    literal->allows_interpolation = false;
                    advance(lexer);
                    break;

                case 'i':
                    if (!valid_symbols[SYMBOL_ARRAY_START]) {
                        return false;
                    }
                    literal->type = SYMBOL_ARRAY_START;
                    literal->allows_interpolation = false;
                    advance(lexer);
                    break;

                case 'W':
                    if (!valid_symbols[STRING_ARRAY_START]) {
                        return false;
                    }
                    literal->type = STRING_ARRAY_START;
                    literal->allows_interpolation = true;
                    advance(lexer);
                    break;

                case 'I':
                    if (!valid_symbols[SYMBOL_ARRAY_START]) {
                        return false;
                    }
                    literal->type = SYMBOL_ARRAY_START;
                    literal->allows_interpolation = true;
                    advance(lexer);
                    break;

                default:
                    if (!valid_symbols[STRING_START]) {
                        return false;
                    }
                    literal->type = STRING_START;
                    literal->allows_interpolation = true;
                    break;
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
                    // If the `/` operator is valid, then so is the `%` operator, which means
                    // that a `%` followed by whitespace should be considered an operator,
                    // not a percent string.
                    if (valid_symbols[FORWARD_SLASH]) {
                        return false;
                    }
                    break;

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
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            }
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
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
                // A plain `#` is content
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

static inline bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    scanner->has_leading_whitespace = false;

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
        return true;
    }

    switch (lexer->lookahead) {
        case '&':
            if (valid_symbols[BLOCK_AMPERSAND]) {
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
                if (lexer->lookahead == '=') {
                    return false;
                }
                if (lexer->lookahead == '*') {
                    if (valid_symbols[HASH_SPLAT_STAR_STAR] || valid_symbols[BINARY_STAR_STAR]) {
                        advance(lexer);
                        if (lexer->lookahead == '=') {
                            return false;
                        }
                        if (valid_symbols[BINARY_STAR_STAR] && !scanner->has_leading_whitespace) {
                            lexer->result_symbol = BINARY_STAR_STAR;
                            return true;
                        }
                        if (valid_symbols[HASH_SPLAT_STAR_STAR] && !is_space(lexer->lookahead)) {
                            lexer->result_symbol = HASH_SPLAT_STAR_STAR;
                            return true;
                        }
                        if (valid_symbols[BINARY_STAR_STAR]) {
                            lexer->result_symbol = BINARY_STAR_STAR;
                            return true;
                        }
                        if (valid_symbols[HASH_SPLAT_STAR_STAR]) {
                            lexer->result_symbol = HASH_SPLAT_STAR_STAR;
                            return true;
                        }
                        return false;
                    }
                    return false;
                }
                if (valid_symbols[BINARY_STAR] && !scanner->has_leading_whitespace) {
                    lexer->result_symbol = BINARY_STAR;
                    return true;
                }
                if (valid_symbols[SPLAT_STAR] && !is_space(lexer->lookahead)) {
                    lexer->result_symbol = SPLAT_STAR;
                    return true;
                }
                if (valid_symbols[BINARY_STAR]) {
                    lexer->result_symbol = BINARY_STAR;
                    return true;
                }
                if (valid_symbols[SPLAT_STAR]) {
                    lexer->result_symbol = SPLAT_STAR;
                    return true;
                }
                return false;
            }
            break;

        case '+':
            // `foo +x` is a command argument, like `foo -x`; otherwise `+` is left to the internal lexer
            if (valid_symbols[UNARY_PLUS] && valid_symbols[BINARY_STAR] && scanner->has_leading_whitespace) {
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
                        (!valid_symbols[BINARY_STAR] || scanner->has_leading_whitespace) &&
                        is_digit(lexer->lookahead)) {
                        lexer->result_symbol = UNARY_MINUS_NUM;
                        return true;
                    }
                    if (valid_symbols[UNARY_MINUS] && scanner->has_leading_whitespace && !is_space(lexer->lookahead)) {
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
                        advance(lexer);
                        literal.open_delimiter = '"';
                        literal.close_delimiter = '"';
                        literal.allows_interpolation = true;
                        array_push(&scanner->literal_stack, literal);
                        lexer->result_symbol = SYMBOL_START;
                        return true;

                    case '\'':
                        advance(lexer);
                        literal.open_delimiter = '\'';
                        literal.close_delimiter = '\'';
                        literal.allows_interpolation = false;
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
            // Valid only where a lambda body can start, which is the meaning `{` has there in Ruby. Not during
            // error recovery (every symbol valid, STRING_CONTENT included)
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
                (!scanner->has_leading_whitespace || !valid_symbols[STRING_START])) {
                advance(lexer);
                lexer->result_symbol = ELEMENT_REFERENCE_BRACKET;
                return true;
            }
            break;

        default:
            break;
    }

    // Open delimiters for literals
    if (((valid_symbols[HASH_KEY_SYMBOL] || valid_symbols[IDENTIFIER_SUFFIX]) &&
         (is_iden_char(lexer->lookahead) && !is_digit(lexer->lookahead))) ||
        (valid_symbols[CONSTANT_SUFFIX] && is_upper(lexer->lookahead))) {
        TokenType validIdentifierSymbol = is_upper(lexer->lookahead) ? CONSTANT_SUFFIX : IDENTIFIER_SUFFIX;
        static const char RESCUE[] = "rescue";
        uint32_t length = 0;
        lexer->mark_end(lexer);
        while (is_iden_char(lexer->lookahead)) {
            if (length >= sizeof(RESCUE) - 1 || lexer->lookahead != RESCUE[length]) {
                rescue_may_end_line = false;
            }
            advance(lexer);
            length++;
        }

        if (rescue_may_end_line && length == sizeof(RESCUE) - 1 && !is_iden_char(lexer->lookahead) &&
            lexer->lookahead != '?' && lexer->lookahead != '!' && lexer->lookahead != ':') {
            // Zero-width line break before `rescue`
            lexer->result_symbol = LINE_BREAK;
            return true;
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

    // Open delimiters for literals
    if (valid_symbols[STRING_START]) {
        Literal literal = {0};
        literal.nesting_depth = 1;

        if (lexer->lookahead == '<') {
            // After an operand `<<` is a shift, unless it starts a command argument (`foo <<EOS`)
            if (valid_symbols[BINARY_STAR] && (!scanner->has_leading_whitespace || !valid_symbols[SPLAT_STAR])) {
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

void *tree_sitter_ruby_external_scanner_create() {
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
    for (uint32_t i = 0; i < scanner->open_heredocs.size; i++) {
        array_delete(&array_get(&scanner->open_heredocs, i)->word);
    }
    array_delete(&scanner->open_heredocs);
    array_delete(&scanner->literal_stack);
    ts_free(scanner);
}
