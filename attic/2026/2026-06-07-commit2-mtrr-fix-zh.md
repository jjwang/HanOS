# 2026-06-07 Commit 2 (`b6751b7`)：修复 MTRR 范围重叠计算，将帧缓冲 MTRR 设为 WC

## Bug 1：MTRR 覆盖使 PAT 在物理机上失效

### 根因

- PAT 索引 2 = WC，帧缓冲以 `PCD=1,PWT=0` 映射，有效内存类型仍为 UC。
- Intel/AMD 组合规则：地址范围的 MTRR 指定 **UC** 时，PAT 失效。

在 AMD Ryzen 7 5700U 上，BIOS 配置了 4 个 UC MTRR，覆盖 0xE0000000 帧缓冲：

| MTRR | 类型 | 基址 | 掩码 | 范围 |
|------|------|------|------|-------|
| #0 | UC (0) | 0xE0000000 | 0x7FE0000000 | 32 MB @ 0xE0000000 |
| #1 | UC (0) | 0xDC000000 | 0x7FFC000000 | 16 MB @ 0xDC000000 |
| #2 | UC (0) | 0xDA000000 | 0x7FFE000000 | 8 MB @ 0xDA000000 |
| #3 | UC (0) | 0xD9800000 | 0x7FFF800000 | 4 MB @ 0xD9800000 |

MTRR #0 覆盖 0xE0000000–0xE1FFFFFF（32 MB），包含 1.83 MB 帧缓冲。MTRR=UC + PAT=WC → UC。

### 修复：将 MTRR #0 修改为 WC

扩展 `kernel/sys/mtrr.c` 中的 `fb_set_wc()`：

1. **检测覆盖帧缓冲的 UC MTRR** — 遍历变量 MTRR。类型为 UC、有效、覆盖帧缓冲时，记录索引。

2. **原地修改 MTRR**，使用标准缓存控制流程：
   - 保存 CR0，设置 CD=1，清除 NW=0
   - `wbinvd`（刷新缓存）
   - 重载 CR3（刷新 TLB）
   - **禁用 MTRRs**（清除 `IA32_MTRR_DEF_TYPE` MSR 0x2FF 的 E 标志）
   - 写入 MTRR PHYSBASE，类型 UC→WC
   - **重新启用 MTRRs**（设置 E 标志）
   - 再次刷新 TLB + 缓存
   - 恢复 CR0

3. **验证** — 回读 MTRR PHYSBASE，记录结果。

### 结果

在 QEMU 与 Ryzen 7 5700U 上验证：`MTRR #0 type now WC (expected WC=1)`。帧缓冲写入性能提升。

---

## Bug 2：MTRR 范围重叠计算

### 根因

- 原始 `range_end = base | ~mask` 未限制 `~mask` 到物理地址宽度。
- MTRR 掩码在 bits 63:48 为零，`~mask` 产生高位值，range_end 溢出。
- MTRR #1、#2、#3 出现假阳性重叠。

### 修复

使用 `~mask & 0x000ffffffffff000` 限制可变位。重叠检测分三种：

```
(fb_phys & mask) == (base & mask) ||
(fb_end  & mask) == (base & mask) ||
((base & mask) >= fb_phys && (base & mask) <= fb_end)
```

---

## 更改的文件

| 文件 | 更改内容 |
|------|---------|
| `kernel/sys/mtrr.c` | 修复 range_end 计算，在 fb_set_wc() 中添加 MTRR UC→WC 修改逻辑 |
