# 实验：printk 到底流向哪里？（C 版）

验证「printk 不会自动出现在 SSH 终端」这一结论，并摸清内核日志的完整输出链路。

结论文档见：[../../docs/printk-output-path.md](../../docs/printk-output-path.md)

> 本目录下的实验**全部用 C 实现，直接在板上运行**（原来那两个 Python 脚本需要
> 从开发机 SSH 过去编排，已删除）。这样更贴近 LDD 的主旨：
> 用系统调用和字符设备去看内核，而不是靠远程命令拼接。

## 文件说明

| 文件 | 作用 | 用到的关键 API |
|---|---|---|
| `run_console.c` | 子进程用 `finit_module()` 加载模块，父进程用 `poll()` + `read()` 实时跟随 `/dev/kmsg`。证明消息确实产生、且能实时捕获 | `finit_module(2)`、`delete_module(2)`、`klogctl(2)`、`poll(2)` |
| `run_pty.c` | 对照两组终端形态：A) 子进程 stdout 接到 pipe；B) 子进程跑在 `forkpty()` 分配的伪终端里（pty 就是它的控制终端）。证明**两种都收不到** printk | `forkpty(3)`、`pipe(2)`、`poll(2)`、`execlp(3)` |

## 运行方式

```sh
make                 # 编译 run_console / run_pty（需要 gcc + libutil）
sudo make run        # 跑 run_console
sudo make run-pty    # 跑 run_pty
```

手动指定模块路径：

```sh
sudo ./run_console /home/wzp/LDD-/02-log-levels/log_levels.ko
sudo ./run_pty     /home/wzp/LDD-/02-log-levels/log_levels.ko
```

两个程序都需要 root：`finit_module` 要 `CAP_SYSADMIN`，`klogctl` 要 `CAP_SYSLOG`。

## 实测输出

### run_console

```
── 内核已注册的 console (/proc/consoles) ──
   tty1                 -WU (EC  p  )    4:1
   ttyAMA10             -W- (E  Np a)  204:74

[子进程 48045] 调用 finit_module("/home/wzp/LDD-/02-log-levels/log_levels.ko") ...
[跟随 10:46:45.189] log_levels: module loaded
[跟随 10:46:45.189] log_levels: EMERGENCY   - system unusable
...（共 10 条，8 个级别全在）
[子进程 48045] finit_module 返回 0 —— 模块已加载

═══ 跟随结束：共捕获 10 条新内核消息 ═══
```

要点：这 10 条是**父进程主动读 /dev/kmsg 读到的**。
父子进程的 stdout 上除了自己 `printf` 的内容，没有任何一条是内核送过来的。

### run_pty

```
════ A. 无终端：子进程 stdout 接到一根 pipe ════
  pipe 里收到的内容:
---8<----------
MARKER_A: 我要 insmod 了            ← 只有子进程自己 printf 的
--->8----------
  包含内核消息吗: 否

════ B. 有终端：子进程跑在 forkpty() 分配的伪终端里 ════
  伪终端里收到的内容:
---8<----------
MARKER_B: 我在伪终端里，tty=/dev/pts/0    ← pty 确实是它的控制终端
MARKER_B: insmod 返回 0
MARKER_B: 结束
--->8----------
  包含内核消息吗: 否 —— 即使是真的控制终端也收不到

════ 收尾：用 dmesg 确认消息确实产生了 ════
[668954.728817] log_levels: INFO        - informational
[668954.728818] log_levels: DEBUG       - debug-level message
...
```

## 结论

终端**是不是"真的"根本不重要**，关键是它有没有被注册成 console：

```
/proc/consoles:
   tty1        ← HDMI 屏幕（虚拟控制台）
   ttyAMA10    ← 串口 UART

SSH 登录拿到的是 /dev/pts/N —— 永远不在这个列表里，所以永远等不到 printk。
```

printk 只往两个地方写：

1. 内核环形缓冲区（所有人都能读，这就是 dmesg 存在的理由）
2. `/proc/consoles` 里列出的已注册 console

## 复现时的注意事项

1. **跟随必须用 `/dev/kmsg` + `poll`，不要用 `dmesg -w`**。
   `dmesg -w` 走的是 syslog 游标，行为跟 `/dev/kmsg` 不完全一致
   （详见 ../log-buffer-fill/README.md 里那个「dmesg 看到的 ≠ 缓冲区内容」的坑）。
2. **先排空再跟随**。打开 `/dev/kmsg` 后它会从**最老一条存活记录**开始吐，
   所以必须先把已有记录全读掉，之后读到的才是真·新消息（见 `kmsg_open_drained()`）。
3. **`/dev/kmsg` 是 record-oriented 设备**，必须按整条记录读。
   `head -c 100 /dev/kmsg` 会报 `Invalid argument`，缓冲区开小了也不行。
4. **别用 `klogctl(SYSLOG_ACTION_CLEAR)` 当"清空"**。它只挪 syslog 游标，
   缓冲区里的记录一条没删，所以排空必须靠把 `/dev/kmsg` 读干。
5. **判断"终端收到没有"时要区分回显**。shell 的命令回显里可能含路径字符串，
   用 `grep log_levels` 会命中回显而不是内核消息。本程序用固定 `MARKER_` 前缀规避。

## 补充验证命令（手动执行）

```sh
# 内核启动参数里的控制台
cat /proc/cmdline | tr ' ' '\n' | grep console

# 已注册的控制台（带 C 的是 /dev/console）
cat /proc/consoles

# 往控制台写（会显示在 HDMI 屏幕上，SSH 里看不到 —— 这个对比最直观）
echo 'TEST' | sudo tee /dev/console

# 环形缓冲区大小（比读 /boot/config 更直接）
# 用 C 的话：klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0)
```
