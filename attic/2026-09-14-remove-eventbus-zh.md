# 2026-09-14：移除旧的事件总线

## 概述

事件总线已经退化为通知原语之上的一层薄封装，因此将其移除，调用方直接使用通知。这样
去掉了一层封装及其空操作的分发钩子。

## 改动

- 删除 `kernel/proc/eventbus.{c,h}` 以及 `eb_publish` / `eb_subscribe` /
  `eb_dispatch` / `eb_init` API。
- `kernel/proc/notify.{h,c}` 新增共享通知对象（`notify_system`）与
  `notify_system_init()`，在分配器就绪后初始化一次。
- 调用点改为使用通知：
  - `kernel/device/keyboard/keyboard.c` 与 `kernel/ipc/input_srv.c` 使用
    `notify_publish(&notify_system, EVENT_KEY_PRESSED, ...)` 发布；
  - `kernel/fs/ttyfs.c` 使用
    `notify_subscribe(&notify_system, EVENT_KEY_PRESSED, ...)` 订阅；
  - `kernel/kmain.c` 调用 `notify_system_init()`。
- `kernel/proc/sched.c`：删除 `do_context_switch()` 中的 `eb_dispatch()`，以及已无
  引用的 `sched_wait_event()` 与 `sched_resume_event()`。
- `kernel/proc/syscall.c`：删除未使用的事件总线头文件。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/eventbus.{c,h}` | 删除 |
| `kernel/proc/notify.{h,c}` | 共享通知对象与初始化 |
| `kernel/device/keyboard/keyboard.c` | 经 notify 发布 |
| `kernel/fs/ttyfs.c` | 经 notify 订阅 |
| `kernel/ipc/input_srv.c` | 经 notify 中继 |
| `kernel/kmain.c` | `notify_system_init()` |
| `kernel/proc/sched.{c,h}` | 删除 `eb_dispatch()` 与未使用的 wait/resume 辅助 |
| `kernel/proc/syscall.c` | 删除未使用的头文件 |

## 测试

- `make -C kernel clean && make -C kernel` 无告警；全量镜像构建成功。
- 内核内键盘（服务关闭）：`ls` → `pwd` 正常。
- 用户态输入服务（服务开启）：`-smp 2` 与 `-smp 4` 上 `ls` → `pwd` 均正常。
