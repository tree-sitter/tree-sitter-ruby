a = 1
b = 2

if a
#  ^ defined: 1
  b
# ^ defined: 2
  c = 10
else
  a
# ^ defined: 1
  c = 100
end

  c
# ^ defined: 8, 12
