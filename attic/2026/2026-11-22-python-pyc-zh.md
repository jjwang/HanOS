# 预编译 Python 标准库

`3rdparty/ports/packages/python.sh` 现在对暂存的标准库执行 `python -m compileall -b`。import 直接加载相邻的 `.pyc` 字节码，不再每次启动解析 `.py`。下次 `make hdd-ports` 后生效。
