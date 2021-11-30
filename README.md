# HanOS

Design as below:

- Bootloader: Limine is used to get the kernel  running as quickly possible. Boot protocol stivale2 is chosed by HanOS.
- CPU mode: x86-64 Long Mode is supported in HanOS. HanOS does not has plan to support other x86 modes. 
- GUI: GUI will not be the 1st priority for HanOS. But HanOS will port some GUI libraries in the future.

Progress update
- [x] Framebuffer based terminal and kernel log system
- [x] Initialize GDT and IDT to handle exceptions

Screenshots
- Nov 20, 2021: Hello World

![Hello World](https://raw.githubusercontent.com/jjwang/HanOS/main/screenshot/0001-helloworld.png)


