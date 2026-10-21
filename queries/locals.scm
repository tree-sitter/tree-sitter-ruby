; Scopes

; `def`, `class`, `module` and `class << x` bodies do not see outer locals
([
  (method)
  (singleton_method)
  (class)
  (module)
  (singleton_class)
] @local.scope
 (#set! local.scope-inherits false))

[
  (lambda)
  (block)
  (do_block)
] @local.scope

; Definitions

(block_parameter (identifier) @local.definition)
(block_parameters (identifier) @local.definition)
(destructured_parameter (identifier) @local.definition)
(hash_splat_parameter (identifier) @local.definition)
(lambda_parameters (identifier) @local.definition)
(method_parameters (identifier) @local.definition)
(splat_parameter (identifier) @local.definition)

(keyword_parameter name: (identifier) @local.definition)
(optional_parameter name: (identifier) @local.definition)

(exception_variable (identifier) @local.definition)
(for pattern: (identifier) @local.definition)

; Pattern matching bindings: `in x`, `in [x, *y]`, `in {k: x}`, `in T => x`
(in_clause pattern: (identifier) @local.definition)
(match_pattern pattern: (identifier) @local.definition)
(test_pattern pattern: (identifier) @local.definition)
(array_pattern (identifier) @local.definition)
(find_pattern (identifier) @local.definition)
(keyword_pattern value: (identifier) @local.definition)
(as_pattern name: (identifier) @local.definition)

; References

(identifier) @local.reference

(assignment left: (identifier) @local.definition)
(operator_assignment left: (identifier) @local.definition)
(left_assignment_list (identifier) @local.definition)
(rest_assignment (identifier) @local.definition)
(destructured_left_assignment (identifier) @local.definition)
