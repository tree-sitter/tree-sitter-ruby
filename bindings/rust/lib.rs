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

unsafe extern "C" {
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

/// The language injection query for this language.
pub const INJECTIONS_QUERY: &str = include_str!("../../queries/injections.scm");

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
            ("<% foo\n# note %>text<% bar %>\n", [range(2, 14, (0, 2), (1, 7)), range(22, 27, (1, 15), (1, 20))]),
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

    #[test]
    fn test_heredoc_end_word_at_eof() {
        // Same tree shape whether `EOS` ends the heredoc or is content, only the range differs
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&super::LANGUAGE.into()).unwrap();
        let tree = parser.parse("x = <<EOS\na\nEOS", None).unwrap();
        let end = tree.root_node().named_child(1).unwrap().named_child(1).unwrap();
        assert_eq!((end.kind(), end.start_byte()), ("heredoc_end", 12), "{}", tree.root_node().to_sexp());
    }

    #[test]
    fn test_incremental_reparse_matches_a_fresh_parse() {
        // Editors reparse after each edit, reusing the scanner state saved with each token (literals, heredocs,
        // local variables and their scopes): the tree must be the one a parse from scratch gives
        const SOURCES: [&str; 4] = [
            "x = 1\ndef f(a) = a\nf { |y| y +1; x -1 }\nclass A < B\n  def g(b, c = b +1)\n    b [1]\n  end\nend\n",
            "for i in [1] do i *2 end\nbegin\n  run\nrescue => e\n  e +1\nend\nputs <<~E, \"#{x}\"\n  #{y}\nE\n",
            "module M\n  a, (b, *c) = 1, 2\n  h = { defined?: 1, k: %w[a b] }\n  -> d { d +1 }.call(b)\nend\n",
            "case v\nin [x, *r] if x +1\n  r -1\nin {k:, n: Integer => m}\n  k +1\nend\nv => [w]\nw *2\n",
        ];
        const SNIPPETS: [&str; 10] =
            [" x = 1;", "x +1\n", " def f(a) = a\n", "{ |y| y +1 }", "end\n", "\n", " +1", "do\n", "}", "a, b = 1, 2\n"];
        let point = |text: &str, byte: usize| {
            let line_start = text[..byte].rfind('\n').map_or(0, |i| i + 1);
            tree_sitter::Point::new(text[..byte].matches('\n').count(), byte - line_start)
        };
        let mut seed: u32 = 7;
        let mut random = |n: usize| {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            seed as usize % n
        };
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&super::LANGUAGE.into()).unwrap();
        let mut compared = 0;
        for source in SOURCES {
            // Each edit starts from the last valid text: with a syntax error, error recovery may take another path
            // around the reused nodes
            let mut text = source.to_string();
            let mut tree = parser.parse(&text, None).unwrap();
            for _ in 0..400 {
                let start = random(text.len() + 1);
                let old_end = (start + random(9)).min(text.len());
                let insert = if random(3) > 0 { SNIPPETS[random(SNIPPETS.len())] } else { "" };
                let mut edited = text.clone();
                edited.replace_range(start..old_end, insert);
                let mut old_tree = tree.clone();
                old_tree.edit(&tree_sitter::InputEdit {
                    start_byte: start,
                    old_end_byte: old_end,
                    new_end_byte: start + insert.len(),
                    start_position: point(&text, start),
                    old_end_position: point(&text, old_end),
                    new_end_position: point(&edited, start + insert.len()),
                });
                let fresh = parser.parse(&edited, None).unwrap();
                if fresh.root_node().has_error() {
                    continue;
                }
                let incremental = parser.parse(&edited, Some(&old_tree)).unwrap();
                assert_eq!(incremental.root_node().to_sexp(), fresh.root_node().to_sexp(), "after the edit to:\n{edited}");
                compared += 1;
                (text, tree) = (edited, incremental);
            }
        }
        assert!(compared >= 100, "only {compared} edits left the code valid");
    }

    #[test]
    fn test_incremental_reparse_after_edits_found_by_edit_fuzz() {
        // Texts and their edits (byte, bytes deleted, bytes inserted) from script/edit_fuzz.c: after the last edit, the
        // reparse with the old tree must give the tree of a fresh parse
        type Edit = (usize, usize, &'static [u8]);
        const CASES: [(&[u8], &[Edit]); 4] = [
            // `<<` and `%w` lexed again where no literal starts must still read the byte that decides one
            (b"p break<<-\n.", &[(11, 1, b""), (10, 0, b"B")]),
            (b"e%wlass Foo\n  x +1\n[]=){}", &[(22, 0, b"->("), (3, 16, b"")]),
            // A `;` on the line after `key:`, lexed again after the call is reused
            (b"o B\0o B:\n;", &[(3, 1, b"")]),
            (b"foo key:\n;", &[(1, 0, b"o")]),
        ];
        let point = |text: &[u8], byte: usize| {
            let line_start = text[..byte].iter().rposition(|&c| c == b'\n').map_or(0, |i| i + 1);
            tree_sitter::Point::new(text[..byte].iter().filter(|&&c| c == b'\n').count(), byte - line_start)
        };
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&super::LANGUAGE.into()).unwrap();
        for (source, edits) in CASES {
            let mut text = source.to_vec();
            let mut tree = parser.parse(&text, None).unwrap();
            for &(start, deleted, inserted) in edits {
                let mut edited = text.clone();
                edited.splice(start..start + deleted, inserted.iter().copied());
                tree.edit(&tree_sitter::InputEdit {
                    start_byte: start,
                    old_end_byte: start + deleted,
                    new_end_byte: start + inserted.len(),
                    start_position: point(&text, start),
                    old_end_position: point(&text, start + deleted),
                    new_end_position: point(&edited, start + inserted.len()),
                });
                tree = parser.parse(&edited, Some(&tree)).unwrap();
                text = edited;
            }
            let fresh = parser.parse(&text, None).unwrap();
            let text = String::from_utf8_lossy(&text);
            assert_eq!(tree.root_node().to_sexp(), fresh.root_node().to_sexp(), "after the edits to:\n{text:?}");
        }
    }
}
