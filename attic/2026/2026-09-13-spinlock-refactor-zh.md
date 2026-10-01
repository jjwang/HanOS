# 2026-09-13：自旋锁 API 统一与加固

## 概述

- `kernel/base/spinlock.{h,c}` 的锁 API 与文件名不一致。
- 类型名为 `lock_t`。
- 调用点使用 `lock_new()`、`lock_lock()`、`lock_release()`、`lock_try()` 宏。
- 实现函数名为 `spin_lock_impl()`、`spin_unlock_impl()`。
- 实现把保存的中断标志存入锁，释放后才读回。

## 命名

现在 API 与 `spinlock` 文件一致，统一前缀：

| 旧 | 新 |
|----|----|
| `lock_t` | `spinlock_t` |
| `lock_new()` | `SPINLOCK_INIT` |
| `lock_lock(x)` | `spinlock_acquire(x)` |
| `lock_release(x)` | `spinlock_release(x)` |
| `lock_try(x)` | `spinlock_try_acquire(x)` |

- 删除 `spin_lock_impl()` 与 `spin_unlock_impl()`。
- 更新所有 27 个使用旧名的文件。
- `spinlock_acquire` 65 处，`spinlock_release` 74 处。
- `spinlock_try_acquire` 2 处，`SPINLOCK_INIT` 12 处。

## 实现加固

- 用 `__atomic_exchange_n`、`__atomic_load_n`、`__atomic_store_n` 替换 `__sync_bool_compare_and_swap`。
- 新原子操作采用 acquire-release 语义。
- **修复释放竞态**：
  - 旧解锁先清零 `locked`，再从锁读取保存的 RFLAGS。
  - 该窗口内获取锁的核心可能覆盖标志。
  - 正在释放的核心读到错误标志。
  - 它恢复的中断状态错误。
  - 现在先把标志读入本地变量，再释放锁。
- 加入基于 `pause` 的自旋退避。
- 中断保存/恢复抽成 `spinlock_irq_save()` 与 `spinlock_irq_restore()`。
- `try_acquire` 失败时恢复原状态。

## 其他清理

- 移除 `kernel/device/storage/ata.c` 中未使用的 `ata_lock` 声明。
- 移除 `kernel/device/keyboard/keyboard.c` 中 `kb_lock` 多余的 `volatile`。
- `locked` 字段本身已是 volatile。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/base/spinlock.h` | 新的 `spinlock_t`、`SPINLOCK_INIT`、函数原型 |
| `kernel/base/spinlock.c` | 重写实现，修复释放竞态 |
| 27 个内核文件 | 所有调用点改名 |

## 测试

- `make -C kernel clean && make -C kernel` 无告警。
- `-smp 2` 与 `-smp 4` 启动正常。
- shell（`ls`、`pwd`）可用。
