# 实验：printk 到底流向哪里？

验证「printk 不会自动出现在 SSH 终端」这一结论，并摸清内核日志的完整输出链路。

结论文档见：[../../printk-output-path.md](../../printk-output-path.md)

## 文件说明

| 文件 | 作用 |
|---|---|
| `run_console.py` | 两个并发 SSH 连接：A 后台 `dmesg -W` 实时跟随，B 加载模块。证明消息确实产生且可实时捕获 |
| `run_pty.py` | 三种终端形态对照（无 pty / 有 pty / 交互式 shell），证明**没有任何一种**会自动显示 printk |

## 运行方式

需要 Python `paramiko`（Windows 上 OpenSSH 无法命令行传密码，只能用 paramiko）：

```bash
pip install paramiko
```

**凭据通过环境变量传入**（脚本不硬编码密码，因为本仓库是公开的）：

```bash
# Linux / macOS / Git Bash
export RPI_PASSWORD='你的密码'
python run_console.py      # 实验 1：实时捕获
python run_pty.py          # 实验 2：终端形态对照

# Windows PowerShell
$env:RPI_PASSWORD='你的密码'
python run_console.py
```

可选环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `RPI_HOST` | `192.168.31.109` | 树莓派地址 |
| `RPI_USER` | `wzp` | 登录用户 |
| `RPI_PASSWORD` | （无，必填） | 登录密码 |
| `RPI_MODDIR` | `/home/wzp/linux-device/02-log_levels` | 远端模块目录 |

不设 `RPI_PASSWORD` 时脚本会直接退出并提示，不会用空密码去连。

## 预期结果

### run_console.py

```
=== 连接 B：sudo insmod log_levels.ko ===
insmod 命令自身输出: ''
  → SSH 终端没有自动冒出任何内核消息（命令返回为空）

=== 连接 A 实时捕获到的新消息 ===
[dmesg-W] [ 5153.653031] log_levels: module loaded
[dmesg-W] [ 5153.653042] log_levels: EMERGENCY   - system unusable
...（共 10 条）
```

### run_pty.py

三组终端形态（无 pty / 有 pty / 交互式 shell）**都不显示**内核消息，
但收尾时 `dmesg | grep log_levels` 能查到消息确实产生了。

## 复现时的注意事项

1. **需要 `dmesg -W` 而不是 `dmesg -w`**：`-W` 只跟随新消息，`-w` 会先把整个
   缓冲区重放一遍，容易淹没目标消息。
2. **必须加 `stdbuf -oL`**：`dmesg` 输出到管道时是全缓冲，不加行缓冲会一直收不到数据。
3. **`cat /dev/kmsg` 会从头重放整个缓冲区**（包含所有开机日志），不适合做实时捕获。
4. **`head -c N /dev/kmsg` 会报 `Invalid argument`**：`/dev/kmsg` 是
   record-oriented 设备，必须按整条记录读。
5. **C 组（交互式 shell）的判断容易误判**：命令行回显里包含路径字符串
   （如 `cd /home/wzp/linux-device/02-log_levels`），用 `grep log_levels`
   检测会命中回显而非内核消息。要看终端实际收到了什么。

## 补充验证命令（手动执行）

```bash
# 内核启动参数里的控制台
cat /proc/cmdline | tr ' ' '\n' | grep console

# 已注册的控制台（带 C 的是 /dev/console）
cat /proc/consoles

# dmesg 的数据来源（需要 strace）
sudo strace -e trace=openat dmesg 2>&1 | grep kmsg

# ring buffer 大小
grep -rE 'LOG_BUF_SHIFT' /usr/src/linux-headers-$(uname -r)/.config

# 往控制台写（会显示在 HDMI 屏幕上，SSH 里看不到）
echo 'TEST' | sudo tee /dev/console
```
