# 2026-09-13：自旋锁 API 统一与加固

## 概述

`kernel/base/spinlock.{h,c}` 中的锁 API 与文件名不一致：类型叫 `lock_t`，调用点
使用 `lock_new()` / `lock_lock()` / `lock_release()` / `lock_try()` 宏，而实现函数
叫 `spin_lock_impl()` / `spin_unlock_impl()`。此外实现把保存的中断标志放在锁里，并
在**释放之后**才读回。

## 命名

现在 API 与 `spinlock` 文件一致，统一使用同一前缀：

| 旧 | 新 |
|----|----|
| `lock_t` | `spinlock_t` |
| `lock_new()` | `SPINLOCK_INIT` |
| `lock_lock(x)` | `spinlock_acquire(x)` |
| `lock_release(x)` | `spinlock_release(x)` |
| `lock_try(x)` | `spinlock_try_acquire(x)` |

删除了 `spin_lock_impl()` / `spin_unlock_impl()`。全部 27 个使用旧名的文件均已更新
（`spinlock_acquire` 65 处、`spinlock_release` 74 处、`spinlock_try_acquire` 2 处、
`SPINLOCK_INIT` 12 处）。

## 实现加固

- 用带 acquire-release 语义的 `__atomic_exchange_n` / `__atomic_load_n` /
  `__atomic_store_n` 替换 `__sync_bool_compare_and_swap`。
- **修复释放竞态**：旧解锁先清零 `locked`，再从锁里读保存的 RFLAGS。在这个窗口内
  获取锁的核心可能覆盖标志，从而破坏正在释放那个核心的中断状态。现在先把标志读入
  本地变量，再释放锁。
- 加入基于 `pause` 的自旋退避。
- 中断保存/恢复抽成 `spinlock_irq_save()` / `spinlock_irq_restore()`；
  `try_acquire` 失败时恢复原状态。

## 其他清理

- 移除 `kernel/device/storage/ata.c` 中未使用的 `ata_lock` 声明。
- 移除 `kernel/device/keyboard/keyboard.c` 中 `kb_lock` 上多余的 `volatile`
  （`locked` 字段本身已是 volatile）。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/base/spinlock.h` | 新的 `spinlock_t`、`SPINLOCK_INIT`、函数原型 |
| `kernel/base/spinlock.c` | 重写实现，修复释放竞态 |
| 27 个内核文件 | 所有调用点改名 |

## 测试

`make -C kernel clean && make -C kernel` 无告警；`-smp 2` 与 `-smp 4` 启动正常，
shell（`ls`、`pwd`）可用。
