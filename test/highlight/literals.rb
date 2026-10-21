"hello"
# ^ string
%(hello)
# ^ string
%w(hello goodbye)
#   ^ string
#         ^ string

:hello
# ^ string.special.symbol
%s(hello)
# ^ string.special.symbol
%I(hello goodbye)
#  ^ string.special.symbol
#         ^ string.special.symbol

/hello/
# ^ string.regexp
%r(hello)
# ^ string.regexp

12345
# ^ number
12.345
# ^ number

nil
# ^ constant.builtin
true
# ^ constant.builtin
TRUE
# ^ constant
false
# ^ constant.builtin
FALSE
# ^ constant
