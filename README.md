# tree-sitter-ruby

[![CI][ci]](https://github.com/tree-sitter/tree-sitter-ruby/actions/workflows/ci.yml)
[![discord][discord]](https://discord.gg/w7nTvsVJhm)
[![matrix][matrix]](https://matrix.to/#/#tree-sitter-chat:matrix.org)
[![crates][crates]](https://crates.io/crates/tree-sitter-ruby)
[![npm][npm]](https://www.npmjs.com/package/tree-sitter-ruby)
[![pypi][pypi]](https://pypi.org/project/tree-sitter-ruby)

Ruby grammar for [tree-sitter](https://github.com/tree-sitter/tree-sitter).

#### Development

The parser is generated for ABI 15, which tree-sitter 0.25 and newer runtimes read (Neovim 0.11, py-tree-sitter 0.25,
node-tree-sitter 0.25, the Rust crate 0.25):

```sh
npm ci
node_modules/.bin/tree-sitter generate
node_modules/.bin/tree-sitter test
```

Deno can load `grammar.js` instead of Node, and lints it:

```sh
TREE_SITTER_JS_RUNTIME=deno node_modules/.bin/tree-sitter generate
deno lint
```

`bundle exec ruby script/prism_diff.rb DIR` compares the trees with [Prism](https://github.com/ruby/prism) on the Ruby files in `DIR`.

#### Local variables

Like Ruby, the parser reads `x +1` as `x + 1` when `x` is a local variable, and as the call `x(+1)` otherwise. The external
scanner tracks assignments, parameters, `for` and `rescue =>` variables, numbered parameters, and the scopes of blocks,
methods, classes and modules. Variables bound by pattern matching (`in [x]`, `=> x`) are not tracked, so there `x +1`
stays a call.

#### References

- [AST Format of the Whitequark parser](https://github.com/whitequark/parser/blob/master/doc/AST_FORMAT.md)

[ci]: https://img.shields.io/github/actions/workflow/status/tree-sitter/tree-sitter-ruby/ci.yml?logo=github&label=CI
[discord]: https://img.shields.io/discord/1063097320771698699?logo=discord&label=discord
[matrix]: https://img.shields.io/matrix/tree-sitter-chat%3Amatrix.org?logo=matrix&label=matrix
[npm]: https://img.shields.io/npm/v/tree-sitter-ruby?logo=npm
[crates]: https://img.shields.io/crates/v/tree-sitter-ruby?logo=rust
[pypi]: https://img.shields.io/pypi/v/tree-sitter-ruby?logo=pypi&logoColor=ffd242
