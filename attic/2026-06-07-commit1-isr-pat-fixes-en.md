# 2026-06-07 Commit 1 (`ec7c458`): Fix exception handler register dump corruption and enable framebuffer WC

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

1. **Register dumps showed wrong values** — When `exc_handler_proc()` read `tr->cs`, it was reading a GPR value instead of the real CS. Every crash output was unreliable for debugging.

2. **`detect_cpl` always returned kernel mode** — It read CS from `8(%r12)`, but with 17 pushes the real CS was at offset 136. Since GPR values rarely have low 2 bits set to 3, CPL was always detected as 0 (kernel mode). The user-mode return path was dead code.

3. **Error code was read from wrong stack offset** — Error code offsets were 136/152 (including FS/GS), but should be 120/136 (15 GPRs only).

4. **`.exc_end` cleanup was overly complex** — With always-kernel CPL detection, the user path (`addq $24 + iretq`) was dead code. `addq $8 + iretq` works for both modes since iretq auto-pops RSP+SS on CPL change.

### Fixes Applied

1. **`pusham`/`popam`** — Removed FS/GS pushes/pops. Now pushes 15 GPRs (120 bytes), matching `push_all` and `task_regs_t`.

2. **`detect_cpl`** — Offset `8(%r12)` → `136(%r12)` (15 GPRs + error code + RIP).

3. **Error code offsets** — Kernel `136→120`, user `152→136` (15 GPRs ± SS+RSP).

4. **`exc14` CR2 gap removed** — Removed dead `push %rax; mov %cr2, %rax; push %rax; pop %rax` that left 8-byte gap. CR2 passed as separate arg to `exc_handler_proc`.

5. **`.exc_end`** — Unified to `addq $8 + iretq` for both kernel and user.

---

## Bug 2: PAT Write-Combining Not Effective

### Root Cause

Framebuffer was mapped with `VMM_FLAGS_DEFAULT` (PCD=0,PWT=0 → PAT index 0 = WB). Even though PAT MSR was modified to set index 2 = WC, no PTE referenced index 2.

### Fixes Applied

1. **`VMM_FLAGS_FB_WC`** — Added to `mm.h`: `VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE` (PCD=1,PWT=0 → PAT index 2).

2. **Framebuffer mapping** — `vmm_init()` now uses `VMM_FLAGS_FB_WC` for the framebuffer region.

3. **`fb_set_wc()`** — New function in `mtrr.c`: reads PAT MSR (0x277), sets byte 2 to WC (value 1), writes back.

4. **`fb_set_wc()` placement** — Called inside `vmm_init()` framebuffer handler rather than `kmain()` to avoid LTO codegen crash.

---

## LTO Codegen Sensitivity

Calling `fb_set_wc()` from `kmain()` caused deterministic crash at `enter_context_switch+0x26`, even with all PAT/PTE changes removed. Moving the call into `vmm_init()` avoided it. The `__attribute__((noinline))` annotation was added as precaution.

---

## Minor Changes

1. **`kernel/sys/smp.c`** — Added `#include <sys/timer.h>` for `use_apic_timer`.
2. **`kernel/kmain.c`** — Added `#include <sys/timer.h>`, removed dead MTRR comment.

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
