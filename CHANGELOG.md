# Changelog

Changes of this fork since upstream 0.23.1. Releases: https://github.com/Magikdidi24/tree-sitter-ruby/releases

## Unreleased

- A string key in a hash pattern binds its name, as in Ruby: after `in {"name":}`, `name +1` is `name + 1`.
- `highlights.scm`: a block's implicit parameter, `it` or `_1` to `_9`, is a parameter, no longer a method call.
- `locals.scm`: `in {name:}`, `in {"name":}`, `in (x)` and `in _a | _b` define their names, so later uses are variables;
  a key without a value is highlighted as the variable it binds.

## 0.26.0 (2026-11-20)

- Packages are published with trusted publishing (OIDC): no registry token is stored in the repository.
- 0.25.0 is on npm, crates.io and PyPI; the README installs from there.
- Parser 27 % smaller (`parser.c` 19.9 → 14.6 MB, 7925 → 6123 states, wasm 2.77 → 2.02 MB) and 2 % fewer instructions
  to parse. An endless method is now an `_arg` in `node-types.json`, as in Ruby's grammar.
- CI measures how much of the scanner the tests run: 95.9 % of lines, 86.4 % of branches.
- An incremental reparse gives the same tree as a fresh parse in 6 more cases found by a new edit fuzzer (`<<` and `%w`
  after a reused node, `rescue` or `;` on the line after `key:`, `def x = v in [d]`, a literal left open). A literal
  left open is now content up to the end of the file, as in Prism. The fuzzer runs every night with ASan and UBSan.
- Names bound by pattern matching (`in [x]`, `in {name:}`, `in A => x`, `v => x`, `v in [x]`) are local variables, so
  `x +1` after them is `x + 1`, like Ruby.

## 0.25.0 (2026-11-20)

- `src/parser.c` is generated for ABI 15, read by tree-sitter 0.25 and newer runtimes (Neovim 0.11, py-tree-sitter 0.25,
  node-tree-sitter 0.25, the Rust crate 0.25). It carries the grammar name, version and supertypes. Older runtimes need
  `tree-sitter generate --abi 14`.
- The Neovim, Helix and Zed Ruby queries all compile against this grammar.
- Playground on GitHub Pages: https://magikdidi24.github.io/tree-sitter-ruby/
- Packages under this fork's names, with the upstream import names (`tree_sitter_ruby`): `@magikdidi24/tree-sitter-ruby`
  on npm, `tree-sitter-ruby-magikdidi24` on crates.io and PyPI. Go module `github.com/Magikdidi24/tree-sitter-ruby`.
- CI parses the inputs that once crashed, hung or slowed the scanner under ASan and UBSan, with a 10 s limit per file.
- Identifier characters are looked up in a table: 4.81 s instead of 4.88 s on 46 MB of real Ruby.

## 0.24.1 (2026-11-19)

- Like Ruby, `x +1` is `x + 1` when `x` is a local variable, and the call `x(+1)` otherwise; same for `-`, `*`, `**`,
  `&`, `[`, `/`, `%` and `<<`. The scanner tracks assignments, parameters, `for` and `rescue =>` variables, numbered
  parameters, and the scopes of blocks, lambdas, methods, classes and modules.
- `def self(token)`, hash keys `defined?:`, `key?:` and `key!:`, symbols of special globals (`:$'`, `:$-w`), and
  `return[1]`, `return-1`, `break[1]` with an operand.
- `locals.scm`: a superclass and the object of `class << obj` are read in the outer scope.
- tree-sitter CLI 0.27.2, `deno lint` instead of ESLint, Rust 1.90, no `wheel` build dependency.
- CI actions pinned to commits, read-only tokens, CodeQL on the scanner, Dependabot.
- Prism agrees on 6685 of 6689 files from stdlib, gems and ruby/spec.

## 0.24.0 (2026-10-25)

- Every upstream issue open at the time handled: 21 fixed (heredocs, ERB comments, anonymous `&` block arguments,
  multi-byte symbols, `%` literals, Ruby 4.0 leading `||`/`and`/`or`, a scanner crash on deserialize, ...), 4 closed
  as wontfix with a reason.
- Better highlights, locals and tags, and a new `injections.scm` (regex, heredocs labelled SQL, GraphQL, HTML, Bash,
  Ruby).
- Rust edition 2024 (Rust 1.85), go-tree-sitter 0.25 (Go 1.23), Python 3.10.
- In `node-types.json`, `call.operator` is now `"." | "&." | "::"`.
