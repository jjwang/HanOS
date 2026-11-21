#!/usr/bin/env python3
# Fibonacci numbers with a memo table.

memo = {0: 0, 1: 1}


def fib(n):
    if n not in memo:
        memo[n] = fib(n - 1) + fib(n - 2)
    return memo[n]


for n in range(15):
    print("%2d %d" % (n, fib(n)))
