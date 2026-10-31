# 2026-10-31：清理剩余代码 TODO

## 变更

- faccessat：校验标志。`AT_EACCESS` 等同默认检查，其他标志返回 EINVAL。
- sched：每次切换检查内核栈低端金丝雀。fork、clone 与创建路径都写入。
- madt：解析并记录 NMI 源记录。
- mmap：拒绝首个页以下的 `MAP_FIXED`，保持零页未映射。
- sched/kshell：移除 idle 与异常探针的过期注释。
- kmalloc：注明清零是有意为之。

## 验证

- GCC 10.2，TCG：`-smp 1/4` 启动标记通过；musltest、faulttest、cloexectest、exitgrouptest 通过；MADT 打印 `NMI source 255 lint 1`。
