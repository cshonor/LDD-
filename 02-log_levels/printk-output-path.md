# printk 到底打印到哪里？（输出链路完整解析）

> 真机环境：树莓派 5，Debian 13 (trixie)，内核 `6.18.34+rpt-rpi-2712`  
> 本文所有输出均为真机实测，非理论推导。

## 一句话答案

`printk` **不打印到任何文件，也不打印到你的终端**。它写进内核内存里的一块**环形缓冲区**（ring buffer），然后：

1. 任何进程都可以通过 `/dev/kmsg` 把这块缓冲区的内容读出来 —— 这就是 `dmesg` 的原理。
2. 内核**同时**把消息转发一份给"已注册的控制台"（比如 HDMI 屏幕、串口），但这一步受 `console_loglevel` 过滤。

所以你在 SSH 里 `insmod`，终端上什么都不会冒出来 —— 这是**设计如此**，不是没打印。

---

## 1. 完整链路图

```
                          printk(KERN_INFO "...")
                                   │
                                   ▼
              ┌────────────────────────────────────────┐
              │   内核 ring buffer（内存中的 __log_buf）  │
              │   树莓派实测大小：2^17 = 128 KiB          │
              │   所有级别的消息都进这里，无差别保留        │
              └────────────────────────────────────────┘
                    │                              │
       (A) 用户主动读取                    (B) 内核自动转发
          不受级别过滤                        受 console_loglevel 过滤
                    │                              │
                    ▼                              ▼
            /dev/kmsg（字符设备 1:11）      已注册的 console driver
                    │                              │
        ┌───────────┴───────────┐          ┌───────┴────────┐
        ▼                       ▼          ▼                ▼
     dmesg              systemd-journald   tty1 (4:1)   ttyAMA10 (204:74)
   （读 /dev/kmsg）      （读 /dev/kmsg）        │                │
        │                       │               ▼                ▼
        ▼                       ▼          HDMI 屏幕        串口 UART
   你的终端输出          journalctl -k      （/dev/console）   (115200 波特)
                                              ↑
                                    这是"控制台"，不是 SSH！
```

### 关键区分：三个容易混为一谈的概念

| 概念 | 是什么 | 树莓派上的实例 | printk 会往这发吗 |
|---|---|---|---|
| **ring buffer** | 内核内存中的日志缓冲区 | 128 KiB，所有消息都进 | ✅ 一定会 |
| **控制台 console** | 内核启动参数 `console=` 指定的设备 | `tty1`（HDMI）+ `ttyAMA10`（串口） | ✅ 会（受级别过滤） |
| **终端 / pts** | 你 SSH 登录后的伪终端 | `/dev/pts/0` | ❌ **永远不会** |

> **最核心的一句话**：SSH 的伪终端**不在** `/proc/consoles` 列表里，所以内核根本不知道它的存在，printk 不可能发过去。

---

## 2. 真机实测证据

### 2.1 树莓派上注册了哪些控制台

```bash
$ cat /proc/cmdline | tr ' ' '\n' | grep console
console=ttyAMA10,115200
console=tty1

$ cat /proc/consoles
tty1                 -WU (EC  p  )    4:1
ttyAMA10             -W- (E  Np a)  204:74
```

`/proc/consoles` 各字段含义：

| 字段 | 含义 |
|---|---|
| 名称 | 控制台设备名 |
| `-WU` / `-W-` | 读状态 / 写状态 / unsuspend 状态 |
| `E` | enabled：已启用 |
| `C` | **`/dev/console` 指向这个设备**（多个 console 里只有一个有 C） |
| `B` | boot console：早期启动控制台 |
| `p` | 需要把缓冲区积压的消息补打出来 |
| `N` | nbcon（新式的无锁 printk 控制台） |
| `a` | anytime：可在原子上下文安全使用 |

读法：树莓派有两个控制台 —— `tty1`（HDMI 屏幕，**且是 `/dev/console`**），`ttyAMA10`（串口，115200 波特）。

### 2.2 dmesg 的数据来自 /dev/kmsg（strace 铁证）

```bash
$ sudo strace -e trace=openat dmesg 2>&1 | grep kmsg
openat(AT_FDCWD, "/dev/kmsg", O_RDONLY|O_NONBLOCK) = 3
```

`dmesg` 打开的就是 `/dev/kmsg`。它没有任何魔法，纯粹是读这个字符设备。

### 2.3 /dev/kmsg 的原始格式

```bash
$ timeout 2 sudo cat /dev/kmsg | head -3
6,0,0,-;Booting Linux on physical CPU 0x0000000000 [0x414fd0b1]
5,1,0,-;Linux version 6.18.34+rpt-rpi-2712 (serge@raspberrypi.com) ...
6,2,0,-;KASLR enabled
```

格式是 `优先级,序列号,时间戳,标志;消息`：

| 字段 | 示例值 | 说明 |
|---|---|---|
| 优先级 | `6` | 就是 printk 级别（6 = KERN_INFO） |
| 序列号 | `0` | 单调递增的记录序号 |
| 时间戳 | `0` | 相对启动的微秒数（这里早期是 0） |
| 标志 | `-` | `c` 表示续行，`-` 表示新行 |
| 消息 | `;` 之后 | 正文 |

`dmesg` 只是把这些原始记录格式化成 `[ 1234.567890] 消息` 的样子而已。

> ⚠️ **坑**：`head -c 200 /dev/kmsg` 会报 `Invalid argument`。
> `/dev/kmsg` 必须按**整条记录**读（它是 record-oriented），不能读半个记录。`cat` 可以，`head -c` 不行。

### 2.4 SSH 终端不会自动显示 printk（三组对照）

用 `insmod log_levels.ko` 做实验，分别测试三种终端形态：

| 终端形态 | 模拟场景 | 终端是否收到 printk |
|---|---|---|
| A. 无 pty（`exec_command`） | 脚本里执行 | ❌ 只收到自己 echo 的 `--- 上面是 insmod 的输出 ---` |
| B. 有 pty（`get_pty=True`） | ssh 登录后敲命令 | ❌ 只收到 `MARKER_A` |
| C. 交互式 shell（`invoke_shell`） | 最接近真实终端 | ❌ 只回显命令本身 |

三组结果一致：**无论你用什么方式连上去，printk 都不会自己冒到终端上。**

但同一时刻，后台 `dmesg -W` 完整抓到了全部 10 条消息：

```
[dmesg-W] [ 5153.653031] log_levels: module loaded
[dmesg-W] [ 5153.653042] log_levels: EMERGENCY   - system unusable
[dmesg-W] [ 5153.653051] log_levels: ALERT       - action must be taken
[dmesg-W] [ 5153.653053] log_levels: CRITICAL    - critical condition
[dmesg-W] [ 5153.653054] log_levels: ERROR       - error condition
[dmesg-W] [ 5153.653055] log_levels: WARNING     - warning condition
[dmesg-W] [ 5153.653056] log_levels: NOTICE      - normal but significant
[dmesg-W] [ 5153.653057] log_levels: INFO        - informational
[dmesg-W] [ 5153.653058] log_levels: DEBUG       - debug-level message
[dmesg-W] [ 5153.653059] log_levels: all 8 levels printed, check which ones you can see
```

**结论**：消息确实产生了，也确实进了 ring buffer，只是不会主动推给你的 SSH 终端。

### 2.5 ring buffer 大小

```bash
$ grep -rE 'LOG_BUF_SHIFT' /usr/src/linux-headers-6.18.34+rpt-rpi-2712/.config
CONFIG_LOG_BUF_SHIFT=17
CONFIG_LOG_CPU_MAX_BUF_SHIFT=12
```

| 配置项 | 值 | 含义 |
|---|---|---|
| `CONFIG_LOG_BUF_SHIFT=17` | 2^17 = **128 KiB** | 主 ring buffer 大小 |
| `CONFIG_LOG_CPU_MAX_BUF_SHIFT=12` | 2^12 = 4 KiB | 每 CPU 额外缓冲上限 |

缓冲区满了会**循环覆盖最老的消息**。所以开机很久的机器，`dmesg` 里看不到最早期的启动日志。

### 2.6 往 /dev/console 写会去哪

```bash
$ echo 'HELLO_FROM_CONSOLE_TEST' | sudo tee /dev/console
写入 /dev/console 成功（请在接 HDMI 的屏幕上查看）
```

这条命令在 SSH 终端里**看不到任何回显**，因为它被送到了 `tty1`（HDMI 屏幕）。如果你给树莓派接了显示器，就能在屏幕上看到这行字。

### 2.7 日志会落盘吗？

```bash
$ ls -l /var/log/kern.log /var/log/syslog /var/log/messages
ls: cannot access '/var/log/kern.log': No such file or directory
ls: cannot access '/var/log/syslog': No such file or directory
ls: cannot access '/var/log/messages': No such file or directory

$ sudo systemctl is-active systemd-journald
active

$ sudo journalctl -k --no-pager -n 3
Aug 29 18:06:31 wzp kernel: log_levels: DEBUG       - debug-level message
Aug 29 18:06:31 wzp kernel: log_levels: all 8 levels printed, check which ones you can see
Aug 29 18:06:31 wzp kernel: log_levels: module unloaded
```

**树莓派 Debian 13 没有装 rsyslog**，所以传统的 `/var/log/kern.log` 不存在。内核日志由 `systemd-journald` 从 `/dev/kmsg` 读取后存进 journal，用 `journalctl -k` 查询。

> 重启后 `dmesg` 是空的（内存缓冲区没了），但 `journalctl -k` 还能查到上次的记录 —— 这就是持久化存储的差别。

---

## 3. 怎么看到 printk 的输出：四种方式

| 方式 | 命令 | 特点 |
|---|---|---|
| **一次性查看** | `sudo dmesg` | 读整个缓冲区，最常用 |
| **实时跟随** | `sudo dmesg -W` 或 `sudo dmesg -w` | 类似 `tail -f`，`-W` 只看新消息 |
| **原始格式** | `sudo cat /dev/kmsg` | 带优先级/序列号，会阻塞持续输出 |
| **持久化查询** | `sudo journalctl -k -f` | 查 journald 存的（含上次开机，`-f` 跟随） |

**推荐工作流**：开两个终端

```bash
# 终端 1：实时跟随
sudo dmesg -W | grep --line-buffered log_levels

# 终端 2：操作
sudo insmod log_levels.ko
sudo rmmod log_levels
```

> `grep` 记得加 `--line-buffered`，否则管道缓冲会让你以为"没输出"。

---

## 4. 常见误解

| # | 误解 | 真相 |
|---|---|---|
| 1 | printk 打印到文件里 | 先进内存 ring buffer；只有 journald/rsyslog 转存后才落盘 |
| 2 | 我在 SSH 里加载模块，屏幕上没东西 = 模块没跑 | 正常。必须用 `dmesg` 主动读 |
| 3 | 提高 console_loglevel 后 dmesg 能看到更多 | 不会。dmesg 读的是完整缓冲区，和 console_loglevel 无关；那个值只影响**控制台** |
| 4 | `/dev/console` 就是我当前的终端 | 不一定。它由内核启动参数 `console=` 决定，树莓派上是 `tty1`（HDMI） |
| 5 | 串口控制台和 HDMI 控制台看到的内容一样 | 基本一致，但早期启动阶段可能不同（取决于哪个先注册、是否有 `a` 标志） |
| 6 | dmesg 清空后消息就丢了 | 如果 journald 在跑，它已经把消息抄走了，`journalctl -k` 还能查到 |

---

## 5. HFT / 嵌入式关联

| 场景 | 要点 |
|---|---|
| **HFT 低延迟** | 生产环境把 `console_loglevel` 压到最低（甚至 `3 4 1 3` 以下）。因为**每次 printk 都要写 ring buffer 并可能触发控制台输出**，热路径上的日志是实打实的延迟来源 |
| **串口做控制台** | 嵌入式常用 `console=ttyS0,115200`。串口很慢（115200 波特 ≈ 11.5 KB/s），大量 printk 会**阻塞内核**。这也是为什么嵌入式量产版要静默日志 |
| **崩溃调试** | 系统挂死后 `dmesg` 都跑不了，这时串口控制台是唯一能抓到 oops/panic 信息的通道 |
| **netconsole** | 内核支持把 printk 通过 UDP 发到另一台机器（`netconsole` 模块），适合无串口、无屏幕的嵌入式设备 |
| **ring buffer 溢出** | 128 KiB 很小。高频打印会迅速覆盖旧消息，排查偶发问题时要注意"日志被冲掉了" |

---

## 6. 自测题

<details>
<summary>Q1. printk 写进文件了吗？</summary>

没有。它写进内核内存的 ring buffer（树莓派上 128 KiB）。用户态通过 `/dev/kmsg` 读取，`dmesg` 就是这么实现的。只有 journald/rsyslog 转存后才会落盘。

</details>

<details>
<summary>Q2. 为什么 SSH 里加载模块看不到内核消息，但 dmesg 能看到？</summary>

因为 SSH 的伪终端（`/dev/pts/N`）不在 `/proc/consoles` 的控制台列表里。内核只把 printk 转发给注册的 console driver（树莓派上是 tty1 和 ttyAMA10）。消息确实进了 ring buffer，所以 dmesg 能读到。

</details>

<details>
<summary>Q3. `console_loglevel` 影响 dmesg 的输出吗？</summary>

不影响。它只控制"是否转发到控制台"。dmesg 读的是完整 ring buffer，所有级别都在。

</details>

<details>
<summary>Q4. 树莓派上 /dev/console 指向哪个设备？怎么确认？</summary>

指向 `tty1`（HDMI 屏幕）。用 `cat /proc/consoles` 看哪个设备带 `C` 标志。

</details>

<details>
<summary>Q5. 想在加载模块的同时实时看到日志，怎么做？</summary>

开两个终端，一个跑 `sudo dmesg -W | grep --line-buffered <模块名>`，另一个跑 `sudo insmod xxx.ko`。

</details>

<details>
<summary>Q6. 为什么 `head -c 100 /dev/kmsg` 会报 Invalid argument？</summary>

`/dev/kmsg` 是 record-oriented 设备，必须按整条记录读写，不能只读部分字节。用 `cat /dev/kmsg` 或 `dmesg`。

</details>
