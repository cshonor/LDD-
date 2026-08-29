# 实验用 Makefile：用变量切换要构建的模块
#
#   make MOD=nolic                                                    # 预期：构建失败
#   sed 's|@LIC@|GPL|g' gplonly.tmpl.c > gplonly.c && make MOD=gplonly # 预期：构建成功
#
# 坑记录：kbuild 二次进入源码目录时固定读名为 Makefile 的文件，
#         所以 `make -f 其他文件名` 不生效（obj-m 读不到），只能用变量切换。

MOD ?= nolic

obj-m += $(MOD).o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
	rm -f gplonly.c
