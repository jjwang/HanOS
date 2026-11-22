# Move servers to /sbin

The ring-3 servers were staged in `/bin`, so a shell command of the same name
(before `mm` and `mem`) started the server and blocked on its service loop.
Stage the servers under `/sbin` and keep the commands in `/bin`; `/sbin` is not
on the shell's PATH. `kernel/kconfig.h` `DEFAULT_*_SVR`, the userspace Makefile
and the initrd tar now use `/sbin`.
