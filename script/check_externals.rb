# frozen_string_literal: true

# The scanner's TokenType enum must list the grammar's externals in the same order: they are matched by position.
# usage: ruby script/check_externals.rb

require 'json'

ROOT = File.expand_path('..', __dir__)
# Scanner names that are not the grammar name in upper case
RENAMED = {
  'heredoc_end' => 'HEREDOC_BODY_END',
  'heredoc_beginning' => 'HEREDOC_START',
  '/' => 'FORWARD_SLASH',
  '_singleton_class_left_angle_left_langle' => 'SINGLETON_CLASS_LEFT_ANGLE_LEFT_ANGLE'
}.freeze

externals = JSON.parse(File.read("#{ROOT}/src/grammar.json"))['externals'].map { it['name'] || it['value'] }
expected = externals.map { RENAMED.fetch(it) { it.delete_prefix('_').upcase } }
tokens = File.read("#{ROOT}/src/scanner.c")[/typedef enum \{(.*?)NONE/m, 1].scan(/\b[A-Z][A-Z_]+\b/)
return if tokens == expected

first = expected.zip(tokens).index { |want, got| want != got }
abort "src/scanner.c TokenType differs from the grammar's externals at #{first}: " \
      "#{tokens[first].inspect} instead of #{expected[first].inspect}"
