# 2026-10-31：misc 时钟为零导致 PS/2 输入失效

## 现象

Skylake 笔记本上 shell 无法输入。USB 键盘仍可用。QEMU 在 PS/2 路径复现：内核读到扫描码并投递，输入服务不产生按键。

## 根因

`hpet_phys` 已声明，也被 `hpet_phys_base()` 读取，但 `hpet_init` 从未赋值。misc 服务没有 HPET 映射，`clock_gettime(CLOCK_MONOTONIC)` 返回 0。输入服务的 `now_ms()` 用该时钟，按键去抖 `now_ms() - release_ms >= 25` 永不成立，所有 PS/2 按键被丢弃。USB HID 由内核解码，不用该时钟。

## 变更

- hpet：在 `hpet_init` 保存物理基址，misc 授权能映射 HPET 窗口。
- misc：单调时钟为零时 `misc_selftest` 失败。

## 验证

- GCC 10.2，TCG，`-smp 1`：PS/2 `sendkey` 能运行 `ls`；`MK: selftest PASS`、`MK: misc PASS`、`MK: signalfd PASS`。
