# 2026-10-17：Haswell 键盘卡死、APIC ID 读取与 AHCI 端口拉起

## 现象

Haswell-ULT 笔记本（i5-4300U）启动后进入 shell。内核能读到扫描码。用户态 input 服务不投递按键。shell 没有输入。光标仍在闪。内核线程继续运行。

QEMU 与 Skylake 笔记本用同一镜像。键盘正常。

## 根因

`serial_init_rx()` 用暂存寄存器探测 COM1。这台 Haswell 笔记本没有 COM 口。端口读出 `0xff`。探测失败并返回。函数返回时未清零 `serial_base`。

`serial_drain()` 随后读取 0x3F8 的线路状态寄存器。悬空总线返回 `0xff`。bit 0（数据就绪）恒为 1。`for (;;)` 循环反复调用 `sys_ioport_access()`。循环不退出。

启动早期的一次伪 IRQ1 唤醒了 input 服务。处理函数先跑 `ps2_drain()`。函数随后进入 `serial_drain()`。服务停在该循环里。服务不再回到接收循环。后续按键通知只入队。无人读取通知。

## 修复

### 串口控制台（`userspace/servers/input.c`）

- 暂存寄存器探测失败时清零 `serial_base`。
- 给 `serial_drain()` 加 32 字节上限。
- 数据字节为 `0xff` 时按悬空总线处理。退出循环。
- 给 `ps2_drain()` 加 32 字节上限。

### IRQ1 字节转发（`kernel/arch/x64/isr.c`、`kernel/ipc/irq.c`、`kernel/ipc/irq.h`）

- IRQ1 处理函数等待输出缓冲。等待有上限。函数随后读取 i8042 数据端口。
- `irq_deliver_data()` 把字节放进通知的 `words[1]`。
- 注释记录：部分机器的字节在中断之后才到 i8042。

### APIC ID 读取（`kernel/arch/x64/smp.c`）

- 从 ID 寄存器 bit 24..31 读取本核 APIC ID。与 Linux `read_apic_id()` 一致。
- 旧代码拿整个寄存器对比 MADT 的 APIC ID。只有 ID 0 能匹配。

### AHCI（`userspace/servers/block.c`）

- `port_start()` 把 ICC 置为 Active。函数随后清除 ST。
- 新增 `port_bringup()`。端口有设备签名且 DET 低于 3 时，函数发送 COMRESET。函数等待 DET 到 3。
- 探测前对 PI 位图里的每个端口执行 `port_bringup()`。

## 改动文件

| 文件 | 改动 |
|------|------|
| `userspace/servers/input.c` | 探测失败清零 `serial_base`；两个 drain 加界；丢弃 `0xff` 字节 |
| `kernel/arch/x64/isr.c` | IRQ1 中读取 i8042 字节并转发 |
| `kernel/ipc/irq.c`、`kernel/ipc/irq.h` | `irq_deliver_data()` 携带一个字 |
| `kernel/arch/x64/smp.c` | APIC ID 寄存器右移 24 位 |
| `userspace/servers/block.c` | ICC 置 Active；COMRESET 拉起端口 |

## 测试

- 内核与用户态编译无 `-Wall -Wextra` 警告。
- QEMU `-smp 1`、`-smp 2`、`-smp 4`：键盘可执行 `/bin/ls`。无 panic、exception、corrupt。
- Haswell：修复后键盘可执行命令。
- Skylake：启动正常。命令可执行。
- AHCI 改动等待物理 SATA 盘验证。
