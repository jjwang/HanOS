#!/usr/bin/env python3
# Print a greeting, run a loop and call a function.


def greet(name):
    return "Hello, %s!" % name


print(greet("HanOS"))
for i in range(1, 6):
    print("count %d" % i)
