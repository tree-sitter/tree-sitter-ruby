//! This crate provides Ruby language support for the [tree-sitter][] parsing library.
//!
//! Typically, you will use the [LANGUAGE][] constant to add this language to a
//! tree-sitter [Parser][], and then use the parser to parse some code:
//!
//! ```
//! use tree_sitter::Parser;
//!
//! let code = r#"
//! def hello(name)
//!  puts "Hello, #{name}!"
//! end
//! "#;
//! let mut parser = Parser::new();
//! let language = tree_sitter_ruby::LANGUAGE;
//! parser
//!     .set_language(&language.into())
//!     .expect("Error loading Ruby parser");
//! let tree = parser.parse(code, None).unwrap();
//! assert!(!tree.root_node().has_error());
//! ```
//!
//! [Parser]: https://docs.rs/tree-sitter/*/tree_sitter/struct.Parser.html
//! [tree-sitter]: https://tree-sitter.github.io/

use tree_sitter_language::LanguageFn;

extern "C" {
    fn tree_sitter_ruby() -> *const ();
}

/// The tree-sitter [`LanguageFn`][LanguageFn] for this grammar.
///
/// [LanguageFn]: https://docs.rs/tree-sitter-language/*/tree_sitter_language/struct.LanguageFn.html
pub const LANGUAGE: LanguageFn = unsafe { LanguageFn::from_raw(tree_sitter_ruby) };

/// The content of the [`node-types.json`][] file for this grammar.
///
/// [`node-types.json`]: https://tree-sitter.github.io/tree-sitter/using-parsers#static-node-types
pub const NODE_TYPES: &str = include_str!("../../src/node-types.json");

/// The syntax highlighting query for this language.
pub const HIGHLIGHTS_QUERY: &str = include_str!("../../queries/highlights.scm");

/// The local-variable syntax highlighting query for this language.
pub const LOCALS_QUERY: &str = include_str!("../../queries/locals.scm");

/// The symbol tagging query for this language.
pub const TAGS_QUERY: &str = include_str!("../../queries/tags.scm");

#[cfg(test)]
mod tests {
    #[test]
    fn test_can_load_grammar() {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("Error loading Ruby parser");
    }

    #[test]
    fn test_comment_stops_at_included_range_end() {
        // Like ERB: only the code between `<%` and `%>` is Ruby
        let range = |start_byte, end_byte, (start_row, start_column), (end_row, end_column)| tree_sitter::Range {
            start_byte,
            end_byte,
            start_point: tree_sitter::Point::new(start_row, start_column),
            end_point: tree_sitter::Point::new(end_row, end_column),
        };
        let cases = [
            ("<% x = 1 # note %>\n<% y = 2 %>\n", [range(2, 16, (0, 2), (0, 16)), range(21, 28, (1, 2), (1, 9))]),
            ("<% foo\n  # note %>text<% bar %>\n", [range(2, 16, (0, 2), (1, 9)), range(24, 29, (1, 17), (1, 22))]),
        ];
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&super::LANGUAGE.into()).unwrap();
        for (source, ranges) in cases {
            parser.set_included_ranges(&ranges).unwrap();
            let tree = parser.parse(source, None).unwrap();
            let root = tree.root_node();
            assert!(!root.has_error(), "{}", root.to_sexp());
            let comment = root.named_child(1).unwrap();
            assert_eq!(comment.kind(), "comment", "{}", root.to_sexp());
            assert_eq!(comment.end_byte(), ranges[0].end_byte, "{}", root.to_sexp());
        }
    }
}
