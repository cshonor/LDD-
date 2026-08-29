# 目标模块名（不能带 .c 后缀）
obj-m := log_levels.o

# 指向当前运行内核的头文件树。KDIR 末尾的 `build` 是一个符号链接，
# 指向 /usr/src/linux-headers-.../ 下的构建配置。
KDIR ?= /lib/modules/$(shell uname -r)/build

# 默认目标：进入内核源码/头文件树，用 kbuild 编译当前目录下的模块。
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# 清理所有 kbuild 生成的中间文件（.o, .ko, .mod, .mod.c, .mod.o, Module.symvers 等）
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
