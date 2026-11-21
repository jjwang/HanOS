#!/usr/bin/env python3
# Report the interpreter and the runtime environment.

import os
import sys

print("version:  %s" % sys.version.split()[0])
print("platform: %s" % sys.platform)
print("os:       %s" % os.name)
print("cwd:      %s" % os.getcwd())
print("maxint:   %d" % sys.maxsize)
print("argv:     %s" % sys.argv)
