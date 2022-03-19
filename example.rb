a = 1
  a
# ^ defined: 1

b = (a = 2)
  b
# ^ defined: 5
  a
# ^ defined: 1, 5

if a
#  ^ defined: 1, 5
  b
# ^ defined: 5
  c = 10
else
  a
# ^ defined: 1, 5
  c = 100
end

  c
# ^ defined: 15, 19
#
d = c
  d
# ^ defined: 25, 15, 19

