# frozen_string_literal: true

# Writes the input of each test in test/corpus to its own file, for script/coverage.sh and script/edit_fuzz.c.
# usage: ruby script/corpus_inputs.rb DIR

require 'fileutils'

dir = ARGV.fetch(0)
FileUtils.mkdir_p(dir)
Dir['test/corpus/*.txt'].each do |path|
  File.binread(path).split(/^={3,}[^\n]*\n[^\n]*\n={3,}[^\n]*\n/).drop(1).each_with_index do |test, i|
    # the longest line of dashes separates the input from the expected tree, as in `tree-sitter test`
    divider = test.scan(/^-{3,}(?=\r?\n)/).max_by(&:size)
    File.binwrite(File.join(dir, "#{File.basename(path, '.txt')}_#{i}.rb"), test[0...test.index(/^#{divider}\r?\n/)])
  end
end
