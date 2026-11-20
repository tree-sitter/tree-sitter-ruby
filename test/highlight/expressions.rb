foo +x
#   ^ operator
"a#{b}c"
#    ^ punctuation.special
$stdout
# <- variable
obj&.bar
#  ^ punctuation.delimiter
private def priv; end
# <- keyword
begin
rescue E => err
  err
  # <- variable
end
case v
in [Integer => i]
  i
  # <- variable
end
/a\d/
#  ^ string.regexp
3r
# <- number
undef bar
# <- keyword
