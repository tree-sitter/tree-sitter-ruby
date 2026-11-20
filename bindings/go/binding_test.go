package tree_sitter_ruby_test

import (
	"testing"

	tree_sitter "github.com/tree-sitter/go-tree-sitter"
	tree_sitter_ruby "github.com/Magikdidi24/tree-sitter-ruby/bindings/go"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_ruby.Language())
	if language == nil {
		t.Errorf("Error loading Ruby grammar")
	}
}

func TestParsesWithTheScanner(t *testing.T) {
	parser := tree_sitter.NewParser()
	defer parser.Close()
	if err := parser.SetLanguage(tree_sitter.NewLanguage(tree_sitter_ruby.Language())); err != nil {
		t.Fatal(err)
	}
	tree := parser.Parse([]byte("x = <<~A\n  hi\nA\n"), nil)
	defer tree.Close()
	root := tree.RootNode()
	if root.HasError() {
		t.Errorf("unexpected error in %s", root.ToSexp())
	}
	if kind := root.Child(0).ChildByFieldName("right").Kind(); kind != "heredoc_beginning" {
		t.Errorf("right is %s, not heredoc_beginning", kind)
	}
}
