# 加大 pipe 服务端的转发超时

## 问题

bash 在首条命令打印 `start_pipeline: pgrp pipe: Out of memory`。内核把 `pipe()` 的失败统一报成 ENOMEM。失败点是 `PIPE_CREATE` 的转发。负载下 pipe 服务端有时晚于 1000ms 超时才回包。调用方于是放弃。此时管道其实已建。

## 修复

用 5000ms 超时转发 `PIPE_CREATE`。pipe 服务端在截止前回包。`pipe()` 成功。重试会再建一个管道并泄漏旧的。加长等待避免重试。

## 验证

压测不再出现 `start_pipeline` 或 `PIPE_CREATE` 失败。压测通过。
