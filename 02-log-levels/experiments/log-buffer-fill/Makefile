# log-buffer-fill 实验的用户态 C 程序
#
#   make            # 编译 rbfill
#   make clean
#   make run        # 跑一轮 300 条 / 每 16 条观察（需要 root）
#   make run-wrap   # 跑到大溢出（在 run 之后接着跑，更容易看到覆盖）
#
# 这是普通用户态程序，不走 kbuild。

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11

PROG = rbfill

.PHONY: all clean run run-wrap

all: $(PROG)

$(PROG): $(PROG).c
	$(CC) $(CFLAGS) -o $@ $<

# 第一轮：观察线性增长
run: $(PROG)
	sudo ./$(PROG) 300 16 200

# 第二轮：接着灌，直到溢出并进入稳态
run-wrap: $(PROG)
	sudo ./$(PROG) 400 25 200
	@echo ""
	@echo "清理现场:   sudo dmesg -C"
	@echo "验证 journald 仍留有记录:   journalctl -k | grep -c RBFILL"

clean:
	rm -f $(PROG)
