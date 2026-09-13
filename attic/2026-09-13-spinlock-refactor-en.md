# 2026-09-13: Spinlock API Unification and Hardening

## Overview

The lock API in `kernel/base/spinlock.{h,c}` did not match the file name: the
type was `lock_t` and call sites used the `lock_new()` / `lock_lock()` /
`lock_release()` / `lock_try()` macros, while the implementation functions were
`spin_lock_impl()` / `spin_unlock_impl()`. The implementation also stored the
saved interrupt flags in the lock and read them back *after* releasing it.

## Naming

The API now matches the `spinlock` file and uses one prefix everywhere:

| Before | After |
|--------|-------|
| `lock_t` | `spinlock_t` |
| `lock_new()` | `SPINLOCK_INIT` |
| `lock_lock(x)` | `spinlock_acquire(x)` |
| `lock_release(x)` | `spinlock_release(x)` |
| `lock_try(x)` | `spinlock_try_acquire(x)` |

`spin_lock_impl()` / `spin_unlock_impl()` were removed. All 27 files that used
the old names were updated (`spinlock_acquire` 65 sites, `spinlock_release` 74
sites, `spinlock_try_acquire` 2 sites, `SPINLOCK_INIT` 12 sites).

## Implementation hardening

- Use `__atomic_exchange_n` / `__atomic_load_n` / `__atomic_store_n` with
  acquire-release semantics instead of `__sync_bool_compare_and_swap`.
- **Fix a release race:** the old unlock cleared `locked` and then read the
  saved RFLAGS from the lock. A core that acquired the lock in that window
  could overwrite the flags before the releasing core read them, corrupting
  its interrupt state. The flags are now read into a local *before* the lock is
  released.
- Add a `pause`-based spin backoff.
- Factor the interrupt save/restore into `spinlock_irq_save()` /
  `spinlock_irq_restore()`; `try_acquire` restores the previous state on
  failure.

## Minor cleanups

- Removed the unused `ata_lock` declaration in `kernel/device/storage/ata.c`.
- Removed a redundant `volatile` on `kb_lock` in `kernel/device/keyboard/keyboard.c`
  (the `locked` field is already volatile).

## Files Changed

| File | Change |
|------|--------|
| `kernel/base/spinlock.h` | new `spinlock_t`, `SPINLOCK_INIT`, function prototypes |
| `kernel/base/spinlock.c` | rewritten implementation, release race fixed |
| 27 kernel files | API rename at all call sites |

## Testing

`make -C kernel clean && make -C kernel` is warning-free; boot on `-smp 2` and
`-smp 4` is clean and the shell (`ls`, `pwd`) still works.
