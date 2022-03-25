a = 1
b = 2

e = if a
  a
else
  b
end

f = if a
  a
end

  e
# ^ defined: 4, 1, 2

  f
# ^ defined: 10, 1
