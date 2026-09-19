# 顶层总控：内核模块在各自子目录构建，用户态程序统一编译
# 注意：目标名与同名目录冲突时，GNU make 会把目录当"已存在的目标"跳过，
# 必须 .PHONY 声明（Pi 实测踩过）
.PHONY: all char block userspace clean

all: char block userspace

char:
	$(MAKE) -C char

block:
	$(MAKE) -C block

userspace:
	$(CC) char/test_char.c -o char/test_char
	$(CC) net/sniff.c -o net/sniff

clean:
	$(MAKE) -C char clean
	$(MAKE) -C block clean
	rm -f char/test_char net/sniff
