# ============================================================
# 01-first/Makefile — 内核模块的 kbuild 构建脚本
#
# 注意：Makefile 里每条规则的命令行必须以「Tab」开头。
#      用空格缩进会报 "*** missing separator. Stop."（新手第一坑）。
# ============================================================

# obj-m  = 编译成可加载模块(.ko)，用 insmod/rmmod 动态装卸
# obj-y  = 内建进内核镜像(vmlinux)，启动时就在，不能卸载
# obj-n  = 不编译
#
# 这里写的是 hello.o（不是 hello.c）：kbuild 会自动把同名的 .c 编译成 .o，
# 再由 MODPOST 阶段打包成 hello.ko。
obj-m += hello.o

# 内核构建树（kernel build tree）的路径。
# /lib/modules/<当前运行内核版本>/build 是一个符号链接，指向真正的头文件目录。
# 树莓派实测：
#   /lib/modules/6.18.34+rpt-rpi-2712/build
#     -> /usr/src/linux-headers-6.18.34+rpt-rpi-2712
#
# 关键：这个版本号必须和你「正在运行的内核」一致，否则编出来的 .ko
#       加载时会报 "invalid module format"（见 README 的坑位说明）。
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

# CURDIR 是 GNU make 的内建变量，等于当前工作目录。
# 比 $(shell pwd) 更可靠：不受环境里 PWD 变量被改的影响，
# 且路径含空格时行为更可预期。
PWD := $(CURDIR)

# all: 编译模块
#   -C $(KERNEL_DIR)  先切到内核构建树，借用内核那套 Makefile 规则
#   M=$(PWD)          再让内核「回到我的目录」来找源码、放产物
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# clean: 删除 .o / .ko / Module.symvers 等产物
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
