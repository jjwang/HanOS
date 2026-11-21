#!/usr/bin/env python3
# Count words in a text file.

path = "churchill.txt"
with open(path, "r") as f:
    text = f.read()

words = text.split()
print("%s: %d lines, %d words" % (path, text.count("\n"), len(words)))
print("first:", " ".join(words[:8]))
