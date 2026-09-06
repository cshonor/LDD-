# console-path 实验的用户态 C 程序
#
#   make            # 编译 run_console / run_pty
#   make clean
#   make run        # 编译并直接跑 run_console（需要 root）
#
# 注意：这是普通用户态程序，不走 kbuild（obj-m），
#       直接用 gcc 编译即可。只有 02-log-levels/ 主目录的 Makefile
#       才是编译内核模块用的。

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS  += -lutil          # forkpty / openpty

PROGS = run_console run_pty
MOD  ?= /home/wzp/LDD-/02-log-levels/log_levels.ko

.PHONY: all clean run run-pty

all: $(PROGS)

run_console: run_console.c
	$(CC) $(CFLAGS) -o $@ $<

run_pty: run_pty.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: run_console
	sudo ./run_console $(MOD)

run-pty: run_pty
	sudo ./run_pty $(MOD)

clean:
	rm -f $(PROGS)
