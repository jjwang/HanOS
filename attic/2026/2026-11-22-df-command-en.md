# Add a df command

`bin/df` calls `statfs("/data")` and prints the ext2 partition's total, used
and available KiB. The statfs syscall and ext2 statfs path already existed;
only the command and the `CELF` entry are new.
