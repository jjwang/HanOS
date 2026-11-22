# 端口镜像运行 login 并恢复回显

## login

端口镜像带 `/etc/ports-image`。init 曾直接 exec `/data/bin/bash`。它绕过 login。首启设密提示不出现。现在 init 等 shell 就绪后 exec `/bin/login`。login 再选 shell。

## 回显

`pw_read_password` 关闭回显后，恢复的是关闭后重读的 termios。echo 一直为关。shell 继承无回显，输入不显示。改为恢复关闭前保存的 termios。

## 验证

端口镜像首启提示设置 root 新密码。登录后 shell 回显输入的命令。
