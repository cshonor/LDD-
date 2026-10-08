# 实验：直接解引用用户指针（deref-direct）

> ⏳ **待真机验证**。预期结论见下，实际输出跑通后回填本节。

## 验证什么

第 1 节说"内核态直接解引用用户指针会死"，理由是缺页 / 安全 / PAN 三层。
本实验在 ARM64 + PAN 的 Pi 5 上**亲眼验证**：同一个 misc 字符设备，
`read` 里唯一的区别是把 `copy_to_user` 换成直接 `*ubuf`。

对照组（安全版）：[04-char-block-net/char/misc_echo.c](../../../04-char-block-net/char/misc_echo.c)。

## ⚠️ 危险警告

- 预期触发 **oops**，发起 `cat` 的进程会被杀；
- 内核 taint 会置 **`D`** 位（kernel died recently），**只有重启才能清除**；
- 实验前先确认 `/proc/sys/kernel/panic_on_oops` 为 `0`——为 `1` 时一个 oops 就整机 panic：
  ```bash
  cat /proc/sys/kernel/panic_on_oops     # 应为 0
  ```

## 步骤

```bash
cat /proc/sys/kernel/panic_on_oops   # ① 确认是 0
cat /proc/sys/kernel/tainted         # ② 记录实验前 taint 值
make
sudo insmod deref.ko
cat /dev/deref                       # ③ 预期：这里就死了
sudo dmesg | tail -30                # ④ 看 oops 全文
cat /proc/sys/kernel/tainted         # ⑤ 对比：应多出 128（D 位）
sudo rmmod deref
```

## 预期输出（待真机回填）

- `cat /dev/deref`：进程被杀（`Killed`），终端返回 shell；
- dmesg 依次出现：
  - `deref: about to dereference user pointer ...`（模块最后一句话）
  - `Unable to handle kernel ... at virtual address`（PAN 下应为 permission fault 类）
  - 完整 oops 栈（`deref_read` 应在栈里）
- `/proc/sys/kernel/tainted` 比实验前多 **128**（bit 7 = `D`）；
- 若 `cat` 竟然读到数据（打印 `survived?!`）——说明本内核未启用 PAN，
  同样是有效结论，需改写主文档第 1.3 节的表述。

## 清理

oops 后模块通常仍可 `rmmod`（read 返回路径被 kill，引用计数由进程退出路径回收）。
taint 的 `D` 位粘性，重启归零。
