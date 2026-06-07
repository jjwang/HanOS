# 2026-06-07: Exception Handler Bug Fixes and Framebuffer Write-Combining

## Overview

Two independent issues were fixed in this commit:
1. **Exception handler register dump corruption** — the `pusham` macro and `task_regs_t` struct had a 16-byte layout mismatch that has existed since the kernel was first written, making all exception register dumps unreliable and CPL detection always return "kernel mode".
2. **Framebuffer write-combining** — PAT MSR index 2 is set to Write-Combining (WC), and the framebuffer page-table entries use `PCD=1,PWT=0` (PAT index 2) to enable WC memory type for the linear framebuffer.

---

## Bug 1: pusham / task_regs_t Layout Mismatch

### Root Cause

The `pusham` macro in `kernel/sys/isr_base.s` pushed **17 values** onto the stack:

```
push %fs       (8 bytes)
push %gs       (8 bytes)
push %rax      (8 bytes)
...            (13 more GPRs)
push %r15      (8 bytes)
```

But `task_regs_t` (in `kernel/proc/task.h`) expected only **15 GPRs** directly followed by the exception frame (`RIP, CS, RFLAGS, RSP, SS`). The FS and GS values pushed by `pusham` had no corresponding fields in `task_regs_t`, creating a 16-byte shift in ALL register dumps.

### Impact

1. **Register dumps showed wrong values** — Since the stack layout was shifted by 16 bytes, when `exc_handler_proc()` read `tr->cs`, it was actually reading a GPR value instead of the real CS. This made every crash output unreliable for debugging purposes.

2. **`detect_cpl` always returned kernel mode** — The `detect_cpl` macro in `isr_base.s` read CS from `8(%r12)` (offset 8 after pusham). But with 17 pushes (136 bytes), CS was at:
   - `17 * 8 = 136 bytes + 8 (error code) + 8 (RIP pushed by CPU) = 152 bytes` from the original RSP
   - After `pusham`, RSP points to the last pushed value (r15). CS is at `17 * 8 = 136` bytes above RSP.
   - The macro read at `8(%r12)` where `r12 = RSP`, so it read a GPR instead of CS.
   - Since GPR values rarely have the low 2 bits set to 3, CPL was always detected as 0 (kernel mode).
   - The user-mode return cleanup path (`addq $24 + iretq`) was effectively dead code.

3. **Error code was read from wrong stack offset** — In both `exc_noerrcode` and `exc_errcode`, the error code offset was calculated as 136 (kernel) or 152 (user), accounting for the FS/GS pushes. After removing FS/GS, these offsets should be 120 (kernel) or 136 (user).

4. **`.exc_end` cleanup was overly complex** — With the wrong CPL detection, the cleanup always took the kernel path (`addq $8 + iretq`). The user path (`addq $24 + iretq`) was dead code. Since iretq automatically pops RSP+SS when the return CS indicates CPL change, a single `addq $8 + iretq` works for both modes.

### Fixes Applied

1. **`pusham` macro** — Removed `push %fs` and `push %gs`. Now pushes exactly 15 GPRs (120 bytes), matching `push_all` and `task_regs_t`.

2. **`popam` macro** — Removed `pop %gs` and `pop %fs` accordingly.

3. **`detect_cpl` macro** — Changed offset from `8(%r12)` to `136(%r12)`:
   - After `pusham` (15 GPRs = 120 bytes), a dummy error code (8 bytes), and RIP (8 bytes pushed by CPU), CS is at offset `120 + 8 + 8 = 136`.
   - For user-mode exceptions, SS+RSP are also on the stack, making the total `15*8 + 8 + 8 + 8 + 8 = 144`, with CS at offset 136 from RSP in both cases.

4. **Error code offset calculation** — In both `exc_noerrcode` and `exc_errcode`:
   - Kernel offset: `136 → 120` (15 GPRs only)
   - User offset: `152 → 136` (15 GPRs + SS + RSP)

5. **`exc14` dead CR2 save/restore removed** — The exc14 handler had a `push %rax; mov %cr2, %rax; push %rax; pop %rax` sequence that left an 8-byte gap on the stack. Removed because CR2 is already passed as a separate argument (4th arg, `%r8`) to `exc_handler_proc`.

6. **`.exc_end` simplified** — Unified to `addq $8 + iretq` for both kernel and user modes. For user mode, iretq automatically pops RSP+SS when it detects a CPL change via the return CS value.

---

## Bug 2: PAT Write-Combining Not Effective

### Root Cause

The framebuffer was mapped with `VMM_FLAGS_DEFAULT` (PCD=0, PWT=0 → PAT index 0 = Write-Back). Even though PAT MSR was modified to set index 2 = WC, no PTE referenced PAT index 2, so the framebuffer memory type remained Write-Back.

### Fixes Applied

1. **`VMM_FLAGS_FB_WC` define** — Added to `kernel/mm/mm.h`: `VMM_FLAGS_FB_WC = VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE`.
   - `VMM_FLAG_CACHE_DISABLE` sets PCD=1 in the PTE.
   - Combined with PWT=0 (default), this selects PAT index 2.
   - PAT index 2 is then configured as Write-Combining via MSR write.

2. **Framebuffer mapping** — Changed `vmm_init()` in `kernel/mm/vmm.c` to use `VMM_FLAGS_FB_WC` instead of `VMM_FLAGS_DEFAULT` for the framebuffer region.

3. **`fb_set_wc()` function** — Added to `kernel/sys/mtrr.c`. This function:
   - Checks for PAT CPU feature support via `CPUID`.
   - Reads current PAT MSR (0x277).
   - Clears byte 2 (bits 23:16).
   - Sets byte 2 to `MTRR_CACHE_WRITE_COMBINING` (value 1).
   - Writes the modified PAT MSR.

4. **`fb_set_wc()` call** — Placed inside the framebuffer entry handler in `vmm_init()` rather than in `kmain()` to avoid LTO codegen sensitivity (see below).

---

## LTO Codegen Sensitivity

During debugging, it was discovered that calling `fb_set_wc()` from `kmain()` (after `vmm_init()` returns) caused a deterministic crash at `enter_context_switch+0x26` (a deliberate triple-fault PF). The crash was triggered by the mere presence of the function call — even with all PAT/page-table changes removed and the function body reduced to a simple `return`.

Moving the `fb_set_wc()` call to inside `vmm_init()` (the framebuffer entry handler) avoided the crash, suggesting that the compiler's LTO (Link-Time Optimization) was reordering or inlining code in `kmain()` in a way that produced a broken executable. The `__attribute__((noinline))` annotation on `fb_set_wc()` was also added as a precaution.

---

## Minor Changes

1. **`kernel/sys/smp.c`** — Added `#include <sys/timer.h>` (needed for `use_apic_timer` variable).
2. **`kernel/kmain.c`** — Added `#include <sys/timer.h>` and removed commented-out MTRR save/restore code.

---

## Files Changed

| File | Changes |
|------|---------|
| `kernel/sys/isr_base.s` | pusham/popam (removed FS/GS), detect_cpl offset (8→136), error code offsets (136→120, 152→136), exc14 CR2 gap removed, .exc_end unified |
| `kernel/mm/mm.h` | Added VMM_FLAGS_FB_WC define |
| `kernel/mm/vmm.c` | fb_set_wc() call + VMM_FLAGS_FB_WC for framebuffer mapping |
| `kernel/sys/mtrr.c` | New fb_set_wc() function |
| `kernel/sys/mtrr.h` | fb_set_wc() declaration |
| `kernel/kmain.c` | Added timer.h include, removed dead MTRR comment |
| `kernel/sys/smp.c` | Added timer.h include |
