# 2026-06-07：异常处理程序Bug修复与帧缓冲Write-Combining

## 概述

本次提交修复了两个独立问题：

1. **异常处理程序寄存器转储错误** — `pusham` 宏与 `task_regs_t` 结构体之间存在 16 字节布局不匹配，该 Bug 自内核首次编写以来一直存在，导致所有异常寄存器转储不可靠，并且 CPL 检测始终返回"内核模式"。
2. **帧缓冲 Write-Combining** — 将 PAT MSR 的索引 2 设置为 Write-Combining (WC)，同时帧缓冲页表项使用 `PCD=1,PWT=0`（PAT 索引 2），从而为线性帧缓冲启用 WC 内存类型。

---

## Bug 1：pusham / task_regs_t 布局不匹配

### 根因

`kernel/sys/isr_base.s` 中的 `pusham` 宏向栈中压入了 **17 个值**：

```
push %fs       (8 字节)
push %gs       (8 字节)
push %rax      (8 字节)
...            (13 个其他通用寄存器)
push %r15      (8 字节)
```

但 `task_regs_t`（`kernel/proc/task.h`）只期望 **15 个通用寄存器**，随后紧跟异常帧（`RIP, CS, RFLAGS, RSP, SS`）。`pusham` 压入的 FS 和 GS 值在 `task_regs_t` 中没有对应字段，导致所有寄存器转储产生 16 字节的偏移。

### 影响

1. **寄存器转储显示错误的值** — 由于栈布局偏移了 16 字节，当 `exc_handler_proc()` 读取 `tr->cs` 时，实际读取的是一个 GPR 值而非真正的 CS。这使得每次崩溃输出都不可靠，无法用于调试。

2. **`detect_cpl` 始终返回内核模式** — `isr_base.s` 中的 `detect_cpl` 宏从 `8(%r12)`（pusham 后的偏移量 8）读取 CS。但由于有 17 次压栈（136 字节），CS 实际位于：
   - `17 * 8 = 136 字节 + 8（错误码）+ 8（CPU 压入的 RIP）= 从原始 RSP 起 152 字节`
   - `pusham` 后，RSP 指向最后一个压入的值（r15）。CS 位于 RSP 上方 `17 * 8 = 136` 字节处。
   - 宏读取了 `8(%r12)`，其中 `r12 = RSP`，因此读取的是 GPR 而不是 CS。
   - 由于 GPR 值的低 2 位很少设置为 3，因此 CPL 始终被检测为 0（内核模式）。
   - 用户模式的返回清理路径（`addq $24 + iretq`）实际上是一段死代码。

3. **错误码从错误的栈偏移位置读取** — 在 `exc_noerrcode` 和 `exc_errcode` 中，错误码偏移量计算为 136（内核）或 152（用户），考虑了 FS/GS 的压栈。移除 FS/GS 后，这些偏移量应为 120（内核）或 136（用户）。

4. **`.exc_end` 清理过于复杂** — 由于 CPL 检测错误，清理始终走内核路径（`addq $8 + iretq`）。用户路径（`addq $24 + iretq`）是死代码。由于 `iretq` 在返回 CS 指示 CPL 变化时会自动弹出 RSP+SS，因此单次 `addq $8 + iretq` 适用于两种模式。

### 修复内容

1. **`pusham` 宏** — 移除了 `push %fs` 和 `push %gs`。现在恰好压入 15 个通用寄存器（120 字节），与 `push_all` 和 `task_regs_t` 匹配。

2. **`popam` 宏** — 相应移除了 `pop %gs` 和 `pop %fs`。

3. **`detect_cpl` 宏** — 将偏移量从 `8(%r12)` 改为 `136(%r12)`：
   - `pusham` 后（15 个 GPR = 120 字节），加上虚拟错误码（8 字节）和 CPU 压入的 RIP（8 字节），CS 位于偏移 `120 + 8 + 8 = 136` 处。
   - 对于用户模式异常，SS+RSP 也在栈上，总计为 `15*8 + 8 + 8 + 8 + 8 = 144`，两种情况下 CS 都在距 RSP 偏移 136 处。

4. **错误码偏移量计算** — 在 `exc_noerrcode` 和 `exc_errcode` 中：
   - 内核偏移量：`136 → 120`（仅 15 个 GPR）
   - 用户偏移量：`152 → 136`（15 个 GPR + SS + RSP）

5. **移除 `exc14` 中无用的 CR2 保存/恢复** — exc14 处理程序中有一段 `push %rax; mov %cr2, %rax; push %rax; pop %rax` 序列，在栈上留下了一个 8 字节的间隙。已移除，因为 CR2 已经作为单独参数（第 4 个参数，`%r8`）传递给 `exc_handler_proc`。

6. **`.exc_end` 简化** — 将内核和用户模式统一为 `addq $8 + iretq`。对于用户模式，`iretq` 会在检测到返回 CS 值指示 CPL 变化时自动弹出 RSP+SS。

---

## Bug 2：PAT Write-Combining 未生效

### 根因

帧缓冲使用 `VMM_FLAGS_DEFAULT`（PCD=0, PWT=0 → PAT 索引 0 = Write-Back）映射。即使 PAT MSR 被修改为将索引 2 设置为 WC，但没有 PTE 引用 PAT 索引 2，因此帧缓冲的内存类型仍然是 Write-Back。

### 修复内容

1. **`VMM_FLAGS_FB_WC` 宏定义** — 在 `kernel/mm/mm.h` 中添加：`VMM_FLAGS_FB_WC = VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE`。
   - `VMM_FLAG_CACHE_DISABLE` 在 PTE 中设置 PCD=1。
   - 结合 PWT=0（默认值），选择 PAT 索引 2。
   - PAT 索引 2 随后通过 MSR 写入配置为 Write-Combining。

2. **帧缓冲映射** — 修改 `kernel/mm/vmm.c` 中的 `vmm_init()`，对帧缓冲区域使用 `VMM_FLAGS_FB_WC` 代替 `VMM_FLAGS_DEFAULT`。

3. **`fb_set_wc()` 函数** — 在 `kernel/sys/mtrr.c` 中新增。该函数：
   - 通过 `CPUID` 检查 PAT CPU 特性支持。
   - 读取当前 PAT MSR（0x277）。
   - 清除字节 2（bits 23:16）。
   - 将字节 2 设置为 `MTRR_CACHE_WRITE_COMBINING`（值 1）。
   - 写入修改后的 PAT MSR。

4. **`fb_set_wc()` 调用位置** — 放在 `vmm_init()` 的帧缓冲条目处理程序中，而不是 `kmain()` 中，以避免 LTO 代码生成敏感性问题（见下文）。

---

## Bug 3：MTRR 覆盖使 PAT 在物理机上失效

### 根因

即使已将 PAT 索引 2 设置为 WC 并使用 `PCD=1,PWT=0`（PAT 索引 2）映射帧缓冲，有效内存类型在物理机上仍然是 UC（Uncacheable）。根据 Intel/AMD 内存类型组合规则，当某地址范围的 MTRR 指定为 **UC** 时，PAT 设置会被**完全覆盖**，无论 PAT 如何设置，有效类型始终为 UC。

在目标机器（AMD Ryzen 7 5700U）上，BIOS 配置了 4 个 type=0 (UC) 的变量 MTRR，覆盖了 0xE0000000 处的帧缓冲区域：

| MTRR | 类型 | 基址 | 掩码 | 范围 |
|------|------|------|------|-------|
| #0 | UC (0) | 0xE0000000 | 0x7FE0000000 | 32 MB @ 0xE0000000 |
| #1 | UC (0) | 0xDC000000 | 0x7FFC000000 | 16 MB @ 0xDC000000 |
| #2 | UC (0) | 0xDA000000 | 0x7FFE000000 | 8 MB @ 0xDA000000 |
| #3 | UC (0) | 0xD9800000 | 0x7FFF800000 | 4 MB @ 0xD9800000 |

MTRR #0 覆盖 0xE0000000–0xE1FFFFFF（32 MB），完全包含帧缓冲（0xE0000000，1920000 字节 ≈ 1.83 MB）。MTRR=UC 导致 PAT=WC 的有效类型为 UC，帧缓冲写入速度缓慢。

### 修复内容：将 MTRR #0 修改为 WC

在 `fb_set_wc()` 函数中扩展了以下逻辑：

1. **检测覆盖帧缓冲的 UC MTRR** — 遍历所有变量 MTRR；如果某个 MTRR 类型为 UC、有效且完全覆盖帧缓冲范围（`fb_phys` 和 `fb_end` 均匹配 MTRR 模式），则记录其索引。

2. **原地修改 MTRR**，使用标准的缓存控制流程：
   - 保存 CR0，设置 CD=1（Cache Disable），清除 NW=0（Not Write-through）
   - 执行 `wbinvd` 刷新所有缓存
   - 通过 CR3 重载刷新 TLB
   - **禁用 MTRRs**：清除 `IA32_MTRR_DEF_TYPE`（MSR 0x2FF）的 E 标志（bit 11）
   - 写入 MTRR `PHYSBASE` 寄存器，将类型从 UC (0) 改为 WC (1)
   - **重新启用 MTRRs**：设置 E 标志
   - 再次刷新 TLB 和缓存
   - 恢复 CR0

3. **验证** — 回读 MTRR PHYSBASE 寄存器，记录类型是否成功更改为 WC。

### 结果

修复后，MTRR 和 PAT 均为帧缓冲指定了 WC，有效内存类型为 WC。在以下环境验证通过：
- **QEMU**（Intel i5-3320M）：`MTRR #0 type now WC (expected WC=1)`
- **物理机**（AMD Ryzen 7 5700U）：`MTRR #0 type now WC (expected WC=1)`

帧缓冲写入性能在物理机上得到明显提升。

### 其他修复：MTRR 范围重叠计算

原始的 range_end 计算使用了 `base | ~mask`，但没有将 `~mask` 限制在物理地址宽度内。由于 MTRR 掩码在 MAXPHYADDR 以上的位（如 bits 63:48）通常为零，`~mask` 会产生高位被置 1 的值，导致 range_end 溢出到高位地址空间。这导致 MTRR #1、#2、#3 在 QEMU 和物理机上均出现假阳性重叠报告。

修复方式：使用 `~mask & 0x000ffffffffff000`（覆盖 52 位物理地址空间）限制可变位范围，并使用三种方式检测重叠：

```
(fb_phys & mask) == (base & mask) ||
(fb_end  & mask) == (base & mask) ||
((base & mask) >= fb_phys && (base & mask) <= fb_end)
```

---

## LTO 代码生成敏感性

在调试过程中发现，从 `kmain()`（在 `vmm_init()` 返回之后）调用 `fb_set_wc()` 会导致 `enter_context_switch+0x26`（一个故意的三故障 PF）处确定性地崩溃。即使移除所有 PAT/页表更改并将函数体缩减为简单的 `return`，仅函数调用的存在就会触发崩溃。

将 `fb_set_wc()` 调用移到 `vmm_init()`（帧缓冲条目处理程序）内部可避免崩溃，这表明编译器的 LTO（链接时优化）在 `kmain()` 中重新排序或内联代码，导致生成了损坏的可执行文件。同时还添加了 `__attribute__((noinline))` 注解作为预防措施。

---

## 次要更改

1. **`kernel/sys/smp.c`** — 添加了 `#include <sys/timer.h>`（需要 `use_apic_timer` 变量）。
2. **`kernel/kmain.c`** — 添加了 `#include <sys/timer.h>` 并移除了注释掉的 MTRR 保存/恢复代码。

---

## 更改的文件

| 文件 | 更改内容 |
|------|---------|
| `kernel/sys/isr_base.s` | pusham/popam（移除 FS/GS）、detect_cpl 偏移量（8→136）、错误码偏移量（136→120、152→136）、exc14 CR2 间隙移除、.exc_end 统一 |
| `kernel/mm/mm.h` | 添加 VMM_FLAGS_FB_WC 宏定义 |
| `kernel/mm/vmm.c` | fb_set_wc() 调用 + 帧缓冲映射使用 VMM_FLAGS_FB_WC |
| `kernel/sys/mtrr.c` | 新增 fb_set_wc() 函数 |
| `kernel/sys/mtrr.h` | fb_set_wc() 声明 |
| `kernel/kmain.c` | 添加 timer.h 头文件，移除无用的 MTRR 注释 |
| `kernel/sys/smp.c` | 添加 timer.h 头文件 |
