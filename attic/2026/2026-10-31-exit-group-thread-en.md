# 2026-10-31: exit_group from a non-leader thread

## Symptom

A process whose non-leader thread calls `exit()` hangs its parent in `wait`, or the parent gets status 0 instead of the status the thread passed.

## Root cause

`k_exit_group` recorded the process exit only when the caller was the group leader (`!t->is_thread`). A non-leader caller killed the leader through `sched_kill_group` but never stored the status. The reaper then recorded the leader with its default status.

## Changes

- proc: record the group exit on the leader pid from any caller. `exit_group_record` closes the group fd table, notifies `PROC_EXIT` for the leader, and marks the leader recorded so the reaper skips it.
- test: add `exitgrouptest`. A child forks, a spawned thread calls `exit(42)`, and the parent checks `WEXITSTATUS`.

## Verification

- GCC 13.1 and cross GCC 10.2, TCG, `-smp 1`: `exitgrouptest PASS status=42`.
