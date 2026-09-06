# 内核环形缓冲区（log_buf）：容量、覆盖与持久化

> 真机环境：树莓派 5，Debian 13 (trixie)，内核 `6.18.34+rpt-rpi-2712`
> 本文所有数字均为真机实测，非理论推导。配套实验（**C 实现**）：
> `02-log-levels/experiments/log-buffer-fill/rbfill.c`

## 一句话答案

`printk` 写进内核内存里的一块**定长环形缓冲区**（`__log_buf`）。它有四个硬性质：

1. **容量固定** —— 实测这台机器 **128 KiB**，启动时分配，运行期不可变。
2. **不落盘** —— 内核自己从不写磁盘文件，重启即丢。
3. **满则覆盖** —— 新消息挤掉最旧的消息，静默丢弃，没有任何报错。
4. **用户态擦不掉** —— `dmesg -C` 只挪游标，不删记录、不腾空间（见 §4）。

---

## 1. 容量怎么查

```sh
# 编译期配置（决定默认值）
$ grep CONFIG_LOG_BUF_SHIFT /boot/config-$(uname -r)
CONFIG_LOG_BUF_SHIFT=17
```

换算公式：**容量 = 2^CONFIG_LOG_BUF_SHIFT 字节**

| CONFIG_LOG_BUF_SHIFT | 容量 |
|---|---|
| 17（本机实测、也是多数发行版的默认） | **128 KiB** |
| 18 | 256 KiB |
| 20 | 1 MiB |
| 24 | 16 MiB |

在 C 程序里可以直接问内核，比读 config 文件可靠：

```c
int sz = klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0);   /* 本机返回 131072 */
```

### 改容量：内核启动参数 `log_buf_len`

```sh
# /boot/firmware/cmdline.txt（树莓派）或 GRUB_CMDLINE_LINUX（x86）
log_buf_len=16M
```

- 值会被**向上取整到 2 的幂**，写 `10M` 实际得到 16 MiB
- 必须是**早期参数**，模块加载时改不了，运行期完全不可变
- 代价：启动即预留物理内存，且 `dmesg` 一次输出会非常长

> ⚠️ **常见误解：以为默认有几 MB。**
> 实测 `CONFIG_LOG_BUF_SHIFT=17` → **131072 字节 = 128 KiB**，比"几 MB"小两个数量级。
> 后果有多严重见 §5 —— 这台机器**开机 10 秒内启动日志就被自己冲光了**。

---

## 2. 关键换算：128 KiB 到底能装多少条？

缓冲区按**文本字节数**计费，不是按条数。实测（把缓冲区刷成 100% 同一规格的消息后读取）：

| 项 | 实测值 |
|---|---|
| 单条消息文本 | **214 字节**（`"RBPURE "` 7 + 5 位序号 + 1 空格 + 200 载荷 + 1 换行） |
| 稳态容纳条数 | **585 条** |
| 585 × 214 B | 125 190 B ≈ 122.3 KiB |
| 缓冲区标称容量 | 131 072 B = 128 KiB |
| 差额（描述符 + 对齐开销） | 5 882 B（**4.5%**，每条摊约 10 字节） |
| 纯文本可用率 | **95.5%** |

```
    标称 128 KiB ───────────────────────────────────────┐
    ├── 描述符/对齐开销 4.5% ──┤├── 纯文本 95.5% (122.3 KiB) ──┤
```

**推论**（按 585 条 / 214 B 反推）：

| 单条长度 | 约可容纳条数 |
|---|---|
| 50 B（短 printk） | ≈ 2400 条 |
| 214 B（本实验） | **585 条（实测）** |
| 1 KiB（带大段 hexdump） | ≈ 125 条 |

> kernel 5.10+ 的 record-based ring buffer **没有** 1024 字节的块对齐填充 ——
> 若按 1 KiB/条计，585 条需要 570 KiB，是容量的 4 倍多，早该溢出，
> 实测却能装下。每条只额外摊 ~10 B。

---

## 3. 覆盖行为实测

`rbfill.c` 往 `/dev/kmsg` 写定长消息，每 50 条采样一次缓冲区：

```
已写入 记录总数 RBPURE数  最老RBPURE 首条记录(最老)
----------------------------------------------------------------------------
  基线     585      -        -        RBFLLB 00116 xxxx...
   50      585     50      00001     RBFLLB 00166 xxxx...   ★覆盖开始
  550      585    550      00001     RBFLLB 00666 xxxx...
  600      585    585      00016     RBPURE 00016 xxxx...   ← 上一轮残留清空
  700      585    585      00116     RBPURE 00116 xxxx...
```

**注意记录总数从头到尾是 585，一点没变** —— 因为缓冲区**一开始就是满的**。
真正能看出覆盖的是「首条记录」那一列：标签从 `RBFLLB` 变成 `RBPURE`，
编号一路往前推 —— **这就是覆盖边界在缓冲区里滚动的样子**。

### 覆盖检测：看 seq，不要看条数

| 判据 | 什么时候有用 | 问题 |
|---|---|---|
| 记录数回落 | 只在"从不满到满"的那一瞬间 | 缓冲区通常一开始就满了，看不到 |
| **最老记录 seq 前进** | **任何时候** | 无 |

`seq` 是内核给每条记录的全局单调递增编号。seq 前进多少 = 被挤掉多少条。
`rbfill.c` 就是这么做的：

```c
if (st->oldest_seq > base.oldest_seq)   /* 覆盖开始了 */
    evicted = st->oldest_seq - base.oldest_seq;
```

---

## 4. ⚠️ `dmesg` 看到的 ≠ 缓冲区真实内容

这是本次实测最大的发现，也是必须用 C 才能测准的原因。

### 两条读取路径

| 读取方式 | 看到的是什么 |
|---|---|
| **`/dev/kmsg`** | 从缓冲区**最老一条存活记录**开始，全部内容 |
| **`dmesg` / `klogctl(2)`** | 走 `syslog(2)` 的游标 `syslog_seq`，只看**游标之后**的新消息 |

### 决定性实测

```
              首条 seq   /dev/kmsg 条数   dmesg 条数
清除前          1235          619             1
sudo dmesg -C   ↓  (返回码 0)
清除后(立刻)    1235          619             0     ← 只有 dmesg 变空了
清除后(2 秒后)  1235          619             0
```

**`syslog_clear()` 只是把 `syslog_seq` 推到 `log_next_seq`，一条记录都没删。**

所以：

- `dmesg -C` 之后 `/dev/kmsg` 照样读出全部旧记录
- **不能用 `dmesg -C` 腾地方** —— 旧记录照样占着空间，新消息照样挤它们
- **不能用 `dmesg | wc -l` 判断缓冲区占用率** —— 那是"游标之后还剩几条没读"

> 想从空缓冲区开始观察增长，**只能重启**。用户态没有任何擦除手段。

---

## 5. 一个真实案例：启动日志去哪了？

这台机器 `uptime` 显示开机 **7 天**，但：

```sh
$ dmesg | head -1
[ 5739.296903] log_levels: module loaded     ← 最早只到 5739 秒

$ journalctl -k -o short-monotonic | head -1
[    2.519926] Booting Linux on physical CPU 0x0000000000   ← 2.5 秒的启动消息
```

### 第一次的错误推断（记录在这里作反面教材）

我一开始量到"缓冲区只有 57 行 / 4778 B，占用率 3.6%"，
据此推断"没满就不会覆盖，所以是被 `dmesg -c` 清掉的"。

**这个推断是错的** —— 那 57 行是 `dmesg` 给的游标视角，不是缓冲区占用（见 §4）。
用游标视角去推断缓冲区状态，前提就不成立。

### 正确的答案

从 journald 统计开机阶段产生了多少条内核消息：

```
开机 10 秒内 : 633 条
开机 60 秒内 : 635 条
7 天累计     : 4257 条
缓冲区稳态容量: 585 条   ← 注意这个数
```

**开机 10 秒内就产生了 633 条，已经超过缓冲区的 585 条容量。**

→ 启动日志**在开机 10 秒内就被自己冲光了**，跟有没有人跑 `dmesg -c` 毫无关系。

### 这个数字意味着什么

| 场景 | 后果 |
|---|---|
| 想查开机阶段的驱动探测失败原因 | `dmesg` 里早就没了，只能靠 `journalctl -b` |
| 内核模块 printk 太多太快 | 几百条就能把之前的调试输出冲掉 |
| Oops / panic 现场 | 触发时伴随大量消息，最前面的关键信息可能已被覆盖 |

### `dmesg -c` 仍然是陷阱

虽然它不是这次的元凶，但危险没变：

| 命令 | 行为 | 危险度 |
|---|---|---|
| `dmesg` | 只读取，游标推进 | 安全 |
| `dmesg -c` | **读完立刻推进游标** | ⚠️ 一个字母之差，读一次就没了 |
| `dmesg -C` | 只推进游标，不打印 | ⚠️ 需 sudo |
| `dmesg -w` | 实时跟随（不清） | 安全 |
| `dmesg -W` | 只跟随**新**消息 | 安全 |

> 排查内核问题时**永远不要**用 `dmesg -c`。要"读完重来"就用 `dmesg -C`，
> 而且先确认 `journalctl -k` 能查到同样的内容。

---

## 6. 持久化真相：内核不落盘，journald 才落盘

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

| 常见说法 | 本机实测 |
|---|---|
| "rsyslog 会把内核日志拷到 `/var/log/dmesg`、`/var/log/kern.log`" | ❌ **rsyslog 未运行，这两个文件都不存在** |
| 真正的落盘者 | **systemd-journald** → `/var/log/journal`（实测占 8 MiB） |

> `/var/log/dmesg` 是 **sysvinit 时代**由 boot 脚本 `bootmisc.sh` 生成的。
> 在 systemd 机器上它默认**不存在**。别依赖它。

### journald 是 dmesg 的安全网（实测）

本次实验往缓冲区灌了 2400 条测试消息（4 轮 × 600/700），然后 `dmesg -C`：

```sh
$ sudo dmesg -C
$ dmesg | wc -l
0                                    ← 游标推到底，dmesg 空了

$ journalctl -k | grep -c RBPURE
700                                  ← 一条不少，全在 journald 里
```

**结论：推进 dmesg 游标不会丢数据，因为 journald 在消息进缓冲区时就同步抄走了一份。**

```
        printk
          │
          ▼
   ┌─────────────┐   同步抄送    ┌──────────────────────┐
   │  ring buffer │ ───────────▶ │ systemd-journald      │
   │  128 KiB     │              │ /var/log/journal      │
   │  重启即丢     │              │ 持久化，可翻历史       │
   └─────────────┘              └──────────────────────┘
        │
        ├─ /dev/kmsg  → 真实全部内容（从最老存活记录）
        └─ dmesg      → 只有游标之后的
```

---

## 7. 实用手段速查

```sh
# 实时跟随（调模块最常用；走 /dev/kmsg，不受游标影响）
dmesg -W                  # 只跟新增
dmesg -w                  # 先重放再跟随
journalctl -k -f          # 从 journald 跟随（推荐，能看到历史）

# 导出到磁盘（手动持久化）
dmesg > my_log.log
dmesg --ctime > my_log.log        # 带人类可读时间

# 从 journald 查内核日志（推荐，可查历史启动）
journalctl -k                     # 本次启动
journalctl -k -b -1               # 上一次启动
journalctl -k --since "10 min ago"
journalctl -k -o short-monotonic  # 用内核单调节时间戳，便于和 dmesg 对齐

# 控制 console 输出级别（不影响缓冲区内容）
cat /proc/sys/kernel/printk        # 本机: 3 4 1 3
sudo sh -c 'echo 8 > /proc/sys/kernel/printk'   # 全级别都往 console 吐

# 推进 dmesg 游标（注意：不擦记录、不腾空间）
sudo dmesg -C
```

### 权限相关

```sh
$ cat /proc/sys/kernel/dmesg_restrict
0        # 0 = 任何用户可读 dmesg；1 = 需 CAP_SYSLOG（sudo）
```

---

## 8. 与模块命令的对应关系

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

## 9. 四条记忆点

| # | 结论 | 本机实测数据 |
|---|---|---|
| 1 | 缓冲区**很小**，不是几 MB | **128 KiB**（`CONFIG_LOG_BUF_SHIFT=17`） |
| 2 | 换算成条数更直观 | **约 585 条**短 printk 就满（单条 214 B） |
| 3 | 满则**静默覆盖**，无任何报错 | 边界像履带滚动；检测要看最老记录的 **seq** |
| 4 | **`dmesg` 看到的 ≠ 缓冲区内容** | `dmesg -C` 只挪游标，619 条记录一条没少 |

外加一条：内核**不落盘**，journald 才是持久化层（rsyslog 在这台机器上是关的）。

---

## 相关

- `02-log-levels/docs/printk-output-path.md` —— printk 输出链路完整解析
- `02-log-levels/experiments/log-buffer-fill/` —— 填灌实验（**C 版**：`rbfill.c`）
- `02-log-levels/experiments/console-path/` —— 终端形态对照实验（**C 版**：`run_console.c` / `run_pty.c`）
