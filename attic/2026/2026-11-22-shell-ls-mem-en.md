# Fix the ls alias and make mem a command

## ls

`.bashrc` aliased `ls` and `grep` with `--color=auto`. The ported coreutils
rejected the option and printed several "invalid option" lines. Drop the
option from the aliases.

## mem

`/bin/mem` was the MM server; typing `mem` started it and it blocked on its
boot-info loop. Rename the server to `/bin/mm` (kconfig and the MM service
spawn follow) and ship `bin/mem`, a command that prints the memory totals.

## Verification

In the guest shell `ls` prints no "invalid option" line and `mem` prints the
totals.
