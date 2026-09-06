#!/bin/bash
#
# 内核 printk 环形缓冲区（log_buf）填灌实验
#
# 目的：实测 ring buffer 的容量上限（条数 vs 字节）、以及覆盖发生的确切时机。
# 原理：往 /dev/kmsg 持续写入定长消息，每批观察一次缓冲区状态。
#       溢出前行数线性增长；溢出瞬间行数回落；稳态行数锁死不变。
#
# 用法：
#   sudo ./rbfill.sh [总条数] [每批观察数] [每条载荷字节]
#
#   ./rbfill.sh 300 16 200    # 写 300 条，每 16 条观察一次，每条 200B 载荷
#
# 注意：
#   - 会往内核日志里灌入大量测试消息，可能挤掉已有日志。
#     执行前先确认 journalctl -k 能查到同样的内容（journald 是安全网）。
#   - 执行后可用 sudo dmesg -C 清理现场。
#   - 写入 /dev/kmsg 通常需要 root（CAP_SYS_ADMIN），脚本会自动降级尝试。
#
# 环境：树莓派 5 / Debian 13 / 内核 6.18.34+rpt-rpi-2712
#       CONFIG_LOG_BUF_SHIFT=17 → 131072 字节 = 128 KiB

set -u

N=${1:-300}        # 总写入条数
BATCH=${2:-16}     # 每多少条观察一次
PAY=${3:-200}      # 每条消息有效载荷字节数

PAD=$(printf 'x%.0s' $(seq 1 $PAY))

# 决定写入方式（/dev/kmsg 通常需要 root）
if [ -w /dev/kmsg ]; then
    emit() { printf '<4>%s\n' "$1" > /dev/kmsg; }
    MODE="普通用户直写"
else
    emit() { printf '<4>%s\n' "$1" | sudo tee /dev/kmsg >/dev/null; }
    MODE="sudo 写入"
fi

SHIFT=$(grep -oE '^CONFIG_LOG_BUF_SHIFT=[0-9]+' "/boot/config-$(uname -r)" 2>/dev/null | cut -d= -f2)
CAP=$(( 1 << ${SHIFT:-17} ))

echo "缓冲容量    : $CAP 字节 ($((CAP>>10)) KiB)，CONFIG_LOG_BUF_SHIFT=$SHIFT"
echo "写入方式    : $MODE"
echo "每条载荷    : $PAY 字节"
echo ""

BASE_L=$(dmesg | wc -l)
BASE_F=$(dmesg | head -1 | cut -c1-50)
echo "=== 基线：行数=$BASE_L  首行=[$BASE_F]"
echo ""
printf "%-8s %-10s %-10s %-10s %s\n" "已写入" "缓冲区行数" "RBFILL存活" "首条存活" "首行"
echo "------------------------------------------------------------------------"

for ((i=1; i<=N; i++)); do
    emit "RBFILL $(printf '%05d' $i) $PAD"
    if (( i % BATCH == 0 )); then
        L=$(dmesg | wc -l)
        S=$(dmesg | grep -c 'RBFILL')
        K=$(dmesg | grep -c 'RBFILL 00001 ')
        F=$(dmesg | head -1 | cut -c1-40)
        printf "%-8s %-10s %-10s %-10s %s\n" "$i" "$L" "$S" "$K" "$F"
    fi
done

echo ""
echo "=== 最终结果 ==="
echo "  缓冲区总行数        : $(dmesg | wc -l)"
echo "  dmesg 输出字节数    : $(dmesg | wc -c)"
echo "  RBFILL 存活条数     : $(dmesg | grep -c RBFILL)"
echo "  基线首行还在吗      : $(dmesg | grep -cF "$BASE_F")  (0=已被覆盖)"
echo "  最早存活的 RBFILL   : $(dmesg | grep 'RBFILL' | head -1 | cut -c1-40)"
echo "  最新存活的 RBFILL   : $(dmesg | grep 'RBFILL' | tail -1 | cut -c1-40)"
echo ""
echo "清理现场: sudo dmesg -C"
