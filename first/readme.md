# First Kernel Module

最简单的 Linux 内核模块示例，加载/卸载时通过 printk 输出信息。

## 文件说明

| 文件 | 说明 |
|------|------|
| `hello.c` | 模块源码，定义 `my_init` / `my_exit` |
| `module.h` | 公共头文件 |
| `Makefile` | 编译脚本 |

## 编译与加载

```bash
make                    # 编译生成 hello.ko
sudo insmod hello.ko    # 加载模块
dmesg | tail            # 查看 "Hello: Hello, Kernel"
sudo rmmod hello        # 卸载模块
dmesg | tail            # 查看 "Hello: Goodbye, Kernel"
make clean              # 清理编译产物
```

## 依赖

- 内核头文件：`linux-headers-$(uname -r)`
- 构建工具：`make`、`gcc`
