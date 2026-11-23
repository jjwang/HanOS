# 校验 ELF 入口并只用一个装载器

## 问题

g++ 负载下的压测把一个 `cc1plus` 在 RIP 0、addr 0 处缺页杀掉。链路：`PROC_EXEC` 用默认 1s 转发超时发给 process 服务端；映射大型 `cc1plus` 可能更久，`exec_via_server` 失败后 `k_execve` 回退内核 `sched_execve`，在 process 服务端已 spawn 之外又建一个进程。process 服务端 `exec_load` 还直接用 `eh->entry` 不校验，残缺 ELF 会把进程起在 RIP 0。`sys_proc_spawn` 之后的任何失败都返回 pid，半初始化进程被当成成功。

## 修复

- `exec_load` 要求 `eh->entry` 落在某个 `PT_LOAD` 段内；只有完整映射并 `start` 才返回 pid，否则返回 -1。
- `k_execve` 在 process 服务端已注册时只走它；失败返回 `ENOEXEC`，不再回退第二个装载器。
- `PROC_EXEC` 等 process 服务端完成，不再 1s 后放弃。

## 验证

三轮压测通过，无 page fault、无 `router_forward(SVC_PROC)` 超时、无 `VFS open` 失败。
