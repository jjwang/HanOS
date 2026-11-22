# 用 QMP 注入测试输入

HMP `sendkey` 会吞掉连续两个相同字符里的第二个：输入 `root` 到 guest 变成 `rot`，登录测试无法通过。`scripts/stress-test.sh` 改为打开 QMP socket，对每个字符调用 `input-send-event`，显式发送按下/抬起，不再合并重复字符。
