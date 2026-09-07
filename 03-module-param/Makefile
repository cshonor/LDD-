# 目标模块名（不能带 .c 后缀）
obj-m := param_demo.o

# 指向当前运行内核的头文件树。KDIR 末尾的 `build` 是一个符号链接，
# 指向 /usr/src/linux-headers-.../ 下的构建配置。
KDIR ?= /lib/modules/$(shell uname -r)/build

# 默认目标：进入内核源码/头文件树，用 kbuild 编译当前目录下的模块。
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# 清理所有 kbuild 生成的中间文件
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

# 一键：编译 + 加载 + 看日志（带参数）
# 注意 insmod 的模块参数必须放在 .ko 之后，写反了会被当成 insmod 自己的选项。
load: all
	sudo insmod param_demo.ko my_int=10 my_bool=1 my_str="hello" my_arr=10,20,30
	sudo dmesg | tail -12

unload:
	sudo rmmod param_demo
	sudo dmesg | tail -12
