# Precompile the Python standard library

`3rdparty/ports/packages/python.sh` now runs `python -m compileall -b` over the
staged stdlib. Imports load the adjacent `.pyc` bytecode instead of parsing
`.py` on each start. The change takes effect on the next `make hdd-ports`.
