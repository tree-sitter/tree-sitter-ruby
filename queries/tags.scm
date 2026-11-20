; Method definitions

(
  (comment)* @doc
  .
  [
    (method
      name: (_) @name) @definition.method
    (singleton_method
      name: (_) @name) @definition.method
  ]
  (#strip! @doc "^#\\s*")
  (#select-adjacent! @doc @definition.method)
)

(alias
  name: (_) @name) @definition.method

(setter
  (identifier) @ignore)

; Class definitions

(
  (comment)* @doc
  .
  [
    (class
      name: [
        (constant) @name
        (scope_resolution
          name: (_) @name)
      ]) @definition.class
    (singleton_class
      value: [
        (constant) @name
        (scope_resolution
          name: (_) @name)
      ]) @definition.class
  ]
  (#strip! @doc "^#\\s*")
  (#select-adjacent! @doc @definition.class)
)

; Module definitions

(
  (comment)* @doc
  .
  (module
    name: [
      (constant) @name
      (scope_resolution
        name: (_) @name)
    ]) @definition.module
  (#strip! @doc "^#\\s*")
  (#select-adjacent! @doc @definition.module)
)

; Constant definitions

(assignment
  left: [
    (constant) @name
    (scope_resolution
      name: (constant) @name)
  ]) @definition.constant

; Calls

(
  (call method: (identifier) @name) @reference.call
  (#not-match? @name "^(lambda|load|require|require_relative)$")
)

(
  (identifier) @name @reference.call
  (#is-not? local)
  (#not-match? @name "^(lambda|load|require|require_relative|__FILE__|__LINE__)$")
)

(call method: (constant) @name) @reference.call ; `Integer("1")`

(constant) @name @reference.class
