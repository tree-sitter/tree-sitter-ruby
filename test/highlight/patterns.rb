expr = [false | __LINE__ | __FILE__ | __ENCODING__]
#       ^ constant.builtin
#               ^ constant.builtin
#                          ^ constant.builtin
#                                     ^ constant.builtin

case expr
  in {a: 5, b:, **nil}
#               ^ operator
#                 ^ constant.builtin
  in [false | __LINE__ | __FILE__ | __ENCODING__]
#     ^ constant.builtin
#             ^ constant.builtin
#                        ^ constant.builtin
#                                   ^ constant.builtin
  else
end

def bindings(v, w)
  case v
  in {name:}
  #   ^ variable
    name
    # <- variable
  in {"key":}
  #    ^ variable
    key
    # <- variable
  in [a, *b]
    [a, b]
    #^ variable
    #   ^ !function.method
  in [*, c, *d]
    [c, d]
    #^ variable
    #   ^ !function.method
  in {e: 1, **f}
    f
    # <- !function.method
  in [[g], {h: [i]}]
    [g, i]
    #^ variable
    #   ^ variable
  in (j)
    j
    # <- variable
  in _k | _l
    _k
    # <- variable
  in ^w
  #   ^ variable.parameter
    w
    # <- variable.parameter
  in {m:} if m
  #          ^ variable
  end
  v => n
  n
  # <- variable
  v in o
  o
  # <- variable
  v => {p:}
  p
  # <- variable
end

