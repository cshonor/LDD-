# 实验 01：MODULE_LICENSE 到底管什么

> 目的：用真机实测回答三个"教程里说法不一"的问题。
> 完整输出见 [`results.log`](./results.log)，本文件只讲怎么复现。

| 问题 | 老教程的常见说法 | 实测结论 |
|---|---|---|
| 不写 `MODULE_LICENSE` 会怎样？ | "只是 taint 警告" | ❌ 错。**≥ v5.16 直接构建失败** |
| `taints kernel` 一定是许可证问题吗？ | 混为一谈 | ❌ 错。**树外模块（bit 12）≠ 许可证（bit 0）** |
| `MODULE_LICENSE` 只是"元数据"吗？ | "不影响逻辑" | ❌ 错。**它决定你能用哪 57% 的内核符号** |

---

## 文件

| 文件 | 作用 |
|---|---|
| `nolic.c` | 故意不写 `MODULE_LICENSE` 的模块。**构建注定失败**，这就是实验一的目的 |
| `taint.tmpl.c` | 模板：普通 hello 模块，许可证用 `@LIC@` 占位，两种许可证都能编过 |
| `gplonly.tmpl.c` | 模板：引用 GPL-only 符号 `pm_power_off`，用来验证许可证字符串 |
| `Makefile` | 用 `MOD=` 变量切换要构建的模块 |
| `results.log` | 真机实测记录 |

---

## 怎么跑

```bash
# 实验一：不写 LICENSE（预期失败）
make MOD=nolic

# 实验二：同一份代码换许可证，对比 dmesg（需要重启后才能看到 taint 文案）
sed 's|@LIC@|GPL|g'         taint.tmpl.c > taint.c && make MOD=taint
sudo dmesg -c                 # 清空环形缓冲区，保证日志干净
sudo insmod taint.ko && sudo dmesg && cat /proc/sys/kernel/tainted
sudo rmmod taint

sed 's|@LIC@|Proprietary|g' taint.tmpl.c > taint.c && make MOD=taint
sudo dmesg -c
sudo insmod taint.ko && sudo dmesg && cat /proc/sys/kernel/tainted
sudo rmmod taint

# 实验三：许可证字符串 → 能否用 GPL-only 符号
for L in "GPL" "GPL v2" "GPL v2 or later" "GPLv2" "Proprietary"; do
    make clean >/dev/null
    sed "s|@LIC@|$L|g" gplonly.tmpl.c > gplonly.c
    printf '%-22s ' "$L"
    make MOD=gplonly >/dev/null 2>&1 && echo "✅ 构建成功" || echo "❌ 被 modpost 拒绝"
done
```

---

## 复现实验二的注意事项（重要）

taint 有两个"一次性"特性，会让复现结果看起来和文档不一致：

1. **粘性** —— 位一旦置上，`rmmod` 不清零，只有重启回到 0。
2. **每次开机只报一次** —— 内核打印 taint 文案的代码有 `if (!test_taint(...))` 守卫。

所以**要看到 `module license 'Proprietary' taints kernel.` 这行，必须先重启**。
不重启的话，`/proc/sys/kernel/tainted` 一直是上次的残留值，dmesg 里也不会有新文案。

---

## 速查

```
/proc/sys/kernel/tainted 位图
  bit  0 =     1   TAINT_PROPRIETARY_MODULE   非自由许可证
  bit 12 =  4096   TAINT_OOT_MODULE           树外模块（写 GPL 也躲不掉）
```

内核认定的 GPL 兼容字符串**只有 6 个**（`include/linux/license.h`）：

```
"GPL"    "GPL v2"    "GPL and additional rights"
"Dual BSD/GPL"    "Dual MIT/GPL"    "Dual MPL/GPL"
```

⚠️ `"GPL v2 or later"` 和 `"GPLv2"` **不在其中**。
