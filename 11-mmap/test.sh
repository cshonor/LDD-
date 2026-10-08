#!/bin/bash
# 11-mmap 真机验证脚本（在树莓派上执行）
# 注意：deploy_module.py 只上传 .c/Makefile，本脚本需手动 scp 到同一目录。
set -euo pipefail
cd "$(dirname "$0")"

echo "=== ① 编译模块与测试程序 ==="
make
gcc -O2 -Wall -o test_mmap test_mmap.c

echo "=== ② 加载 ==="
sudo insmod mmapbuf.ko

echo "=== ③ 对照路径：read + copy_to_user 读 init 内容 ==="
sudo cat /dev/mmapbuf | head -c 64; echo

echo "=== ④ 零拷贝路径：mmap 直接读写 ==="
./test_mmap

echo "=== ⑤ 内核侧确认用户态的 mmap 写入（同一块物理内存）==="
sudo cat /dev/mmapbuf | head -c 64; echo

echo "=== ⑥ 交互日志 ==="
sudo dmesg | tail -8

echo "=== ⑦ 卸载（test_mmap 已 munmap，安全）==="
sudo rmmod mmapbuf
sudo dmesg | tail -2

echo "=== 全部通过 ==="
