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
    use tree_sitter::{Node, Parser, Point, Range, Tree};

    fn ruby_parser() -> Parser {
        let mut parser = Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("Error loading Ruby parser");
        parser
    }

    #[test]
    fn test_can_load_grammar() {
        let _ = ruby_parser();
    }

    fn point_at(src: &str, byte: usize) -> Point {
        let mut row = 0;
        let mut column = 0;
        for &b in &src.as_bytes()[..byte] {
            if b == b'\n' {
                row += 1;
                column = 0;
            } else {
                column += 1;
            }
        }
        Point { row, column }
    }

    fn ranges_simple(src: &str) -> Vec<Range> {
        let mut ranges = Vec::new();
        let mut i = 0;
        while let Some(rel) = src[i..].find("<%") {
            let start = i + rel + 2;
            let Some(crel) = src[start..].find("%>") else {
                break;
            };
            let end = start + crel;
            ranges.push(Range {
                start_byte: start,
                end_byte: end,
                start_point: point_at(src, start),
                end_point: point_at(src, end),
            });
            i = end + 2;
        }
        ranges
    }

    fn ranges_erb(src: &str) -> Vec<Range> {
        let bytes = src.as_bytes();
        let mut ranges = Vec::new();
        let mut i = 0;
        while let Some(rel) = src[i..].find("<%") {
            let mut start = i + rel + 2;
            if start < bytes.len() && (bytes[start] == b'=' || bytes[start] == b'-') {
                start += 1;
            }
            let Some(crel) = src[start..].find("%>") else {
                break;
            };
            let close = start + crel;
            let mut end = close;
            if end > start && bytes[end - 1] == b'-' {
                end -= 1;
            }
            ranges.push(Range {
                start_byte: start,
                end_byte: end,
                start_point: point_at(src, start),
                end_point: point_at(src, end),
            });
            i = close + 2;
        }
        ranges
    }

    fn parse_with(src: &str, ranges: &[Range]) -> Tree {
        let mut parser = ruby_parser();
        if !ranges.is_empty() {
            parser
                .set_included_ranges(ranges)
                .expect("valid included ranges");
        }
        parser.parse(src, None).expect("parse produced a tree")
    }

    fn comment_texts<'a>(src: &'a str, tree: &Tree) -> Vec<&'a str> {
        fn walk<'a>(node: Node, src: &'a str, out: &mut Vec<&'a str>) {
            if node.kind() == "comment" {
                out.push(&src[node.start_byte()..node.end_byte()]);
            }
            for i in 0..node.child_count() {
                walk(node.child(i).unwrap(), src, out);
            }
        }
        let mut out = Vec::new();
        walk(tree.root_node(), src, &mut out);
        out
    }

    #[test]
    fn erb_comment_stops_at_included_range_boundary() {
        let cases = [
            (
                "comment then later tags",
                "<div>\n  <% # c %>\n  <p>x</p>\n  <% foo %>\n  <% bar %>\n</div>\n",
                "(program (comment) (identifier) (identifier))",
            ),
            (
                "comment in last tag (hits eof)",
                "<div>\n  <% foo %>\n  <p>x</p>\n  <% # trailing %>\n</div>\n",
                "(program (identifier) (comment))",
            ),
            (
                "comment is entire tag, starts at range start",
                "<div>\n  <%# c %>\n  <p>x</p>\n  <% foo %>\n</div>\n",
                "(program (comment) (identifier))",
            ),
            (
                "two comment tags in a row",
                "<% # a %>\n<p>x</p>\n<% # b %>\n<p>y</p>\n<% zz %>\n",
                "(program (comment) (comment) (identifier))",
            ),
            (
                "multi-line tag: comment ends at real newline inside range",
                "<div>\n  <%\n    # c\n    foo\n  %>\n  <p>x</p>\n  <% bar %>\n</div>\n",
                "(program (comment) (identifier) (identifier))",
            ),
            (
                "code then comment same tag",
                "<% foo # c %>\n<p>x</p>\n<% bar %>\n",
                "(program (identifier) (comment) (identifier))",
            ),
            (
                "no comment at all (regression guard)",
                "<% foo %>\n<p>x</p>\n<% bar %>\n",
                "(program (identifier) (identifier))",
            ),
            (
                "# inside a string is not a comment",
                "<% x = \"a # b\" %>\n<p>q</p>\n<% bar %>\n",
                "(program (assignment left: (identifier) right: (string (string_content))) (identifier))",
            ),
            (
                "interpolation inside ERB string",
                "<% x = \"v#{y}\" %>\n<p>q</p>\n<% bar %>\n",
                "(program (assignment left: (identifier) right: (string (string_content) (interpolation (identifier)))) (identifier))",
            ),
            (
                "plain comment",
                "# hello\nfoo\n",
                "(program (comment) (identifier))",
            ),
            (
                "plain trailing comment",
                "foo # hello\n",
                "(program (identifier) (comment))",
            ),
            (
                "comment at eof no newline",
                "foo # hello",
                "(program (identifier) (comment))",
            ),
            (
                "__END__ swallows comment",
                "word\n__END__\n# comment\n",
                "(program (identifier) (uninterpreted))",
            ),
            (
                "block comment",
                "=begin\nstuff\n=end\nfoo\n",
                "(program (comment) (identifier))",
            ),
        ];

        for (name, src, expected) in cases {
            let tree = parse_with(src, &ranges_simple(src));
            assert_eq!(tree.root_node().to_sexp(), expected, "case: {name}");
        }
    }

    #[test]
    fn erb_comment_byte_spans() {
        let cases: [(&str, &str, &[&str]); 12] = [
            (
                "if-tag with inline comment, block closed later",
                "<% if user.admin? # check %>\n  <p>a</p>\n<% end %>\n",
                &["# check "],
            ),
            (
                "each-do tag with inline comment",
                "<% items.each do |i| # loop %>\n  <p>x</p>\n<% end %>\n",
                &["# loop "],
            ),
            (
                "if / else / end across tags, comment in the if",
                "<% if a # why %>\n<p>x</p>\n<% else %>\n<p>y</p>\n<% end %>\n",
                &["# why "],
            ),
            (
                "inline: <% # c %>",
                "<% # c %>\n<p>x</p>\n<% foo %>\n",
                &["# c "],
            ),
            (
                "dedicated: <%# c %>",
                "<%# c %>\n<p>x</p>\n<% foo %>\n",
                &["# c "],
            ),
            (
                "no space before %>",
                "<% # c%>\n<p>x</p>\n<% foo %>\n",
                &["# c"],
            ),
            (
                "output tag <%= expr # c %>",
                "<%= name # label %>\n<p>x</p>\n<% foo %>\n",
                &["# label "],
            ),
            (
                "trim tag <%- ... -%>",
                "<%- x = 1 # set -%>\n<p>q</p>\n<% foo %>\n",
                &["# set "],
            ),
            (
                "percent sign inside comment",
                "<% # uses 100% of width %>\n<p>x</p>\n<% foo %>\n",
                &["# uses 100% of width "],
            ),
            (
                "two comment tags, spans must not merge",
                "<% # one %>\n<p>x</p>\n<% # two %>\n<p>y</p>\n<% z %>\n",
                &["# one ", "# two "],
            ),
            (
                "code after comment on next line in same tag",
                "<%\n  # c\n  foo\n%>\n<p>x</p>\n<% bar %>\n",
                &["# c"],
            ),
            (
                "no comment anywhere",
                "<% foo %>\n<p>x</p>\n<% bar %>\n",
                &[],
            ),
        ];

        for (name, src, want) in cases {
            let tree = parse_with(src, &ranges_erb(src));
            assert_eq!(comment_texts(src, &tree).as_slice(), want, "case: {name}");
        }

        let src = "<% if user.admin? # check %>\n  <p>a</p>\n<% end %>\n";
        let tree = parse_with(src, &ranges_erb(src));
        assert_eq!(
            tree.root_node().to_sexp(),
            "(program (if condition: (call receiver: (identifier) method: (identifier)) (comment)))",
            "if-tag sexp",
        );
    }
}
