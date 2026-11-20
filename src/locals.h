// Local variables, for `x +1`: after one, Ruby reads a binary operator. Included by scanner.c, after the Scanner type
// and the lexer helpers that this code uses.

#ifndef TREE_SITTER_RUBY_LOCALS_H_
#define TREE_SITTER_RUBY_LOCALS_H_

// FNV-1a, folded to the 16 bits that `locals` keeps
static inline uint32_t hash_char(uint32_t hash, int32_t c) { return (hash ^ (uint32_t)c) * 16777619u; }
static inline uint16_t short_hash(uint32_t hash) { return (uint16_t)(hash ^ (hash >> 16)); }
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
        if (*array_get(&scanner->locals, i) == short_hash(name)) {
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
    array_push(&scanner->locals, short_hash(name));
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
    return emit(lexer, symbol);
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

// Where a pattern element starts (`in [x, *y]`, `v => x`), or a `for` variable or a target after the first of
// `a, b = 1` (which declare variables too): a string may start there, but no other operand (`-a`) nor any binary
// operator. Not after `class` (`class <<`) nor where a parameter starts.
static inline bool binds_pattern(const bool *valid_symbols) {
    return valid_symbols[STRING_START] && !valid_symbols[UNARY_MINUS] && !valid_symbols[BINARY_MINUS] &&
           !valid_symbols[SINGLETON_CLASS_LEFT_ANGLE_LEFT_ANGLE] && !valid_symbols[PARAMETER_START];
}

// A key in a hash pattern binds its name when no value follows: `in {k:}` and `in k: then`, not `in {k: v}` nor
// `a::B`. Reads from the `:`.
static inline bool bind_key(Scanner *scanner, TSLexer *lexer, const Name *name) {
    advance(lexer);
    if (lexer->lookahead == ':') {
        return false;
    }
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        advance(lexer);
    }
    if (is_local_start(lexer->lookahead)) {
        Name next = scan_name(lexer);
        if (!name_is(&next, "then") && !name_is(&next, "if") && !name_is(&next, "unless") && !name_is(&next, "and") &&
            !name_is(&next, "or")) {
            return false;
        }
    } else if (!lexer->eof(lexer) && !is_one_of(lexer->lookahead, ",})]|;#\r\n")) {
        return false;
    }
    return !is_keyword(name) && add_local(scanner, name->hash);
}

// `in A => x`: `=>` where neither a binary operator nor an operand may follow, so after a pattern (`{a => x}` and
// `rescue A => e` follow an operand, `rescue => e` follows a keyword). Reads from the `=`.
static inline bool scan_pattern_name(Scanner *scanner, TSLexer *lexer) {
    lexer->mark_end(lexer);
    advance(lexer);
    if (lexer->lookahead != '>') {
        return false;
    }
    advance(lexer);
    while (is_space(lexer->lookahead)) {
        advance(lexer);
    }
    if (!is_local_start(lexer->lookahead)) {
        return false;
    }
    Name name = scan_name(lexer);
    return bind_name(scanner, lexer, &name) && emit_zero_width(scanner, lexer, LOCAL_VARIABLES);
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
    return emit(lexer, SCOPE_END);
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

#endif // TREE_SITTER_RUBY_LOCALS_H_
