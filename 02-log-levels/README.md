# 02-log-levels - printk 日志级别详解

> 配套真机：树莓派 5（aarch64，内核 6.18.34+rpt-rpi-2712）  
> 前置章节：[01-first](../01-first/README.md)（Hello Kernel Module）  
> 延伸阅读：[printk 到底打印到哪里？（输出链路完整解析）](docs/printk-output-path.md)  
> 　　　　　[内核环形缓冲区：容量、覆盖与持久化](docs/kernel-log-buffer.md)

## 本节讲什么

`printk` 是内核里的 `printf`，但它不是直接打印到终端，而是把消息送进内核环形日志缓冲区。每个消息都带一个**日志级别**，内核根据当前系统的 `console_loglevel` 决定哪些消息可以输出到**控制台**，哪些只留在 `dmesg` 里。

本节目标：

1. 理解 printk 的 8 个日志级别（不是 7 个，很多教程漏了 `KERN_CRIT`）。
2. 搞清楚 `KERN_INFO` 这类宏的**真实形态**——它不是 `printk` 的第二个参数，而是字符串拼接。
3. 在真机上加载模块，看默认设置下哪些级别的消息能直接看到，哪些被过滤掉。
4. 学会临时/永久修改 `console_loglevel`，让 `KERN_DEBUG` 也能显示。

---

## 1. 代码结构与核心概念

### 1.1 8 个日志级别（数字越小越紧急）

printk 的级别定义在 `include/linux/kern_levels.h`：

| 宏 | 展开后的字符串 | 数字 | 含义 |
|---|---:|---:|---|
| `KERN_EMERG` | `"<0>"` | 0 | 系统崩溃/不可用 |
| `KERN_ALERT` | `"<1>"` | 1 | 必须立刻处理 |
| `KERN_CRIT` | `"<2>"` | 2 | 严重条件（硬件/临界错误） |
| `KERN_ERR` | `"<3>"` | 3 | 错误条件 |
| `KERN_WARNING` | `"<4>"` | 4 | 警告条件 |
| `KERN_NOTICE` | `"<5>"` | 5 | 正常但重要 |
| `KERN_INFO` | `"<6>"` | 6 | 普通信息 |
| `KERN_DEBUG` | `"<7>"` | 7 | 调试信息 |

**记忆口诀**：

> **Emerg(0) → Alert(1) → Crit(2) → Err(3) → Warning(4) → Notice(5) → Info(6) → Debug(7)**  
> 数字越小越紧急，越不容易被过滤掉。

### 1.2 `printk(KERN_INFO "...")` 的真实展开

这是新手最容易踩的坑：

```c
printk(KERN_INFO "log_levels: Hello, Kernel\n");
```

预处理器会把它展开成：

```c
printk("<6>" "log_levels: Hello, Kernel\n");
```

C 语言里相邻字符串常量会**自动拼接**，所以最终等价于：

```c
printk("<6>log_levels: Hello, Kernel\n");
```

也就是说，`<6>` 被塞进消息头部，printk 解析这个数字 6 就知道这是 `KERN_INFO` 级别。

所以下面这种写法是**错的**：

```c
printk(KERN_INFO, "log_levels: Hello, Kernel\n");   /* ❌ 多写了逗号 */
```

展开后变成 `printk("<6>", "...")`，第一个参数变成了格式字符串 `"<6>"`，第二个参数被当成一个参数但格式串里没占位符——编译能过，但打印出来的不是你想要的内容。

### 1.3 `pr_*` 快捷宏：两层宏 + 一次拼接

本节代码里同时出现了两种写法：前 8 行用完整的 `printk(KERN_xxx "...")`，
头尾用 `pr_info("...")`。后者不是新函数，而是**两层宏嵌套展开**的结果。

内核里的定义（简化自 `include/linux/printk.h` 和 `include/linux/kern_levels.h`）：

```c
/* printk.h */
#define pr_info(fmt, ...)   printk(KERN_INFO    pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)    printk(KERN_ERR     pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)   printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)

/* printk.h，默认模板：原样返回 */
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

/* kern_levels.h，字符串常量，不是变量 */
#define KERN_SOH   "\001"
#define KERN_INFO  KERN_SOH "6"      /* 展开为 "\0016"，等价于老写法 "<6>" */
```

`pr_info("log_levels: module loaded\n")` 在编译器眼里经历的过程：

```
你写的源码
    pr_info("log_levels: module loaded\n")
        │  ① pr_info 是带参数的宏（function-like macro）
        │     预处理器把 fmt 替换成实参，纯文本粘贴
        ▼
    printk(KERN_INFO pr_fmt("log_levels: module loaded\n"))
        │  ② pr_fmt 默认模板：原样返回
        ▼
    printk(KERN_INFO "log_levels: module loaded\n")
        │  ③ KERN_INFO 是对象式宏，替换成字符串常量
        ▼
    printk("\0016" "log_levels: module loaded\n")
        │  ④ C 语言相邻字符串字面量自动拼接（编译期行为）
        ▼
    printk("\0016log_levels: module loaded\n")
        │  ⑤ 编译完成，.rodata 里只有这一条完整字符串
        ▼
    运行时：printk 扫描开头 \001+数字，剥出来当本条消息的日志级别
```

三个关键认知：

1. **"模板"就是带参数的宏**。`fmt` 是占位符，预处理器在编译期做纯文本替换，
   不存在运行时的模板机制，零开销。
2. **`KERN_xxx` 是字符串常量，不是变量**。`#define KERN_INFO "<6>"` 定义的是
   宏，展开后是字符串字面量，直接进 `.rodata`。
3. **整套设计靠两个 C 语言特性撑着**：宏展开（翻译阶段 4）+ 相邻字符串拼接。
   这也是 1.2 节"逗号坑"的根源——加逗号就破坏了第 ④ 步的拼接前提。

#### `##__VA_ARGS__` 是什么

`__VA_ARGS__` 代表 `...` 收到的所有实参。前面的 `##` 是 GNU 扩展：
当没传可变参数时，把 `##` 前面那个**多余的逗号吃掉**。
没有它，`pr_info("hi")` 会展开成 `printk(fmt, )`，直接编不过。

#### `pr_fmt`：自动加模块名前缀的正确姿势

本节每条消息手写了 `log_levels:` 前缀（为了 dmesg 里好 grep）。
其实内核给了标准做法——在 `#include` **之前**覆盖 `pr_fmt` 模板：

```c
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt   /* 必须在所有 #include 之前 */
#include <linux/module.h>
...
pr_info("module loaded\n");   /* dmesg 里自动变成 "log_levels: module loaded" */
```

`KBUILD_MODNAME` 是 kbuild 传给编译器的 `-D` 宏，值就是模块名（不带 `.ko`）。
这样每条日志自带前缀，不用每行手写。本节没这么写是为了让级别演示的代码
更直白，实际驱动开发建议用 `pr_fmt`。

#### 全链路时间线

| 阶段 | 发生的事 | 产物 |
|---|---|---|
| 编译期·预处理 | `pr_*`/`pr_fmt`/`KERN_xxx` 三层宏文本替换 | `printk("<6>...")` |
| 编译期·编译 | 相邻字符串常量拼接 | `.rodata` 里一条完整格式串 |
| 运行时 | `printk` 解析 `\001`+数字前缀 → 剥掉、定级别 → 消息入 ring buffer | dmesg 里的带级别记录 |

---

### 1.4 控制台级别 vs dmesg

内核有两个概念要分开：

- **环形缓冲区（ring buffer）**：所有级别的消息都会进去，`dmesg` 可以全部看到。
- **控制台（console）**：只有**优先级 ≤ console_loglevel** 的消息才会被实时打到控制台。

你可以把 printk 想象成发邮件：

- 所有邮件都进了服务器（dmesg 能查）。
- 但只有重要程度超过某个阈值（console_loglevel）的邮件，才会弹窗通知你（控制台实时显示）。

查看当前控制台级别：

```bash
$ cat /proc/sys/kernel/printk
3       4       1       3
```

四个数字分别是：

| 位置 | 含义 |
|---|---|
| 第 1 个 | **console_loglevel**：当前控制台允许输出的最高级别数字 |
| 第 2 个 | **default_message_loglevel**：没有显式 `<N>` 前缀的消息默认用几级 |
| 第 3 个 | **minimum_console_loglevel**：console_loglevel 能设到的最小值 |
| 第 4 个 | **default_console_loglevel**：系统默认的控制台级别 |

树莓派 5（内核 6.18.34+rpt-rpi-2712）默认是 `3 4 1 3`：只有级别数字 ≤ 3 的消息（EMERG/ALERT/CRIT/ERR）才能打到控制台；WARNING(4)、NOTICE(5)、INFO(6)、DEBUG(7) 只能进 dmesg。

> 注：很多 x86 发行版默认是 `4 4 1 7`，比树莓派多放行 WARNING。具体值以 `cat /proc/sys/kernel/printk` 为准。

> 💡 **"控制台"到底是什么？为什么 SSH 里看不到 printk？**
> 这是新手最容易困惑的地方，单独写了一篇：
> [printk 到底打印到哪里？（输出链路完整解析）](docs/printk-output-path.md)

---

## 2. 代码逐行解析

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
```

- `<linux/module.h>`：模块机制。
- `<linux/kernel.h>`：printk 和 KERN_xxx 宏定义。
- `<linux/init.h>`：`__init` / `__exit` 修饰符和 `module_init`/`module_exit`。

```c
static int __init log_levels_init(void)
{
    pr_info("log_levels: module loaded\n");
```

`pr_info` 是内核提供的快捷宏，等价于 `printk(KERN_INFO ...)`。类似还有 `pr_err`、`pr_warn`、`pr_debug` 等。

```c
    printk(KERN_EMERG  "log_levels: EMERGENCY   - system unusable\n");
    printk(KERN_ALERT   "log_levels: ALERT       - action must be taken\n");
    printk(KERN_CRIT     "log_levels: CRITICAL    - critical condition\n");
    printk(KERN_ERR      "log_levels: ERROR       - error condition\n");
    printk(KERN_WARNING  "log_levels: WARNING     - warning condition\n");
    printk(KERN_NOTICE   "log_levels: NOTICE      - normal but significant\n");
    printk(KERN_INFO     "log_levels: INFO        - informational\n");
    printk(KERN_DEBUG    "log_levels: DEBUG       - debug-level message\n");
```

按级别从高到低打印 8 行，每一行前面都有 `log_levels:` 前缀，方便在 dmesg 里过滤。

```c
    pr_info("log_levels: all 8 levels printed, check which ones you can see\n");
    return 0;
}
```

```c
static void __exit log_levels_exit(void)
{
    pr_info("log_levels: module unloaded\n");
}
```

```c
module_init(log_levels_init);
module_exit(log_levels_exit);

MODULE_AUTHOR("MPCoding - LDD");
MODULE_DESCRIPTION("printk log levels demo for Linux kernel module");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
```

元信息宏上一节已讲过。`MODULE_LICENSE("GPL")` 是唯一能让我们自由调用 GPL-only 内核符号的写法。

---

## 3. 真机实测输出

### 3.1 编译与加载

```bash
cd /path/to/02-log-levels
make
sudo insmod log_levels.ko
```

### 3.2 默认 console_loglevel 下控制台能看到什么

在树莓派 5 上默认是 `3 4 1 3`，所以只有前 4 个级别（EMERG/ALERT/CRIT/ERR）能实时打到控制台。WARNING(4)、NOTICE(5)、INFO(6)、DEBUG(7) 被控制台过滤，只能进 `dmesg`。

用 `dmesg --level=emerg,alert,crit,err` 查看默认能进控制台级别的消息：

```bash
$ dmesg --level=emerg,alert,crit,err | grep log_levels
[ 4077.736831] log_levels: EMERGENCY   - system unusable
[ 4077.736839] log_levels: ALERT       - action must be taken
[ 4077.736841] log_levels: CRITICAL    - critical condition
[ 4077.736842] log_levels: ERROR       - error condition
```

注意：默认 `console_loglevel=3` 时，WARNING(4) **也不会实时刷到控制台**，但 `dmesg` 里仍然完整保留。

### 3.3 用 dmesg 查看全部级别

```bash
$ dmesg --level=notice,info,debug | grep log_levels
```

在树莓派 5 默认级别下：

```
[ 4077.736820] log_levels: module loaded
[ 4077.736844] log_levels: NOTICE      - normal but significant
[ 4077.736845] log_levels: INFO        - informational
[ 4077.736846] log_levels: DEBUG       - debug-level message
[ 4077.736847] log_levels: all 8 levels printed, check which ones you can see
```

再看完整 `dmesg | grep log_levels`：

```
[ 4077.736820] log_levels: module loaded
[ 4077.736831] log_levels: EMERGENCY   - system unusable
[ 4077.736839] log_levels: ALERT       - action must be taken
[ 4077.736841] log_levels: CRITICAL    - critical condition
[ 4077.736842] log_levels: ERROR       - error condition
[ 4077.736843] log_levels: WARNING     - warning condition
[ 4077.736844] log_levels: NOTICE      - normal but significant
[ 4077.736845] log_levels: INFO        - informational
[ 4077.736846] log_levels: DEBUG       - debug-level message
[ 4077.736847] log_levels: all 8 levels printed, check which ones you can see
[ 4077.899839] log_levels: module unloaded
```

**注意**：`dmesg` 里的所有 8 行都在，因为缓冲区不看 `console_loglevel`。

### 3.4 让 KERN_DEBUG 也能打到控制台

**临时生效（重启失效）**：

```bash
$ sudo sh -c 'echo 8 > /proc/sys/kernel/printk'
$ cat /proc/sys/kernel/printk
8       4       1       7
```

把 console_loglevel 提升到 8 后，清空日志再加载模块：

```bash
$ sudo dmesg -C
$ cd /home/wzp/linux-device/02-log-levels
$ sudo insmod log_levels.ko
$ dmesg | grep log_levels
[ 4119.363946] log_levels: module loaded
[ 4119.363963] log_levels: EMERGENCY   - system unusable
[ 4119.363972] log_levels: ALERT       - action must be taken
[ 4119.363974] log_levels: CRITICAL    - critical condition
[ 4119.363976] log_levels: ERROR       - error condition
[ 4119.363977] log_levels: WARNING     - warning condition
[ 4119.363979] log_levels: NOTICE      - normal but significant
[ 4119.363981] log_levels: INFO        - informational
[ 4119.363982] log_levels: DEBUG       - debug-level message
[ 4119.363984] log_levels: all 8 levels printed, check which ones you can see
```

现在 8 个级别全部实时可见。

**永久生效**：

```bash
# 编辑 /etc/sysctl.conf 或创建 /etc/sysctl.d/99-printk.conf
kernel.printk = 8 4 1 7

# 然后应用
sudo sysctl --system
```

### 3.5 按级别过滤 dmesg

```bash
# 只看 error 级别及以上的消息
$ dmesg -l err,crit,alert,emerg

# 只看 info 级别
$ dmesg -l info

# 只看 debug 级别
$ dmesg -l debug
```

`dmesg -l` 用的是级别名（小写），不是 KERN_ 宏名。

---

## 4. 常见坑

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | `printk(KERN_INFO, "...")` 编译通过但输出奇怪 | KERN_xxx 是字符串宏，不是 `printk` 的参数，不能加逗号 | 写成 `printk(KERN_INFO "...")` |
| 2 | `KERN_DEBUG` 的消息控制台实时看不到 | 默认 console_loglevel 通常 ≤ 4，DEBUG(7) 被过滤 | `echo 8 > /proc/sys/kernel/printk`，或用 `dmesg` 直接读缓冲区 |
| 3 | `printk` 没输出到普通终端 | printk 不会写用户态 stdout/终端，只进内核 ring buffer | 用 `dmesg` 或 `cat /dev/kmsg` |
| 4 | 消息前面带 `<6>` 原样输出 | 把 KERN_xxx 当参数传了，没有被字符串拼接解析 | 检查逗号、括号 |
| 5 | `dmesg` 被大量日志淹没，找不到自己的模块 | 没加固定前缀 | 每条 printk 都加 `log_levels:` 前缀，然后用 `dmesg \| grep log_levels` |
| 6 | 想临时提高日志级别但重启后失效 | 只改了 `/proc/sys/kernel/printk` | 写进 `/etc/sysctl.d/` 配置文件 |
| 7 | 用 `pr_debug()` 发现 dmsg 里没有 | `pr_debug()` 默认只在启用了 `DEBUG` 宏或动态调试时编译进代码 | 用 `printk(KERN_DEBUG ...)` 演示，或开启 `CONFIG_DYNAMIC_DEBUG` |
| 8 | 每条消息手写模块名前缀，又长又容易漏 | 没用 `pr_fmt` 模板 | 在所有 `#include` 前 `#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt`，见 1.3 节 |
| 9 | 用 `dmesg -c` 看了一次，之前的日志全没了 | `-c` 是 **read-clear**（读完立刻清空），不是 `-C` | 一律用 `dmesg`（只读）或 `dmesg -C`（只清不读）。见 [kernel-log-buffer.md §5](docs/kernel-log-buffer.md) |
| 10 | 早先的 printk 在 `dmesg` 里找不到了 | 缓冲区只有 **128 KiB**，满了会**静默覆盖**最旧记录 | 去 `journalctl -k` 查（journald 同步抄走了一份）；或调大 `log_buf_len`，同上使用 `dmesg -w` 实时跟随 |
| 11 | 以为缓冲区有几 MB，结果调试输出被冲掉 | 实测 `CONFIG_LOG_BUF_SHIFT=17` → **128 KiB**，约 585 条短消息（单条 214 B）就满 | 用 `grep CONFIG_LOG_BUF_SHIFT /boot/config-$(uname -r)` 自己确认 |
| 12 | `dmesg -C` 之后以为缓冲区空了 | 它**只挪 syslog 游标**，记录一条没删、空间一点没腾。实测清完 `/dev/kmsg` 里 619 条和首条 seq 纹丝不动 | 要看真实内容读 `/dev/kmsg`；要腾空间只能靠新消息挤，或重启。详见 [kernel-log-buffer.md §4](docs/kernel-log-buffer.md) |
| 13 | 用 `dmesg \| wc -l` 判断缓冲区占用率 | 那是"游标之后还剩几条没读"，不是缓冲区里有多少条 | 同上，读 `/dev/kmsg`。本仓库的 `experiments/log-buffer-fill/rbfill.c` 就是干这个的 |

### 4.1 `pr_debug()` 和 `printk(KERN_DEBUG ...)` 的区别

`pr_debug()` 比较特殊：

- 如果**没有**定义 `DEBUG` 宏，很多内核配置下 `pr_debug()` 会被编译成空语句。
- `printk(KERN_DEBUG ...)` 永远会编译进代码，只要级别够低就能看到。

所以做"级别演示"时建议用 `printk(KERN_DEBUG ...)`，避免被 `pr_debug()` 的开关逻辑干扰。

---

## 5. printk 与 HFT/嵌入式关联

| 场景 |  printk 级别的意义 |
|---|---|
| **HFT 低延迟路径** | 生产环境通常把 `console_loglevel` 设得很低（只留 err），避免 INFO/DEBUG 刷屏拖慢中断处理 |
| **嵌入式调试** | 开发阶段开到 8，甚至开 `CONFIG_DYNAMIC_DEBUG`；量产时关闭或重定向到串口/flash |
| **故障排查** | 用 `dmesg -l err` 快速定位硬错误，比翻完整 dmesg 高效 |
| **内核态与用户态区别** | 用户态 `printf` 直接写 fd=1；内核态 `printk` 写 ring buffer，**内核自己从不落盘** —— 是 systemd-journald 在旁边同步抄走一份存进 `/var/log/journal`（本机 rsyslog 未运行，`/var/log/kern.log` 不存在）。详见 [kernel-log-buffer.md](docs/kernel-log-buffer.md) |

---

## 6. 自测题

<details>
<summary>Q1. printk 的 8 个级别里，数字最小和最分别是哪个？</summary>

最小：KERN_EMERG（0，最紧急）；最大：KERN_DEBUG（7，最不紧急）。

</details>

<details>
<summary>Q2. 默认 console_loglevel=3（树莓派 5 实测）时，哪些级别的消息能实时打到控制台？</summary>

EMERG(0)、ALERT(1)、CRIT(2)、ERR(3) 能实时显示；WARNING(4)、NOTICE(5)、INFO(6)、DEBUG(7) 只能进 dmesg。

> 如果你的机器默认是 `4 4 1 7`，则 WARNING(4) 也能实时显示。

</details>

<details>
<summary>Q3. 为什么 `printk(KERN_INFO, "...")` 是错的？</summary>

因为 `KERN_INFO` 展开成字符串 `"<6>"`，`printk` 接收的是**一个格式字符串**（含拼接后的 `<6>`），不是两个参数。加了逗号会变成第一个参数是 `"<6>"`，第二个参数被忽略或导致未定义行为。

</details>

<details>
<summary>Q4. 想让 KERN_DEBUG 的消息实时显示，最快捷的命令是什么？</summary>

`sudo sh -c 'echo 8 > /proc/sys/kernel/printk'`（临时生效）。

</details>

<details>
<summary>Q5. `dmesg` 和实时控制台看到的消息范围一样吗？</summary>

不一样。`dmesg` 读的是完整 ring buffer，所有级别都在；控制台只显示级别 ≤ console_loglevel 的消息。

</details>

<details>
<summary>Q6. `pr_info("hi\n")` 从源码到 dmesg，中间经历了哪几步？分别在什么阶段发生？</summary>

① 预处理期：`pr_info` 宏展开为 `printk(KERN_INFO pr_fmt("hi\n"))`，`pr_fmt` 原样返回、`KERN_INFO` 替换为字符串常量 `"\0016"`；
② 编译期：相邻字符串常量拼接，`.rodata` 里生成一条完整格式串 `"\0016hi\n"`；
③ 运行时：`printk` 解析开头的 `\001`+数字，剥出日志级别 6，消息（不带前缀）进 ring buffer，`dmesg` 读出来。

全程 ①② 是编译期文本替换与拼接，零运行时开销；`KERN_xxx` 是宏不是变量。

</details>
