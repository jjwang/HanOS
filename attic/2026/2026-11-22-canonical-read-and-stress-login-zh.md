# canonical 读按行返回，并改用串口登录压测

## tty

tty 服务端的 canonical 读一次返回 `min(len, 缓存)` 字节，不在换行处停下。一次大读会吞掉多行。login 用 `fgets`（带 stdio 缓冲）读用户名，用裸 `read` 读密码。多出的行进 stdio 缓冲，密码读饿死。让直接与延迟两条路径都在第一个换行处截断。

## 压测

压测脚本原来用 QMP PS/2 按键驱动登录。canonical 模式下注入被打乱。改为经 QEMU 串口控制台喂入首启登录：input server 本就读 COM1。桥接进程把 guest 输出写入日志，逐行写回登录文本。

## 验证

`scripts/stress-test.sh` 进入 shell 并打印 STRESS PASS。
