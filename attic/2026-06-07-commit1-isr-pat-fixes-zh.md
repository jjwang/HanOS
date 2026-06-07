# 2026-06-07 Commit 1 (`ec7c458`)：修复异常处理程序寄存器转储错误，启用帧缓冲 WC

## Bug 1：pusham / task_regs_t 布局不匹配

### 根因

`kernel/sys/isr_base.s` 中的 `pusham` 宏向栈压入 **17 个值**：

```
push %fs       (8 字节)
push %gs       (8 字节)
push %rax      (8 字节)
...            (13 个其他 GPR)
push %r15      (8 字节)
```

但 `task_regs_t`（`kernel/proc/task.h`）只期望 **15 个 GPR**，随后紧跟异常帧（`RIP, CS, RFLAGS, RSP, SS`）。FS/GS 无对应字段，导致所有寄存器转储偏移 16 字节。

### 影响

1. **寄存器转储值错误** — `exc_handler_proc()` 读取 `tr->cs` 时实际读到的是 GPR 值。所有崩溃输出均不可靠。

2. **`detect_cpl` 始终返回内核模式** — 从 `8(%r12)` 读取 CS，但实际 CS 在偏移 136 处。GPR 低 2 位很少为 3，CPL 始终为 0。用户模式返回路径是死代码。

3. **错误码偏移错误** — 偏移量 136/152（含 FS/GS），应为 120/136（仅 15 GPR）。

4. **`.exc_end` 过于复杂** — 因 CPL 始终为内核模式，用户路径无用。`addq $8 + iretq` 适用于两种模式（iretq 在 CPL 变化时自动弹出 RSP+SS）。

### 修复

1. **`pusham`/`popam`** — 移除 FS/GS 压栈/弹出。现压入 15 个 GPR（120 字节），与 `push_all` 和 `task_regs_t` 匹配。

2. **`detect_cpl`** — 偏移量 `8(%r12)` → `136(%r12)`（15 GPR + 错误码 + RIP）。

3. **错误码偏移** — 内核 `136→120`，用户 `152→136`（15 GPR ± SS+RSP）。

4. **移除 `exc14` CR2 间隙** — 移除死代码 `push %rax; mov %cr2, %rax; push %rax; pop %rax`（遗留 8 字节间隙）。CR2 作为单独参数传给 `exc_handler_proc`。

5. **`.exc_end`** — 统一为 `addq $8 + iretq`。

---

## Bug 2：PAT Write-Combining 未生效

### 根因

帧缓冲使用 `VMM_FLAGS_DEFAULT`（PCD=0,PWT=0 → PAT 索引 0 = WB）映射。即使 PAT MSR 修改了索引 2 = WC，但无 PTE 引用索引 2。

### 修复

1. **`VMM_FLAGS_FB_WC`** — 在 `mm.h` 中添加：`VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE`（PCD=1,PWT=0 → PAT 索引 2）。

2. **帧缓冲映射** — `vmm_init()` 对帧缓冲使用 `VMM_FLAGS_FB_WC`。

3. **`fb_set_wc()`** — `mtrr.c` 中新增函数：读取 PAT MSR（0x277），设置字节 2 为 WC（值 1），写回。

4. **调用位置** — 在 `vmm_init()` 帧缓冲处理程序中调用，而非 `kmain()`，以避免 LTO 代码生成崩溃。

---

## LTO 代码生成敏感性

从 `kmain()` 调用 `fb_set_wc()` 会导致 `enter_context_switch+0x26` 处确定性崩溃（即使移除所有 PAT/PTE 更改）。移入 `vmm_init()` 后崩溃消失。添加了 `__attribute__((noinline))` 注解作为预防。

---

## 次要更改

1. **`kernel/sys/smp.c`** — 添加 `#include <sys/timer.h>`（需要 `use_apic_timer`）。
2. **`kernel/kmain.c`** — 添加 `#include <sys/timer.h>`，移除无用 MTRR 注释。

---

## 更改的文件

| 文件 | 更改内容 |
|------|---------|
| `kernel/sys/isr_base.s` | pusham/popam（移除 FS/GS）、detect_cpl 偏移（8→136）、错误码偏移（136→120、152→136）、移除 exc14 CR2 间隙、统一 .exc_end |
| `kernel/mm/mm.h` | 添加 VMM_FLAGS_FB_WC 宏定义 |
| `kernel/mm/vmm.c` | fb_set_wc() 调用 + 帧缓冲使用 VMM_FLAGS_FB_WC |
| `kernel/sys/mtrr.c` | 新增 fb_set_wc() 函数 |
| `kernel/sys/mtrr.h` | fb_set_wc() 声明 |
| `kernel/kmain.c` | 添加 timer.h 头文件，移除无用 MTRR 注释 |
| `kernel/sys/smp.c` | 添加 timer.h 头文件 |
