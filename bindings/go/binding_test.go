package tree_sitter_ruby_test

import (
	"testing"

	tree_sitter "github.com/smacker/go-tree-sitter"
	"github.com/tree-sitter/tree-sitter-ruby"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_ruby.Language())
	if language == nil {
		t.Errorf("Error loading Ruby grammar")
	}
}
