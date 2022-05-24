# HanOS

Design as below

- Bootloader: Limine is used to get the kernel running as quickly possible. Limine boot protocol is chosed by HanOS.
- CPU mode: x86-64 Long Mode is supported in HanOS. HanOS does not has plan to support other x86 modes. 
- GUI: GUI will not be the 1st priority for HanOS. But HanOS will port some GUI libraries in the future.

Progress update
- [x] Framebuffer based terminal and kernel log system
- [x] Initialize GDT and IDT to handle exceptions
- [x] Physical memory allocator and virtual memory manager
- [x] Parse ACPI tables and initialize MADT
- [x] Start up all CPUs
- [x] Set up APIC interrupt controller
- [x] Read RTC time from CMOS and configure HPET timer
- [x] When HPET timer not available in Hyper-V VM, PIT is used instead
- [x] Scheduling driven by APIC timer
- [x] Keyboard/mouse driver and command line interface
- [x] VFS and FAT32 file system 

Screenshots
- May 3, 2022: Live Demo

![Cool~~~](https://raw.githubusercontent.com/jjwang/HanOS/main/screenshot/0004-demo220503.gif)


