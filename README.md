# tree-sitter-ruby

[![CI][ci]](https://github.com/Magikdidi24/tree-sitter-ruby/actions/workflows/ci.yml)
[![release][release]](https://github.com/Magikdidi24/tree-sitter-ruby/releases/latest)
[![npm][npm]](https://www.npmjs.com/package/@magikdidi24/tree-sitter-ruby)
[![crates][crates]](https://crates.io/crates/tree-sitter-ruby-magikdidi24)
[![pypi][pypi]](https://pypi.org/project/tree-sitter-ruby-magikdidi24/)
[![coverage][coverage]](https://github.com/Magikdidi24/tree-sitter-ruby/actions/workflows/ci.yml)
[![scorecard][scorecard]](https://scorecard.dev/viewer/?uri=github.com/Magikdidi24/tree-sitter-ruby)
[![performance][performance]](https://magikdidi24.github.io/tree-sitter-ruby/perf.html)
[![playground][playground]](https://magikdidi24.github.io/tree-sitter-ruby/)
[![discord][discord]](https://discord.gg/w7nTvsVJhm)
[![matrix][matrix]](https://matrix.to/#/#tree-sitter-chat:matrix.org)

Ruby grammar for [tree-sitter](https://github.com/tree-sitter/tree-sitter), a maintained fork of
[tree-sitter/tree-sitter-ruby](https://github.com/tree-sitter/tree-sitter-ruby). Try it in the
[playground](https://magikdidi24.github.io/tree-sitter-ruby/).

#### Install

The import names are the upstream ones (`tree_sitter_ruby`), so only the dependency changes:

```sh
cargo add tree-sitter-ruby-magikdidi24
npm install @magikdidi24/tree-sitter-ruby
pip install tree-sitter-ruby-magikdidi24
go get github.com/Magikdidi24/tree-sitter-ruby@v0.25.0
```

#### Editors

Editors fetch upstream's grammar by default; point them at this repository instead. They need a tree-sitter 0.25 or
newer runtime (ABI 15). The editors' own Ruby queries work with this grammar.

Neovim with nvim-treesitter (`main` branch), then `:TSInstall! ruby`:

```lua
vim.api.nvim_create_autocmd('User', { pattern = 'TSUpdate', callback = function()
  local info = require('nvim-treesitter.parsers').ruby.install_info
  info.url = 'https://github.com/Magikdidi24/tree-sitter-ruby'
  info.revision = 'df14fbe651cb06eeec66ec9be0b9279035d06639' -- v0.26.0
end })
```

Helix, in `languages.toml`, then `hx --grammar fetch && hx --grammar build`:

```toml
[[grammar]]
name = "ruby"
source = { git = "https://github.com/Magikdidi24/tree-sitter-ruby", rev = "df14fbe651cb06eeec66ec9be0b9279035d06639" }
```

Emacs 30, then `M-x treesit-install-language-grammar RET ruby`:

```elisp
(add-to-list 'treesit-language-source-alist
             '(ruby "https://github.com/Magikdidi24/tree-sitter-ruby" "v0.26.0"))
```

#### Development

The parser is generated for ABI 15, which tree-sitter 0.25 and newer runtimes read (Neovim 0.11, py-tree-sitter 0.25,
node-tree-sitter 0.25, the Rust crate 0.25):

```sh
npm ci
node_modules/.bin/tree-sitter generate
node_modules/.bin/tree-sitter test
```

Older runtimes (Neovim 0.10, tree-sitter 0.22 to 0.24) need a parser for ABI 14: `node_modules/.bin/tree-sitter generate --abi 14`.

Deno can load `grammar.js` instead of Node, and lints it:

```sh
TREE_SITTER_JS_RUNTIME=deno node_modules/.bin/tree-sitter generate
deno lint
```

`bundle exec ruby script/prism_diff.rb DIR` compares the trees with [Prism](https://github.com/ruby/prism) on the Ruby files in `DIR`.
`script/coverage.sh` prints how much of the hand-written scanner the tests run, by line and by branch; the coverage
badge shows it for master.
The [performance history](https://magikdidi24.github.io/tree-sitter-ruby/perf.html) shows the parse cost and the parser
size of every release.

#### Local variables

Like Ruby, the parser reads `x +1` as `x + 1` when `x` is a local variable, and as the call `x(+1)` otherwise. The external
scanner tracks assignments, parameters, `for` and `rescue =>` variables, numbered parameters, variables bound by pattern
matching (`in [x]`, `in {name:}`, `in {"name":}`, `v => x`), and the scopes of blocks, methods, classes and modules.

#### References

- [AST Format of the Whitequark parser](https://github.com/whitequark/parser/blob/master/doc/AST_FORMAT.md)

[ci]: https://img.shields.io/github/actions/workflow/status/Magikdidi24/tree-sitter-ruby/ci.yml?branch=master&logo=github&label=CI
[release]: https://img.shields.io/github/v/release/Magikdidi24/tree-sitter-ruby?logo=github
[npm]: https://img.shields.io/npm/v/%40magikdidi24%2Ftree-sitter-ruby?logo=npm
[crates]: https://img.shields.io/crates/v/tree-sitter-ruby-magikdidi24?logo=rust
[pypi]: https://img.shields.io/pypi/v/tree-sitter-ruby-magikdidi24?logo=pypi
[coverage]: https://img.shields.io/endpoint?url=https%3A%2F%2Fmagikdidi24.github.io%2Ftree-sitter-ruby%2Fcoverage.json
[scorecard]: https://api.scorecard.dev/projects/github.com/Magikdidi24/tree-sitter-ruby/badge
[performance]: https://img.shields.io/badge/performance-history-blue
[playground]: https://img.shields.io/badge/playground-try%20it-blue?logo=webassembly
[discord]: https://img.shields.io/discord/1063097320771698699?logo=discord&label=discord
[matrix]: https://img.shields.io/matrix/tree-sitter-chat%3Amatrix.org?logo=matrix&label=matrix
