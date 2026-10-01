# 2026-06-07 Commit 2 (`b6751b7`): Fix MTRR range overlap calculation and set FB region MTRR to WC

## Bug 1: MTRR Override Makes PAT Ineffective on Real Hardware

### Root Cause

PAT index 2 = WC and a framebuffer mapped with `PCD=1,PWT=0` still gave effective type UC. Intel/AMD combination rules: when MTRR for an address range specifies **UC**, PAT is overridden.

On AMD Ryzen 7 5700U, BIOS had 4 UC MTRRs covering the framebuffer at 0xE0000000:

| MTRR | Type | Base | Mask | Range |
|------|------|------|------|-------|
| #0 | UC (0) | 0xE0000000 | 0x7FE0000000 | 32 MB @ 0xE0000000 |
| #1 | UC (0) | 0xDC000000 | 0x7FFC000000 | 16 MB @ 0xDC000000 |
| #2 | UC (0) | 0xDA000000 | 0x7FFE000000 | 8 MB @ 0xDA000000 |
| #3 | UC (0) | 0xD9800000 | 0x7FFF800000 | 4 MB @ 0xD9800000 |

MTRR #0 covers 0xE0000000–0xE1FFFFFF (32 MB), containing the framebuffer (1.83 MB). MTRR=UC + PAT=WC → UC.

### Fix: Modify MTRR #0 to WC

Extended `fb_set_wc()` in `kernel/sys/mtrr.c`:

1. **Detect covering UC MTRR** — Iterate variable MTRRs. If type=UC, valid, and covering the framebuffer, record the index.

2. **Modify MTRR in place** with the standard cache-control procedure:
   - Save CR0, set CD=1, clear NW=0
   - `wbinvd` (flush caches)
   - CR3 reload (flush TLB)
   - **Disable MTRRs** (clear E flag in `IA32_MTRR_DEF_TYPE` MSR 0x2FF)
   - Write MTRR PHYSBASE with type UC→WC
   - **Re-enable MTRRs** (set E flag)
   - Flush TLB + caches again
   - Restore CR0

3. **Verify** — Read back MTRR PHYSBASE, log result.

### Result

Verified on QEMU and Ryzen 7 5700U: `MTRR #0 type now WC (expected WC=1)`. Framebuffer write performance improved measurably.

---

## Bug 2: MTRR Range Overlap Calculation

### Root Cause

Original `range_end = base | ~mask` did not limit `~mask` to the physical address width. MTRR masks have zeros at bits 63:48, so `~mask` produced high-bit values and range_end overflowed. This gave false overlap positives for MTRRs #1, #2, #3.

### Fix

Use `~mask & 0x000ffffffffff000` for the variable bits. Overlap check has three cases:

```
(fb_phys & mask) == (base & mask) ||
(fb_end  & mask) == (base & mask) ||
((base & mask) >= fb_phys && (base & mask) <= fb_end)
```

---

## Files Changed

| File | Changes |
|------|---------|
| `kernel/sys/mtrr.c` | Fixed range_end calculation, added MTRR UC→WC modification logic in fb_set_wc() |
