# frozen_string_literal: true

# Writes the inputs that once crashed, hung or slowed the scanner, for script/sanitize.c.
# usage: ruby script/adversarial.rb DIR

require 'fileutils'

dir = ARGV.fetch(0)
FileUtils.mkdir_p(dir)
write = ->(name, text) { File.binwrite(File.join(dir, "#{name}.rb"), text) }

# Pending heredocs and long heredoc words, past the 1024-byte state
write['heredocs', "x = [#{'<<A, ' * 500}]\n#{"a\nA\n" * 500}"]
words = (1..50).map { |i| "#{'W' * 200}#{i}" }
write['heredoc_words', "#{words.map { |w| "<<#{w}" }.join(', ')}\n#{words.map { |w| "a\n#{w}\n" }.join}"]
# Deep nesting of literals, interpolations, blocks and parentheses
write['percent', "#{'%(' * 10_000}#{')' * 10_000}"]
write['interpolation', "#{'"#{' * 10_000}1#{'}"' * 10_000}"]
write['symbol_regex', "#{':"#{' * 5000}1#{'}"' * 5000}\n#{'/#{' * 5000}1#{'}/' * 5000}"]
write['blocks', "#{'f { ' * 3000}#{'}' * 3000}\n#{'-> { ' * 3000}#{'}' * 3000}\n#{'f do ' * 3000}#{'end ' * 3000}"]
write['parens', "#{'(' * 100_000}#{')' * 100_000}"]
# Many parameters and locals, and the comma lists and long lines that were quadratic
write['params', "def f(#{(1..3000).map { |i| "a#{i}" }.join(', ')}); a1 +1; end"]
write['locals', (1..5000).map { |i| "v#{i} = 1; v#{i} +1" }.join("\n")]
write['masgn', "x = #{'a, ' * 20_000}1\n#{'a, ' * 20_000}b = 1\n"]
write['long_line', "x = 1;#{'x +1;' * 40_000}"]
# Bytes that are not Ruby
write['random', Random.new(1).bytes(200_000)]
write['nul', "x = 1\0y = 2\0" * 100]

# Every prefix of constructs that keep scanner state, cut anywhere
SEEDS = <<~'RUBY'.split("\n---\n")
  x = <<~A
    #{y}
  A
  ---
  %w[a #{b}]
  ---
  "a#{"b#{c}"}"
  ---
  :"a#{b}"
  ---
  /a#{b}/x
  ---
  def f(a, b = a +1) = a
  ---
  f { |a; b| a +1 }
  ---
  x = 1; def g; x +1; end
  ---
  case x
  in [a, *b] then a
  end
  ---
  a, (b, *c) = 1, 2
  ---
  for i in a do i +1 end
  ---
  begin
  rescue => e
    e +1
  end
  ---
  class A < B; def self(t) = t; end
  ---
  { defined?: 1, k?: 2 }
  ---
  x = :$'; return[1]
RUBY
SEEDS.each_with_index do |seed, i|
  (0...seed.bytesize).each { |n| write["cut_#{i}_#{n}", seed.byteslice(0, n)] }
end
