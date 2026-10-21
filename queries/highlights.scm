(identifier) @variable

((identifier) @function.method
 (#is-not? local))

[
  "BEGIN"
  "END"
  "alias"
  "and"
  "begin"
  "break"
  "case"
  "class"
  "def"
  "do"
  "else"
  "elsif"
  "end"
  "ensure"
  "for"
  "if"
  "in"
  "module"
  "next"
  "not"
  "or"
  "rescue"
  "redo"
  "retry"
  "return"
  "then"
  "undef"
  "unless"
  "until"
  "when"
  "while"
  "yield"
] @keyword

(constant) @constructor

; Function calls

"defined?" @function.method.builtin

(call
  method: [(identifier) (constant)] @function.method)

((identifier) @function.method.builtin
 (#any-of? @function.method.builtin
   "require" "require_relative" "load" "autoload"
   "attr_reader" "attr_writer" "attr_accessor"
   "include" "extend" "prepend" "raise" "fail"))

((identifier) @keyword
 (#match? @keyword "^(private|protected|public)$"))

; Function definitions

(alias (identifier) @function.method)
(setter (identifier) @function.method)
(method name: [(identifier) (constant)] @function.method)
(singleton_method name: [(identifier) (constant)] @function.method)

; Identifiers

[
  (class_variable)
  (instance_variable)
] @property

(global_variable) @variable

((identifier) @constant.builtin
 (#match? @constant.builtin "^__(FILE|LINE|ENCODING)__$"))

(file) @constant.builtin
(line) @constant.builtin
(encoding) @constant.builtin

(hash_splat_nil
  "**" @operator) @constant.builtin

((constant) @constant
 (#match? @constant "^[A-Z\\d_]+$"))

[
  (self)
  (super)
] @variable.builtin

(block_parameter (identifier) @variable.parameter)
(block_parameters (identifier) @variable.parameter)
(destructured_parameter (identifier) @variable.parameter)
(hash_splat_parameter (identifier) @variable.parameter)
(lambda_parameters (identifier) @variable.parameter)
(method_parameters (identifier) @variable.parameter)
(splat_parameter (identifier) @variable.parameter)

(keyword_parameter name: (identifier) @variable.parameter)
(optional_parameter name: (identifier) @variable.parameter)

; Literals

[
  (string)
  (bare_string)
  (subshell)
  (heredoc_body)
  (heredoc_beginning)
] @string

[
  (simple_symbol)
  (delimited_symbol)
  (hash_key_symbol)
  (bare_symbol)
] @string.special.symbol

(regex) @string.regexp
(escape_sequence) @escape

[
  (integer)
  (float)
  (rational)
  (complex)
] @number

(character) @string

(uninterpreted) @comment

[
  (nil)
  (true)
  (false)
] @constant.builtin

(comment) @comment

; Operators

[
  "=" "=>" "->"
  "!" "!=" "!~" "%" "%=" "&" "&&" "&&=" "&=" "*" "**" "**=" "*="
  "+" "+=" "-" "-=" "/=" "<" "<<" "<<=" "<=" "<=>" "==" "==="
  "=~" ">" ">=" ">>" ">>=" "^" "^=" "|" "|=" "||" "||=" "~" ".." "..."
] @operator

(binary "/" @operator) ; "/" also delimits regex literals

(conditional ["?" ":"] @operator)

[
  ","
  ";"
  "."
  "&."
  "::"
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
  "%w("
  "%i("
] @punctuation.bracket

(block_parameters "|" @punctuation.bracket)

(interpolation
  "#{" @punctuation.special
  "}" @punctuation.special) @embedded
