((regex
  (string_content) @injection.content)
 (#set! injection.language "regex"))

((subshell
  (string_content) @injection.content)
 (#set! injection.language "bash"))

; Heredocs whose label names a language, e.g. `<<~SQL`
((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @_label)
 (#eq? @_label "SQL")
 (#set! injection.language "sql"))

((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @_label)
 (#any-of? @_label "GRAPHQL" "GQL")
 (#set! injection.language "graphql"))

((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @_label)
 (#eq? @_label "HTML")
 (#set! injection.language "html"))

((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @_label)
 (#any-of? @_label "SH" "BASH" "SHELL")
 (#set! injection.language "bash"))

((heredoc_body
  (heredoc_content) @injection.content
  (heredoc_end) @_label)
 (#eq? @_label "RUBY")
 (#set! injection.language "ruby"))
