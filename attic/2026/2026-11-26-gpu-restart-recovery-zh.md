# GPU 服务重启后恢复 console

## 问题

GPU 属主进程退出。路由清空 `SVC_GPU`。重启回调没有再次拉起服务，服务停在未注册状态，console 取不到终端 sprite。

- 触发：GPU 用户态进程在编程平面之后退出。
- 现象：字符雨、光标和最后一次 sprite 内容继续被扫描。console 不再往 sprite 绘制。终端区显示冻结或空白窗口，没有状态栏。
- 预期：显示服务重启，路由注册新端点，console 重新把终端画进 sprite。

## 原因

`gpu_server_start()` 以 `if (gpu_active) return true;` 开头。属主退出后 `gpu_active` 仍为真，重启回调跳过建端点、重启进程和 `router_register()`。`router_owner_died()` 已清空 `SVC_GPU`，服务因此不可达。

console 只在启动时解析一次 GPU 端点，等待固定时间后回退。它跟不上重启后的新端点。

## 修复

- `gpu_server_start()` 仅在 `gpu_active` 为真且 `router_lookup(SVC_GPU)` 仍返回端点时早退。否则清 `gpu_active`，建新端点，重启进程并重新注册。
- console 每轮重新解析 `SVC_GPU`，重试 `GPU_CONSOLE_SURFACE` 请求，重试结束才回退 framebuffer。

## 验证

- Broadwell：显示服务重启后终端和状态栏恢复。
- QEMU：`boot test PASS`，无 panic。
- GPU 进程首次退出原因待查。本次改动与退出原因无关，保证 console 恢复。

## 涉及文件

- `kernel/srv/gpu_srv.c`
- `userspace/servers/console.c`
- `userspace/servers/gpu.c`
