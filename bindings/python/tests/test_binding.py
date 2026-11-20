from unittest import TestCase

import tree_sitter, tree_sitter_ruby


class TestLanguage(TestCase):
    def test_can_load_grammar(self):
        try:
            tree_sitter.Language(tree_sitter_ruby.language())
        except Exception:
            self.fail("Error loading Ruby grammar")

    def test_parses_with_the_scanner(self):
        parser = tree_sitter.Parser(tree_sitter.Language(tree_sitter_ruby.language()))
        tree = parser.parse(b"x = <<~A\n  hi\nA\n")
        self.assertFalse(tree.root_node.has_error)
        self.assertEqual(tree.root_node.child(0).child_by_field_name("right").type, "heredoc_beginning")
