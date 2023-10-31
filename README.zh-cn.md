# HanOS

![](https://tokei.rs/b1/github/jjwang/HanOS?category=code)
| [English](https://github.com/jjwang/HanOS/blob/mainline/README.md) |
[中文](https://github.com/jjwang/HanOS/blob/mainline/README.zh-cn.md)

## 设计如下

- Bootloader：Limine用于使内核尽快运行，HanOS选择了Limine启动协议
- CPU模式：HanOS支持x86-64长模式，HanOS没有计划支持其他x86模式
- GUI：GUI不是HanOS的首要任务，但HanOS将来会移植一些GUI库

## 当前进展
- [x] 基于帧缓冲的终端和内核日志系统
- [x] 初始化GDT和IDT以处理异常
- [x] 物理内存分配器和虚拟内存管理器
- [x] 解析ACPI表并初始化MADT
- [x] 启动所有CPU
- [x] 设置APIC（高级可编程中断控制器）中断控制器
- [x] 从CMOS读取RTC时间并配置HPET计时器
- [x] 由APIC计时器驱动调度
- [x] 键盘/鼠标驱动程序和命令行界面
- [x] VFS、FAT32和RAMFS文件系统，RAMFS用于从ELF文件加载和执行程序
- [x] 内核和用户空间的任务
- [x] 用于命令行界面的背景图像显示
- [x] 为bash和其他系统工具实现系统调用
- [x] 从xv6移植的简单的用户空间shell应用程序

## 字体选用
- 使用bdf2psf将来自https://font.gohu.org/的14px字体转换为psf1：

`bdf2psf gohufont-14.bdf /usr/share/bdf2psf/standard.equivalents /usr/share/bdf2psf/ascii.set 256 gohufont-14.psf`

## 如何运行
- 发布文件夹中的磁盘映像文件 - "hdd.img" 可用于测试：

`qemu-system-x86_64 -serial stdio -M q35 -m 1G -smp 2 -no-reboot -rtc base=localtime -drive id=handisk,if=ide,format=raw,bus=0,unit=0,file=hdd.img`

## 屏幕快照
- 2023年10月17日：用户空间shell演示

![Cool~~~](https://raw.githubusercontent.com/jjwang/HanOS/main/screenshot/0005-shell.gif)

