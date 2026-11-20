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

require 'prism'
require 'tempfile'

ROOT = File.expand_path('..', __dir__)

# Prism node type => comparison group; `in` guards and heredocs are in prism_location
PRISM = {
  def_node: :def, class_node: :class, module_node: :module, singleton_class_node: :singleton_class,
  lambda_node: :lambda, block_node: :block, if_node: :if, unless_node: :unless,
  while_node: :while, until_node: :until, for_node: :for, case_node: :case, case_match_node: :case,
  begin_node: :begin, rescue_node: :rescue, rescue_modifier_node: :rescue_modifier, ensure_node: :ensure,
  alias_method_node: :alias, alias_global_variable_node: :alias,
  range_node: :range, flip_flop_node: :range, # a range in a condition; tree-sitter has no separate node
  return_node: :return, yield_node: :yield, break_node: :break, next_node: :next,
  match_predicate_node: :test_pattern, match_required_node: :match_pattern,
  and_node: :binary, or_node: :binary,
  regular_expression_node: :regex, interpolated_regular_expression_node: :regex,
  match_last_line_node: :regex, interpolated_match_last_line_node: :regex # a regex as a condition, matched against $_
}.freeze

TREE_SITTER = {
  'method' => :def, 'singleton_method' => :def,
  'class' => :class, 'module' => :module, 'singleton_class' => :singleton_class,
  'lambda' => :lambda, 'block' => :block, 'do_block' => :block,
  'if' => :if, 'if_modifier' => :if, 'elsif' => :if, 'conditional' => :if,
  'unless' => :unless, 'unless_modifier' => :unless,
  'while' => :while, 'while_modifier' => :while, 'until' => :until, 'until_modifier' => :until,
  'for' => :for, 'case' => :case, 'case_match' => :case,
  'rescue' => :rescue, 'ensure' => :ensure, 'rescue_modifier' => :rescue_modifier,
  'alias' => :alias, 'range' => :range,
  'return' => :return, 'yield' => :yield, 'break' => :break, 'next' => :next,
  'test_pattern' => :test_pattern, 'match_pattern' => :match_pattern,
  'regex' => :regex, 'heredoc_beginning' => :heredoc, 'begin' => :begin,
  'if_guard' => :if, 'unless_guard' => :unless, 'binary' => :binary
}.freeze

BINARY_OPERATORS = %i[** * / % + - << >> & | ^ < <= > >= == != === =~ !~ <=>].freeze
# tree-sitter starts these at the keyword; implicit `def ... rescue` bodies have no `begin`
KEYWORD_LOC = { begin_node: :begin_keyword_loc, ensure_node: :ensure_keyword_loc }.freeze
HEREDOC_NODES = %i[string_node interpolated_string_node x_string_node interpolated_x_string_node].freeze
NODE = /^ *(\w+: )?\((\w+) \[(\d+), (\d+)\]/ # the CLI prints one node per line
ERROR = /\((ERROR|MISSING)[^\[]*\[(\d+), (\d+)\]/

def prism_positions(root)
  found = Hash.new(0)
  stack = [[root, nil]]
  while (node, parent = stack.pop)
    stack.concat(node.compact_child_nodes.map { [it, node] })
    group, loc = prism_location(node, parent)
    found[[group, loc.start_line - 1, loc.start_column]] += 1 if loc
  end
  found
end

def prism_location(node, parent)
  case node
  # `in pat if cond`: tree-sitter's guard starts at the keyword, Prism's IfNode at the pattern
  in Prism::IfNode if parent in Prism::InNode then [:if, node.if_keyword_loc]
  in Prism::UnlessNode if parent in Prism::InNode then [:unless, node.keyword_loc]
  in Prism::CallNode if binary?(node) then [:binary, node.location]
  in _ if heredoc?(node) then [:heredoc, node.opening_loc]
  in _ if (group = PRISM[node.type]) then [group, node.public_send(KEYWORD_LOC.fetch(node.type, :location))]
  else nil
  end
end

# `a + b`, not `a.+(b)` nor `a::+(b)`
def binary?(node) = node.receiver && !node.call_operator_loc && BINARY_OPERATORS.include?(node.name)

def heredoc?(node) = HEREDOC_NODES.include?(node.type) && node.opening_loc&.slice&.start_with?('<<')

def tree_sitter_positions(tree)
  found = Hash.new(0)
  tree.scan(NODE) do |field, kind, row, column|
    group = TREE_SITTER[kind]
    next if group.nil? || (group == :block && field == 'body: ') # a lambda body, part of Prism's LambdaNode

    found[[group, row.to_i, column.to_i]] += 1
  end
  found
end

# tree-sitter's trees as it prints them, so it parses while Prism works
def tree_sitter_trees(files, &)
  Tempfile.create('prism-diff') do |list|
    list.puts(files)
    list.flush
    cli = [ENV.fetch('TREE_SITTER', 'tree-sitter'), 'parse', '-p', ROOT, '--paths', list.path]
    IO.popen(cli, err: File::NULL) do |out|
      # each tree starts with its root at column 0: (program ...) or, when nothing parses, (ERROR ...)
      out.each_line.slice_before { it.start_with?('(') }.lazy.map(&:join).drop_while { !it.start_with?('(') }.each(&)
    end
  end
end

def each_tree(files)
  count = 0
  tree_sitter_trees(files) do |tree|
    yield files[count], tree if count < files.size
    count += 1
  end
  abort "tree-sitter printed #{count} trees for #{files.size} files" unless count == files.size
end

verbose = ARGV.delete('--verbose')
known = []
if (i = ARGV.index('--known'))
  known = File.readlines(ARGV.delete_at(i + 1), chomp: true).map { it.sub(/#.*/, '').strip }.reject(&:empty?)
  ARGV.delete_at(i)
end
known.map! { "/#{it}" }
files = ARGV.flat_map { |path| File.directory?(path) ? Dir.glob("#{path}/**/*.rb") : [path] }
abort 'usage: ruby script/prism_diff.rb [--verbose] [--known LIST] FILE_OR_DIR...' if files.empty?

counts = Hash.new(0)
kinds = Hash.new(0)
fixed = []
each_tree(files) do |file, tree|
  result = Prism.parse_file(file)
  next counts[:prism_errors] += 1 if result.failure?

  error = tree.match(ERROR)
  expected = prism_positions(result.value)
  actual = tree_sitter_positions(tree)
  diffs = (expected.keys | actual.keys).reject { expected[it] == actual[it] }
  listed = file.end_with?(*known)
  if !error && diffs.empty?
    fixed << file if listed
    next counts[:clean] += 1
  end
  next counts[:known] += 1 if listed

  counts[:differ] += 1
  diffs.each { kinds[it.first] += (expected[it] - actual[it]).abs }
  kinds[:tree_sitter_errors] += 1 if error
  problems = diffs.map do |key|
    group, row, column = key
    "#{group} at #{row + 1}:#{column}: Prism #{expected[key]}, tree-sitter #{actual[key]}"
  end
  problems.unshift("tree-sitter #{error[1]} at #{error[2].to_i + 1}:#{error[3]}") if error
  puts "#{file}\n  #{(verbose ? problems : problems.first(3)).join("\n  ")}"
end

puts "\n#{files.size} files: #{counts[:clean]} agree, #{counts[:differ]} differ, " \
     "#{counts[:known]} known differences, #{counts[:prism_errors]} rejected by Prism (skipped)"
puts "differences by kind: #{kinds.sort_by { -_2 }.to_h}" unless kinds.empty?
puts "now agree, remove from the known list:\n  #{fixed.join("\n  ")}" unless fixed.empty?
exit(counts[:differ].zero? && fixed.empty?)
