((*
  (comment)+ @doc
  (method
    name: (identifier) @name) @method)
 (strip! @doc "^#\\s*")
 (select-adjacent! @doc @method))
(method
  name: (identifier) @name) @method

((*
  (comment)+ @doc
  (singleton_method
    name: (identifier) @name) @method)
 (strip! @doc "^#\\s*")
 (select-adjacent! @doc @method))
(singleton_method
  name: (identifier) @name) @method

((*
  (comment)+ @doc
  (class
    name: (constant) @name) @class)
 (strip! @doc "^#\\s*")
 (select-adjacent! @doc @class))
(class
  name: (constant) @name) @class

(method_call
  method: (identifier) @name) @call
(method_call
  method: (call method: (identifier) @name)) @call
(call
  method: (identifier) @name) @call

((identifier) @name @call
 (is-not? local))
