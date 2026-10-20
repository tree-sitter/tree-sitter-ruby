# frozen_string_literal: true

# Differential test against Prism, Ruby's own parser.
#
# For each file it reports (1) tree-sitter ERROR/MISSING nodes in code Prism accepts, and (2) constructs
# whose start position differs: the node vocabularies differ, so only kinds with a clear 1:1 mapping
# are compared, by (line, byte column).
#
# usage: ruby script/prism_diff.rb [--verbose] [--known LIST] FILE_OR_DIR...
#   TREE_SITTER=<cli> (default: tree-sitter) is run as `<cli> parse -p <repo root> --paths <list>`.
#   LIST holds path suffixes (one per line, `#` comments) of files expected to differ.
#   Exits 1 on a difference outside LIST, or when a listed file agrees again (so the list only shrinks).

require "prism"
require "tempfile"

ROOT = File.expand_path("..", __dir__)

# Prism node type => [comparison group, location method (default: location)]
PRISM = {
  def_node: [:def],
  class_node: [:class],
  module_node: [:module],
  singleton_class_node: [:singleton_class],
  lambda_node: [:lambda],
  block_node: [:block],
  if_node: [:if],
  unless_node: [:unless],
  while_node: [:while],
  until_node: [:until],
  for_node: [:for],
  case_node: [:case],
  case_match_node: [:case],
  rescue_node: [:rescue],
  ensure_node: [:ensure, :ensure_keyword_loc],
  rescue_modifier_node: [:rescue_modifier],
  alias_method_node: [:alias],
  alias_global_variable_node: [:alias],
  range_node: [:range],
  flip_flop_node: [:range], # a range in a condition; tree-sitter has no separate node
  return_node: [:return],
  yield_node: [:yield],
  break_node: [:break],
  next_node: [:next],
  match_predicate_node: [:test_pattern],
  match_required_node: [:match_pattern],
  regular_expression_node: [:regex],
  interpolated_regular_expression_node: [:regex],
}.freeze

# tree-sitter node type => comparison group
TREE_SITTER = {
  "method" => :def, "singleton_method" => :def,
  "class" => :class, "module" => :module, "singleton_class" => :singleton_class,
  "lambda" => :lambda, "block" => :block, "do_block" => :block,
  "if" => :if, "if_modifier" => :if, "elsif" => :if, "conditional" => :if,
  "unless" => :unless, "unless_modifier" => :unless,
  "while" => :while, "while_modifier" => :while, "until" => :until, "until_modifier" => :until,
  "for" => :for, "case" => :case, "case_match" => :case,
  "rescue" => :rescue, "ensure" => :ensure, "rescue_modifier" => :rescue_modifier,
  "alias" => :alias, "range" => :range,
  "return" => :return, "yield" => :yield, "break" => :break, "next" => :next,
  "test_pattern" => :test_pattern, "match_pattern" => :match_pattern,
  "regex" => :regex, "heredoc_beginning" => :heredoc, "begin" => :begin,
  "if_guard" => :if, "unless_guard" => :unless,
}.freeze

HEREDOC_NODES = %i[string_node interpolated_string_node x_string_node interpolated_x_string_node].freeze

def prism_positions(result)
  found = Hash.new(0)
  stack = [[result.value, nil]]
  until stack.empty?
    node, parent = stack.pop
    stack.concat(node.compact_child_nodes.map { [_1, node] })
    group, loc_method = PRISM[node.type]
    loc = loc_method ? node.public_send(loc_method) : node.location
    if parent&.type == :in_node && %i[if_node unless_node].include?(node.type)
      # `in pat if cond`: tree-sitter's guard starts at the keyword, Prism's IfNode at the pattern
      loc = node.type == :if_node ? node.if_keyword_loc : node.keyword_loc
    elsif node.type == :begin_node
      group, loc = :begin, node.begin_keyword_loc # implicit `def ... rescue` bodies have none
    elsif HEREDOC_NODES.include?(node.type) && node.opening_loc&.slice&.start_with?("<<")
      group, loc = :heredoc, node.opening_loc
    end
    found[[group, loc.start_line - 1, loc.start_column]] += 1 if group && loc
  end
  found
end

def tree_sitter_trees(files)
  list = Tempfile.new("prism-diff")
  list.write(files.join("\n") << "\n")
  list.close
  out = IO.popen([ENV.fetch("TREE_SITTER", "tree-sitter"), "parse", "-p", ROOT, "--paths", list.path], err: File::NULL, &:read)
  # each tree starts with its root at column 0: (program ...) or, when nothing parses, (ERROR ...)
  trees = out.each_line.slice_before { |line| line.start_with?("(") }.map(&:join)
  trees = trees.drop_while { |tree| !tree.start_with?("(") }
  abort "tree-sitter printed #{trees.size} trees for #{files.size} files" unless trees.size == files.size
  trees
end

def tree_sitter_positions(tree)
  found = Hash.new(0)
  tree.scan(/(\w+: )?\((\w+) \[(\d+), (\d+)\]/) do |field, kind, row, column|
    next if field == "body: " && %w[block do_block].include?(kind) # a lambda body, part of Prism's LambdaNode

    group = TREE_SITTER[kind]
    found[[group, row.to_i, column.to_i]] += 1 if group
  end
  found
end

verbose = ARGV.delete("--verbose")
known = []
if (i = ARGV.index("--known"))
  known = File.readlines(ARGV.delete_at(i + 1), chomp: true).map { _1.sub(/#.*/, "").strip }.reject(&:empty?)
  ARGV.delete_at(i)
end
known_file = ->(file) { known.any? { file.end_with?("/#{_1}") } }
files = ARGV.flat_map { |path| File.directory?(path) ? Dir.glob("#{path}/**/*.rb").sort : [path] }
abort "usage: ruby script/prism_diff.rb [--verbose] [--known LIST] FILE_OR_DIR..." if files.empty?

counts = Hash.new(0)
failures = 0
fixed = []
files.zip(tree_sitter_trees(files)).each do |file, tree|
  result = Prism.parse_file(file)
  next counts[:prism_errors] += 1 unless result.errors.empty? # invalid Ruby: nothing to compare against

  problems = []
  error = tree[/\((ERROR|MISSING)[^\[]*\[(\d+), (\d+)\]/]
  problems << "tree-sitter #{$1} at #{$2.to_i + 1}:#{$3}" if error
  expected, actual = prism_positions(result), tree_sitter_positions(tree)
  kinds = Hash.new(0)
  (expected.keys | actual.keys).each do |key|
    diff = expected[key] - actual[key]
    next if diff.zero?

    group, row, column = key
    kinds[group] += diff.abs
    problems << "#{group} at #{row + 1}:#{column}: Prism #{expected[key]}, tree-sitter #{actual[key]}"
  end
  if problems.empty?
    fixed << file if known_file[file]
    next counts[:clean] += 1
  end
  next counts[:known] += 1 if known_file[file]

  failures += 1
  counts.merge!(kinds) { |_, a, b| a + b }
  counts[:tree_sitter_errors] += 1 if error
  puts "#{file}\n  #{(verbose ? problems : problems.first(3)).join("\n  ")}"
end

puts "\n#{files.size} files: #{counts[:clean]} agree, #{failures} differ, #{counts[:known]} known differences, " \
     "#{counts[:prism_errors]} rejected by Prism (skipped)"
puts "differences by kind: #{counts.except(:clean, :known, :prism_errors).sort_by { -_2 }.to_h}" if failures.positive?
puts "now agree, remove from the known list:\n  #{fixed.join("\n  ")}" unless fixed.empty?
exit(failures.zero? && fixed.empty? ? 0 : 1)
