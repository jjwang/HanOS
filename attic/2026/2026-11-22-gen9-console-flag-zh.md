# 把 Gen9 标志传给 console

## 问题

`c3e33e2` 用 `bi.gpu_gen9` 控制 console 的颜色预乘。内核从未给 console 填该字段。console 在所有平台都读到 0，走不透明分支。Gen9 丢失半透 sprite。

## 修复

新增 `gpu_srv_gen9()`，报告当前显示是否 Gen9。`console_spawn_attach` 据此设置 `bi.gpu_gen9`。Gen9 的 console 恢复预乘。Gen7.5 仍走不透明路径。

## 验证

Skylake 终端在字符雨上半透。Broadwell 的 GPU 桌面与终端正常。Skylake 连续启动三次正常。
