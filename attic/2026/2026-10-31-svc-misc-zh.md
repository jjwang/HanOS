# 2026-10-31：杂项服务承接时钟、熵与 uname

## 变更

- misc：新增 `include/misc.h`，定义 `MISC_*` 标签与 `utsname` 布局。
- misc：新增 `kernel/srv/misc_srv.{h,c}`。
- misc：创建 endpoint，授予 HPET 窗口与启动时刻。
- misc：拉起 `/bin/misc`，注册 `SVC_MISC`。
- misc：新增 `userspace/servers/misc.c`。
- misc：用 HPET 计数器答复 `clock_gettime`。
- misc：填 `uname`。
- misc：用 HPET 计数器做种子，用 xorshift 填 `getrandom`。
- kernel：把 `k_getclock`、`k_uname`、`k_getentropy` 转发到 `SVC_MISC`。`clock_now` 留给内核内部的绝对时间睡眠。
- hpet：导出物理基址、计数周期与基准计数。
- bootinfo：新增 misc 的 HPET 窗口、启动时刻与版本。
- kmain：启动 misc 服务，运行 `misc_selftest`，打印 `MK: misc PASS`。
- ci：启动脚本要求 `MK: misc PASS`。
- musltest：打印 `uname`、`getrandom` 与 `CLOCK_REALTIME`。
- docs：把 `SVC_MISC` 标为完成。

## 验证

- GCC 13.1 与交叉 GCC 10.2，TCG，`-smp 1`：`MK: selftest PASS`，`MK: misc PASS`，shell 以 `hansh` 启动。
