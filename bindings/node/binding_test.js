const assert = require("node:assert");
const { test } = require("node:test");

const Parser = require("tree-sitter");

test("can load grammar", () => {
  const parser = new Parser();
  assert.doesNotThrow(() => parser.setLanguage(require(".")));
});

test("parses with the scanner", () => {
  const parser = new Parser();
  parser.setLanguage(require("."));
  const tree = parser.parse("x = <<~A\n  hi\nA\n");
  assert.equal(tree.rootNode.hasError, false);
  assert.equal(tree.rootNode.child(0).childForFieldName("right").type, "heredoc_beginning");
});
