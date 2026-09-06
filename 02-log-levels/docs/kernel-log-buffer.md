# 内核环形缓冲区（log_buf）：容量、覆盖与持久化

> 真机环境：树莓派 5，Debian 13 (trixie)，内核 `6.18.34+rpt-rpi-2712`
> 本文所有数字均为真机实测，非理论推导。配套实验脚本：
> `02-log-levels/experiments/log-buffer-fill/rbfill.sh`

## 一句话答案

`printk` 写进内核内存里的一块**定长环形缓冲区**（`__log_buf`）。它有三个硬性质：

1. **容量固定** —— 实测这台机器 **128 KiB**，启动时分配，运行期不可变。
2. **不落盘** —— 内核自己从不写磁盘文件，重启即丢。
3. **满则覆盖** —— 新消息挤掉最旧的消息，静默丢弃，没有任何报错。

---

## 1. 容量怎么查

```sh
# 编译期配置（决定默认值）
$ grep CONFIG_LOG_BUF_SHIFT /boot/config-$(uname -r)
CONFIG_LOG_BUF_SHIFT=17

# 运行期是否用启动参数改过
$ cat /proc/cmdline | grep -o 'log_buf_len=[^ ]*'
（无输出 = 没改，用默认值）
```

换算公式：**容量 = 2^CONFIG_LOG_BUF_SHIFT 字节**

| CONFIG_LOG_BUF_SHIFT | 容量 |
|---|---|
| 17（本机实测、也是多数发行版的默认） | **128 KiB** |
| 18 | 256 KiB |
| 20 | 1 MiB |
| 24 | 16 MiB |

> ⚠️ **常见误解：以为默认有几 MB。**
> 实测 `CONFIG_LOG_BUF_SHIFT=17` → **131072 字节 = 128 KiB**，远小于"几 MB"。
> 这是调 `printk` 调试时最容易踩的认知偏差：你以为还很空，其实早就滚过去了。

### 改容量：内核启动参数 `log_buf_len`

```sh
# /boot/firmware/cmdline.txt（树莓派）或 GRUB_CMDLINE_LINUX（x86）
log_buf_len=16M
```

注意点：

- 值会被**向上取整到 2 的幂**，写 `log_buf_len=10M` 实际得到 16 MiB。
- 必须是**早期参数**（boot param），模块加载时改不了，运行期完全不可变。
- 代价：启动即预留，占用物理内存，且 `dmesg` 一次输出会非常长。

---

## 2. 关键换算：128 KiB 到底能装多少条？

这是最容易误判的地方。缓冲区按**文本字节数**计费，不是按条数。实测：

| 项 | 实测值 |
|---|---|
| 每条消息文本长度 | 213 字节（`"RBFILL 00117 "` 13 B + 200 B 载荷） |
| 溢出前最多容纳条数 | **584 条** |
| 584 × 213 B | 124 392 B ≈ 121.5 KiB |
| 缓冲区标称容量 | 131 072 B = 128 KiB |
| 差额（描述符 + 对齐开销） | ≈ 4 KiB（约 3%） |

```
    标称 128 KiB ─────────────────────────────────┐
    ├── 描述符/对齐开销 ~4 KiB ──┤├── 文本 121.5 KiB ──┤
```

**推论：** 单条消息越短，能存的条数越多。满打满算 128 KiB 大约能放：

| 单条长度 | 约可容纳条数 |
|---|---|
| 50 B（短 printk） | ≈ 2500 条 |
| 213 B（本实验） | **584 条（实测）** |
| 1 KiB（带大段 hexdump） | ≈ 120 条 |

> 内核的 record-based ring buffer（5.10+ 默认）**没有** 1024 字节的块对齐填充 ——
> 本实验 584 条短消息若按 1 KiB/条计需要 570 KiB，远超 128 KiB 却没溢出，
> 证明文本是紧凑排布的。

---

## 3. 覆盖行为实测

脚本：往 `/dev/kmsg` 持续写入，每批观察一次缓冲区状态。

```
已写入  缓冲区行数  RBFILL存活  首行
------------------------------------------------------------
  基线      57         -        [ 5739.296903] log_levels: module loaded
   250     607       550        [ 5739.296903] log_levels: module loaded   ← 还没满
   275     594       575        [58579.736998] input: INSTANT USB GAMING   ← ★覆盖开始
   300     584       584        [665415.594403] RBFILL 00017
   325     584       584        [665415.838555] RBFILL 00042
   400     584       584        [665416.589487] RBFILL 00117
```

**读出三个结论：**

1. **溢出前**：行数线性增长（57 → 607），每条新消息都追加。
2. **溢出瞬间**：行数**回落**（607 → 594）—— 因为新消息挤掉了更旧的，净增为负。
   同时最老的那条 `log_levels: module loaded`（7 天前的）**被静默丢弃**。
3. **稳态**：行数**锁死在 584**，此后每写一条就丢一条，缓冲区像履带一样往前滚。

```
   覆盖前                          覆盖后（稳态）
   ┌──────────────────────┐       ┌──────────────────────┐
   │ 旧 ... 新          空余│       │新 旧 ... 新      极少量空│
   └──────────────────────┘       └──────────────────────┘
     head→              ←tail       head→              ←tail
     追加不丢数据                    每写 1 条 → 挤掉 head 处 1 条
```

> **判别技巧**：`dmesg | head -1` 如果**不是**启动阶段的消息，
> 就说明缓冲区已经滚过（或被清空过）。

---

## 4. 持久化真相：内核不落盘，journald 才落盘

这是你笔记里最需要修正的一条。

### 内核侧：从不写文件

`printk` 只写内存缓冲区，内核代码里没有任何落盘路径。重启 = 全丢。

### 用户侧：谁在搬运？

本机实测：

```sh
$ ls /var/log/dmesg /var/log/kern.log /var/log/syslog /var/log/messages
/var/log/dmesg     不存在
/var/log/kern.log  不存在
/var/log/syslog    不存在
/var/log/messages  不存在

$ systemctl is-active rsyslog
inactive            ← rsyslog 根本没跑

$ systemctl is-active systemd-journald
active              ← 真正在干活的
```

| 你的笔记 | 本机实测 |
|---|---|
| "rsyslog 会把内核日志拷到 `/var/log/dmesg`、`/var/log/kern.log`" | ❌ **rsyslog 未运行，这两个文件都不存在** |
| 真正的落盘者 | **systemd-journald** → `/var/log/journal`（实测占 8 MiB） |

> `/var/log/dmesg` 是 **sysvinit 时代**由 boot 脚本 `bootmisc.sh` 生成的。
> 在 systemd 机器上它默认**不存在**。别依赖它。

### journald 是 dmesg 的安全网（实测）

本次实验往缓冲区灌了 700 条测试消息，然后 `dmesg -C` 清空：

```sh
$ sudo dmesg -C
$ dmesg | wc -l
0                                    ← 缓冲区空了

$ journalctl -k | grep -c RBFILL
700                                  ← 一条不少，全在 journald 里
```

**结论：清空 dmesg 不会丢数据，因为 journald 已经在消息进缓冲区时同步抄走了一份。**

```
        printk
          │
          ▼
   ┌─────────────┐   同步抄送    ┌──────────────────────┐
   │  ring buffer │ ───────────▶ │ systemd-journald      │
   │  128 KiB     │              │ /var/log/journal      │
   │  重启即丢     │              │ 持久化，可翻历史       │
   └─────────────┘              └──────────────────────┘
```

---

## 5. 一个真实案例：启动日志去哪了？

本机 `uptime` 显示已开机 **7 天**，但：

```sh
$ dmesg | head -1
[ 5739.296903] log_levels: module loaded     ← 最早只到 5739 秒

$ journalctl -k -o short-monotonic | head -1
[    2.519926] Booting Linux on physical CPU 0x0000000000  ← 2.5 秒的启动消息还在
```

**启动消息在 dmesg 里没了，但 journald 里还在。**

### 是被覆盖掉的吗？不是

当时缓冲区状态：

| 项 | 值 |
|---|---|
| 缓冲区行数 | 57 行 |
| 内容字节数 | 4 778 B |
| 容量 | 131 072 B |
| **占用率** | **≈ 3.6%** |

**缓冲区远没满** —— 满了才会覆盖，所以启动消息**不是被挤掉的**。

→ 剩下的唯一解释：有人执行过 `dmesg -c` 或 `dmesg -C` 主动清空过。

### `dmesg -c` 是个陷阱

| 命令 | 行为 | 危险度 |
|---|---|---|
| `dmesg` | 只读取，缓冲区不变 | 安全 |
| `dmesg -c` | **读完立刻清空** | ⚠️ 一个字母之差，读一次就没了 |
| `dmesg -C` | 只清空，不打印 | ⚠️ 需 sudo |
| `dmesg -w` | 实时跟随（不清空） | 安全 |
| `dmesg -W` | 只跟随**新**消息（跳过已存在的） | 安全 |

> 排查内核问题时**永远不要**用 `dmesg -c`。要"读完重来"就用 `dmesg -C`，
> 而且先确认 `journalctl -k` 能查到同样的内容。

---

## 6. 实用手段速查

```sh
# 实时跟随（调模块最常用）
dmesg -w

# 只看新增，不刷历史
dmesg -W

# 导出到磁盘（手动持久化）
dmesg > my_log.log
dmesg --ctime > my_log.log        # 带人类可读时间

# 从 journald 查内核日志（推荐，可查历史启动）
journalctl -k                     # 本次启动
journalctl -k -b -1               # 上一次启动
journalctl -k --since "10 min ago"
journalctl -k -f                  # 跟随

# 控制 console 输出级别（不影响缓冲区内容）
cat /proc/sys/kernel/printk        # 本机: 3 4 1 3
sudo sh -c 'echo 8 > /proc/sys/kernel/printk'   # 全级别都往 console 吐

# 清空（需 CAP_SYSLOG）
sudo dmesg -C
```

### 权限相关

```sh
$ cat /proc/sys/kernel/dmesg_restrict
0        # 0 = 任何用户可读 dmesg；1 = 需 CAP_SYSLOG（sudo）
```

---

## 7. 与模块命令的对应关系

```
insmod xxx.ko  ─┐
                ├─▶ 模块内的 printk() ──▶ ring buffer ──▶ dmesg 可见
rmmod xxx    ─┘                              │
                                             └─▶ journald ──▶ 永久留存
```

实测（本机）：

```
[665508.805862] log_levels: module loaded
[665508.805875] log_levels: EMERGENCY   - system unusable
...
[665508.805894] log_levels: DEBUG       - debug-level message
[665509.843530] Hello: Hello, Kernel
[665510.868840] Hello: Goodbye, Kernel
[665510.906492] log_levels: module unloaded
```

⚠️ 注意：`/proc/sys/kernel/printk = 3 4 1 3` 表示 console_loglevel=**3**，
所以 8 个级别里只有 **EMERG / ALERT / CRIT** 会出现在屏幕上，
但 **dmesg 能看到全部 8 条** —— 因为 dmesg 直接读缓冲区，不经过 console 过滤。

---

## 8. 三条记忆点

| # | 结论 | 本机实测数据 |
|---|---|---|
| 1 | 缓冲区**很小**，不是几 MB | **128 KiB**（`CONFIG_LOG_BUF_SHIFT=17`） |
| 2 | 内核**不落盘**，journald 才是持久化层 | rsyslog 未运行，`/var/log/kern.log` 不存在；journald 占 8 MiB |
| 3 | 满则**静默覆盖**，无任何报错 | 稳态锁死 584 条，每写 1 条丢 1 条 |

---

## 相关

- `02-log-levels/docs/printk-output-path.md` —— printk 输出链路完整解析
- `02-log-levels/experiments/log-buffer-fill/` —— 本文的填灌实验脚本与原始输出
