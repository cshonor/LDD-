# 01-first · 第一个内核模块（hello.ko）

> **本节目标**：写一个能跑的内核模块，亲手走完「编译 → 加载 → 看日志 → 卸载」全流程，
> 并搞懂背后三件事：模块生命周期、kbuild 构建机制、内核空间和用户空间的根本差异。
>
> **实测环境**：树莓派 5（aarch64），Debian 13 trixie，内核 `6.18.34+rpt-rpi-2712`。
> 本目录下 `artifacts/` 里的产物就是在这台机器上真机编译并加载验证过的。

---

## 0. 学习路径导航

| 章节 | 主题 | 状态 |
|------|------|------|
| **01-first** | 最小模块：生命周期、kbuild、printk | ✅ 本目录 |
| 02-（待定） | 字符设备与 `file_operations` | 待开 |
| 03-（待定） | 用户态/内核态数据交换 `copy_to_user` | 待开 |

---

## 1. 环境准备

内核模块不能像普通程序那样 `gcc hello.c -o hello` 就完事——它必须**针对某个特定内核**编译。
所以第一件事是拿到"内核构建树"。

```bash
uname -r                                  # 查看当前运行的内核版本
ls -l /lib/modules/$(uname -r)/build      # 检查内核构建树是否存在（符号链接）
```

| 环境 | 安装方式 | 说明 |
|------|----------|------|
| 树莓派 OS / Debian | 官方镜像通常**已自带** | 缺失时：`sudo apt install linux-headers-$(uname -r)` |
| Ubuntu 主机 | `sudo apt install build-essential linux-headers-$(uname -r)` | 最省事的学习环境 |
| 虚拟机 | 同上 | **推荐新手用虚拟机练手**，崩了能秒回滚 |
| 交叉编译（开发板） | 需完整内核源码树 + `ARCH=arm64 CROSS_COMPILE=` | 后面章节再碰 |

工具链版本（本目录产物对应的环境）：

| 组件 | 版本 |
|------|------|
| gcc | Debian 14.2.0-19 |
| make | GNU Make（Debian） |
| 内核头文件 | `/usr/src/linux-headers-6.18.34+rpt-rpi-2712` |

---

## 2. 文件清单

| 文件 | 作用 | 是否入库 |
|------|------|----------|
| `hello.c` | 模块源码：入口 `my_init` / 出口 `my_exit` | ✅ |
| `Makefile` | kbuild 构建脚本 | ✅ |
| `README.md` | 本文件 | ✅ |
| `.gitignore` | 忽略中间产物，但放行 `artifacts/` | ✅ |
| `artifacts/hello.ko` | 真机编译产物（含 vermagic，可 `modinfo` 核对） | ✅ 有意提交 |
| `artifacts/build.log` | `make` 完整输出 | ✅ |
| `artifacts/modinfo.txt` | `modinfo hello.ko` 输出 | ✅ |
| `experiments/license-taint/` | `MODULE_LICENSE` / taint / GPL-only 符号的对照实验 | ✅ |
| `*.o` `*.mod.c` `Module.symvers` 等 | 中间产物 | ❌ 忽略 |

---

## 3. 代码解析：hello.c

### 3.1 一个模块的"三要素"

```c
static int  __init my_init(void) { ... }   /* ① 入口：加载时执行一次 */
static void __exit my_exit(void) { ... }   /* ② 出口：卸载时执行一次 */
module_init(my_init);                       /* ③ 注册 */
module_exit(my_exit);
MODULE_LICENSE("GPL");                      /* ③ 元信息 */
```

### 3.2 `__init` / `__exit` 不是装饰品

| 标记 | 链接到哪个段 | 效果 |
|------|--------------|------|
| `__init` | `.init.text` | 初始化**只跑一次**，跑完内核把这段内存回收 |
| `__exit` | `.exit.text` | 仅对可卸载模块有意义；模块若内建进内核，链接时这段被**直接丢弃** |

> 底层视角：这两个宏本质是 `__attribute__((__section__(".init.text")))` 之类的段属性。
> 内核启动时（或 `.ko` 加载时）按段表找到这些函数挨个调用，调完就把整段释放。
> **所以别在会被反复调用的函数上加 `__init`。**

### 3.3 `printk` 与日志级别

`pr_info("...")` 等价于 `printk(KERN_INFO "...")`，是内核推荐的现代写法。
输出进的是**内核环形缓冲区（ring buffer）**，不是你的终端——所以用 `dmesg` 看。

| 级别 | 宏 | 数值 | 用途 |
|------|-----|------|------|
| 最紧急 | `KERN_EMERG` | 0 | 系统不可用 |
| 错误 | `KERN_ERR` | 3 | 硬件/驱动出错 |
| 警告 | `KERN_WARNING` | 4 | 可疑但不致命 |
| 通知 | `KERN_NOTICE` | 5 | 正常但需注意 |
| 信息 | `KERN_INFO` | 6 | **常规信息（本模块用的）** |
| 调试 | `KERN_DEBUG` | 7 | 调试信息，默认常被过滤 |

数字越小越紧急。`/proc/sys/kernel/printk` 里的 `console_loglevel` 决定哪些级别能直接打到控制台；
**不管什么级别，`dmesg` 都能看到**。

### 3.4 `module_init` 的本质

`module_init(fn)` 把函数地址放进特殊的 **initcall 段**，内核加载器从段表里取出来调用。
这不是运行时的函数调用，是**链接期布局 + 加载期查表**。
同理，`MODULE_LICENSE` 等元信息也会被塞进 `.modinfo` 段，`modinfo` 命令就是从那儿读的。

### 3.5 `MODULE_LICENSE` —— 唯一一个"不纯粹"的元信息

常见说法（包括不少教程）："这些 `MODULE_xxx` 都只是描述信息，内核不做逻辑判断。"
**这句话只对一半。**

| 宏 | 内核会拿它做判断吗 |
|---|---|
| `MODULE_AUTHOR` | ❌ 纯描述 |
| `MODULE_DESCRIPTION` | ❌ 纯描述 |
| `MODULE_VERSION` | ❌ 纯描述（`modinfo` 可查，仅此而已） |
| **`MODULE_LICENSE`** | ✅ **会被解析，且决定你能调用哪些内核函数** |

内核导出的符号分两类：

| 导出方式 | 谁能用 | 本机数量 |
|---|---|---|
| `EXPORT_SYMBOL` | 任何模块 | 8199 |
| `EXPORT_SYMBOL_GPL` | **只有 GPL 兼容许可证的模块** | **10989（占 57%）** |

实测：同一份代码，只引用一个 GPL-only 符号 `pm_power_off`，只改许可证字符串——

```console
MODULE_LICENSE("GPL")           → ✅ 构建成功
MODULE_LICENSE("Proprietary")   → ❌ ERROR: modpost: GPL-incompatible module
                                    gplonly.ko uses GPL-only symbol 'pm_power_off'
```

这不是"少几个函数"的问题——本机 19188 个导出符号里 **10989 个你碰不到**。

> ⚠️ **高频坑**：内核只认下面 6 个字符串（`include/linux/license.h` →
> `license_is_gpl_compatible()`），多一个空格、少一个空格都算不认识：
>
> ```
> "GPL"    "GPL v2"    "GPL and additional rights"
> "Dual BSD/GPL"    "Dual MIT/GPL"    "Dual MPL/GPL"
> ```
>
> **`"GPL v2 or later"` 和 `"GPLv2"` 都不在里面**（实测被 modpost 拒绝）。
> 平时看不出问题，一旦用到 GPL-only 符号就炸。**就写 `"GPL"`。**

### 不写 `MODULE_LICENSE` 会怎样？（版本敏感）

| 内核版本 | 行为 |
|---|---|
| < v4.14 | 不检查 |
| v4.14 ~ v5.15 | `WARNING: modpost: missing MODULE_LICENSE()`，照样出 `.ko`，加载时报 taint |
| **≥ v5.16** | **`ERROR: modpost: missing MODULE_LICENSE()`，构建失败，`.ko` 根本不生成** |

变更点：`commit 1d6cd3929360` *modpost: turn missing MODULE_LICENSE() into error*（2021-11，v5.16）。

> `KBUILD_MODPOST_WARN=1` **绕不过**这条检查。那个开关只作用于「缺 vmlinux /
> `Module.symvers`」这类警告，不覆盖 license（已实测，同样 ERROR 失败）。

### taint：out-of-tree 和 license 是两码事

`/proc/sys/kernel/tainted` 是个位图：

| 位 | 值 | 含义 |
|---|---|---|
| bit 0 | 1 | `TAINT_PROPRIETARY_MODULE` —— 非自由许可证 |
| bit 12 | 4096 | `TAINT_OOT_MODULE` —— 树外模块（out-of-tree） |

- 我们的 `hello.c` 明明写了 `"GPL"`，加载时**依然**会看到
  `hello: loading out-of-tree module taints kernel.` —— 那是 **bit 12**，
  **跟许可证无关，写 GPL 也躲不掉**，正常现象，不是错误。
- 只有 **bit 0** 才是许可证引起的污染，文案是
  `module license 'Proprietary' taints kernel.`，并附带
  `Disabling lock debugging due to kernel taint` —— 内核被污染后 **lockdep 会被自动关掉**。
  这是 taint 的**实际副作用**，不只是"内核开发者不负责"的免责声明。

> ⚠️ taint 是**粘性**的：`rmmod` 不清零，只有重启才回到 0；
> 而且文案**每次开机只打印一次**（打印处有 `if (!test_taint(...))` 守卫）。
> 想复现干净的对照实验必须重启。详见 [`experiments/license-taint/`](./experiments/license-taint/)。

---

### 3.6 元信息宏：写在哪、展开成什么、怎么看

这几个宏**习惯上放在 `.c` 文件末尾**（`module_init` / `module_exit` 之后），
不影响程序逻辑，只往 `.ko` 里塞说明信息。

```c
/* 真正的入口/出口 —— 控制代码执行 */
module_init(my_init);
module_exit(my_exit);

/* 元信息 —— 纯描述（LICENSE 除外，见 3.5） */
MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("First kernel module: print hello/goodbye on load/unload");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");
```

它们全是 `MODULE_INFO` 的包装（`include/linux/module.h` 实测）：

```c
#define MODULE_LICENSE(_license)       MODULE_FILE MODULE_INFO(license, _license)
#define MODULE_AUTHOR(_author)         MODULE_INFO(author, _author)
#define MODULE_DESCRIPTION(_descr)     MODULE_INFO(description, _descr)
#define MODULE_VERSION(_version)       MODULE_INFO(version, _version)
```

`MODULE_INFO` 把 `key=value` 以字符串形式写进 ELF 的 **`.modinfo` 段**，
所以 `modinfo` 能读出来（它不加载模块，只是读 ELF 段）：

```console
$ modinfo hello.ko
version:        0.1
description:    First kernel module: print hello/goodbye on load/unload
author:         wzp
license:        GPL
```

> 注意 `MODULE_LICENSE` 展开时还带了个 `MODULE_FILE`——它额外记录构建时的源文件路径。
> 这也是它"不纯粹"的一个侧面。

| 宏 | 写不写 | 缺了会怎样 |
|---|---|---|
| `MODULE_LICENSE` | **必须** | ≥5.16 构建失败；更早只是 taint |
| `MODULE_DESCRIPTION` | 强烈建议 | 内核 6.x 的 modpost 会 `WARNING: modpost: missing MODULE_DESCRIPTION()` |
| `MODULE_AUTHOR` | 建议 | 同上警告 |
| `MODULE_VERSION` | 可选 | 无警告，但 `modinfo` 里没有版本信息 |

> **Debian / 树莓派 OS 上的小坑**：`modinfo` 在 `/sbin`，普通用户的 `PATH` 里没有。
> 直接敲 `modinfo` 会 `command not found`，用 `/sbin/modinfo` 或 `sudo modinfo`。

---

## 4. Makefile 与 kbuild 原理

为什么不能直接 `gcc hello.c`？因为模块必须和内核用**同一套编译选项、同一份 `.config`、同一个版本**构建。
所以做法反过来了：**借内核的 Makefile 来编我的代码**。

```
  make (在你的源码目录执行)
    │
    │  $(MAKE) -C /lib/modules/$(uname -r)/build  M=$(CURDIR)  modules
    ▼
 ┌──────────────────────────────────────────────────┐
 │ ① 切进内核构建树，加载内核的顶层 Makefile         │
 │    - 读入 .config（内核启用了哪些特性）           │
 │    - 设置编译选项：-D__KERNEL__ -DMODULE          │
 │      -nostdinc（不用系统头文件，用内核自己的）     │
 │    - 确定与本机内核一致的 ABI / 结构体布局         │
 └──────────────────────────────────────────────────┘
    │
    │ ② 带着这套规则「回到」M= 指定的目录
    ▼
 ┌──────────────────────────────────────────────────┐
 │ 你的目录：                                        │
 │   hello.c ──CC──> hello.o ──MODPOST──> hello.ko   │
 │   （MODPOST 负责解析符号、生成 Module.symvers）    │
 └──────────────────────────────────────────────────┘
```

| 变量 | 含义 |
|------|------|
| `obj-m` | 编成**可加载模块** `.ko`（`obj-y` = 内建进内核，`obj-n` = 不编） |
| `KERNEL_DIR` | 内核构建树路径，`/lib/modules/$(uname -r)/build` |
| `M=` | 告诉内核：编完**回到这个目录**找我写的源码、把产物放这儿 |
| `$(CURDIR)` | GNU make 内建变量 = 当前目录，比 `$(shell pwd)` 更稳 |

> **新手第一坑**：Makefile 里规则的命令行必须用 **Tab** 缩进，用空格会报
> `*** missing separator. Stop.`

---

## 5. 完整操作与真机实测输出

```bash
make                    # ① 编译
sudo insmod hello.ko    # ② 加载
sudo dmesg | tail -3    # ③ 看内核日志
lsmod | grep hello      # ④ 确认模块在册
sudo rmmod hello        # ⑤ 卸载（注意：用模块名，不是 hello.ko）
sudo dmesg | tail -2    # ⑥ 看卸载日志
make clean              # ⑦ 清理
```

**真机实测**（树莓派 5 / 6.18.34+rpt-rpi-2712，完整输出见 `artifacts/build.log`）：

```console
$ make
make -C /lib/modules/6.18.34+rpt-rpi-2712/build M=/home/wzp/linux-device/01-first modules
make[1]: Entering directory '/usr/src/linux-headers-6.18.34+rpt-rpi-2712'
make[2]: Entering directory '/home/wzp/linux-device/01-first'
  CC [M]  hello.o
  MODPOST Module.symvers
  CC [M]  hello.mod.o
  CC [M]  .module-common.o
  LD [M]  hello.ko
make[2]: Leaving directory '/home/wzp/linux-device/01-first'
make[1]: Leaving directory '/usr/src/linux-headers-6.18.34+rpt-rpi-2712'

$ sudo insmod hello.ko && sudo dmesg | tail -2
[ 1728.912939] Hello: Hello, Kernel

$ lsmod | grep hello
hello                  49152  0

$ sudo rmmod hello && sudo dmesg | tail -2
[ 1729.088789] Hello: Goodbye, Kernel
```

> 注意这次的输出里**没有** `WARNING: modpost: missing MODULE_DESCRIPTION()` ——
> 补齐 `MODULE_DESCRIPTION` / `MODULE_AUTHOR` 之后警告就没了（见坑 #4）。

---

## 6. `artifacts/` 里的产物说明

这里放着真机编译出的 `hello.ko`，**有意提交进 git**（所以 `.gitignore` 里给 `artifacts/` 开了例外）。
用途：换机器或换内核时，先 `modinfo` 比对一下 vermagic，就能判断这个二进制还能不能用。

### 为什么叫 `artifacts/` 而不是 `prebuilt/`

`prebuilt` 在开源项目里是约定俗成的说法，意思是「**预先编译好、让别人免编译直接用的二进制**」——
比如 Android 源码树里的 `prebuilts/`（预编译的 clang / gcc toolchain），或 release 包里的预编译库。

但这个目录里的 `.ko` **对别人没有复用价值**：vermagic 锁死了内核版本
（`6.18.34+rpt-rpi-2712 ... aarch64`），换任何一台内核版本不同的机器 `insmod` 都会直接报
`invalid module format`。既然别人拿了也加载不了，它就不是「预编译分发包」。

它的真实用途是**构建产物归档**（build artifact）：

| 文件 | 归档目的 |
|---|---|
| `build.log` | 记录「这次编译确实通过了」，含当时的警告信息 |
| `modinfo.txt` | 留一份 vermagic 样本，换内核 / 换机器时对照 |
| `hello.ko` | 配套的产物实体 |

所以叫 `artifacts/`。另外不要用 `build/`——它几乎出现在所有 `.gitignore` 模板里，会被默认忽略。

```
$ modinfo artifacts/hello.ko
filename:       /home/wzp/linux-device/01-first/hello.ko
version:        0.1
description:    First kernel module: print hello/goodbye on load/unload
author:         wzp
license:        GPL
srcversion:     9AF75F930CC4D9AF5776776
depends:
name:           hello
vermagic:       6.18.34+rpt-rpi-2712 SMP preempt mod_unload modversions aarch64
```

**`vermagic` 是关键**：它是模块的"身份证"，记录了这个模块是为哪个内核编的。
`insmod` 时会拿它和当前运行内核逐项比对：

| vermagic 字段 | 含义 |
|---------------|------|
| `6.18.34+rpt-rpi-2712` | 内核版本号（必须完全一致，`uname -r` 的值） |
| `SMP` | 对称多处理（多核） |
| `preempt` | 内核抢占模型 |
| `mod_unload` | 支持卸载 |
| `modversions` | 启用符号版本校验（CRC），防止结构体布局对不上 |
| `aarch64` | CPU 架构 |

任何一项对不上，`insmod` 直接拒绝加载。

---

## 7. 新手必踩的坑

| # | 现象 | 原因 | 解决 |
|---|------|------|------|
| 1 | `*** missing separator. Stop.` | Makefile 命令行用了空格缩进 | 改成 **Tab** |
| 2 | `insmod: ERROR: could not insert module: Invalid module format` | vermagic 与运行内核不匹配（头文件版本 ≠ 运行内核版本） | 用 `uname -r` 核对，装对应版本的 headers 重新编译 |
| 3 | `dmesg` 什么都没有 | ① 权限：Debian 系默认 `kernel.dmesg_restrict=1`，普通用户读不了<br>② 日志被刷掉了 <br>③ 级别低于 console_loglevel（但 dmesg 仍应可见） | 用 `sudo dmesg`；用 `dmesg -w` 实时跟踪 |
| 4 | `WARNING: modpost: missing MODULE_DESCRIPTION()` | 缺 `MODULE_DESCRIPTION` / `MODULE_AUTHOR` | 补上即可（本目录已补） |
| 5 | `hello: loading out-of-tree module taints kernel.` | 外部模块的正常提示，**不是错误**（bit 12，与许可证无关） | 忽略，见 3.5 |
| 6 | `rmmod: ERROR: Module hello is in use` | 还有人在用它（引用计数非 0，比如被打开的设备节点） | 先关掉使用者；别硬来 |
| 7 | `rmmod hello.ko` 报找不到模块 | `rmmod` 用的是**模块名** | `rmmod hello` |
| 8 | 编译报 `printf`/`malloc` 未定义 | 内核里**没有 libc** | `printf`→`printk`/`pr_info`，`malloc`→`kmalloc`（`#include <linux/slab.h>`） |
| 9 | 模块一加载系统就卡死/重启 | 内核态没有内存保护，野指针 = oops 甚至 panic | 用虚拟机或树莓派练手；善用 `dmesg` 看 oops 栈 |
| 10 | 教程代码在你机器上编不过 | 内核 API 变动很快（`file_operations`、定时器、proc 接口都改过） | 查你内核版本对应的头文件，**以源码为准** |
| 11 | 在 Windows 上写/克隆的代码，传到 Linux 编译报 `missing separator` 或 `$'\r': command not found` | 行尾被 git 转成了 CRLF，`\r` 变成命令的一部分 | 仓库根目录的 `.gitattributes` 已用 `* text=auto eol=lf` 锁死 LF，检出即 LF |
| 12 | `ERROR: modpost: missing MODULE_LICENSE() in xxx.o`，`.ko` 没生成 | ≥ v5.16 起这是**硬错误**，不是警告 | 补 `MODULE_LICENSE("GPL")`；`KBUILD_MODPOST_WARN=1` 绕不过去 |
| 13 | `ERROR: modpost: GPL-incompatible module xxx.ko uses GPL-only symbol 'yyy'` | 许可证字符串不在内核认可的 6 个里 | 见 3.5。**`"GPL v2 or later"` 和 `"GPLv2"` 都不合法** |
| 14 | `modinfo: command not found` | `modinfo` 在 `/sbin`，不在普通用户 `PATH` | 用 `/sbin/modinfo` 或 `sudo modinfo` |
| 15 | 换了许可证重新加载，dmesg 里却看不到 taint 文案 | taint 位**粘性**（`rmmod` 不清零）+ 文案**每开机只打印一次** | 重启后再测 |

### 内核空间 vs 用户空间（务必分清）

| 维度 | 用户空间（应用） | 内核空间（模块） |
|------|------------------|------------------|
| 运行级别 | ring 3 | ring 0 |
| 入口 | `main()` | `module_init` 注册的函数 |
| 输出 | `printf` | `printk` / `pr_info` |
| 内存分配 | `malloc` / `free` | `kmalloc` / `kfree`（`#include <linux/slab.h>`） |
| 可用库 | glibc 全套 | **无 libc**，只有内核导出的函数 |
| 栈大小 | 默认 8 MB | **8 KB 或 16 KB**，别在栈上开大数组 |
| 出错后果 | 段错误，进程死 | oops / panic，**整个系统可能挂** |
| 浮点运算 | 随便用 | 需显式保存/恢复 FPU 状态（arm64 另说，原则：避免） |
| 访问用户内存 | — | 不能直接解引用用户指针，必须用 `copy_to_user` / `copy_from_user` |

---

## 8. HFT / 嵌入式关联

**嵌入式方向**：驱动开发是嵌入式 Linux 的核心能力。这个 hello 模块是所有驱动的骨架——
后续把它接上 `file_operations` 就是字符设备，再往前就是 GPIO 点灯、I2C 读传感器、SPI 驱动屏幕。
在树莓派这种真机上练，好处是能直接碰到硬件，比纯虚拟机直观得多。

**HFT 方向**：内核模块本身在交易系统生产环境里很少直接上（稳定性与合规风险），
但**理解内核机制是低延迟调优的前提**：

| 调优项 | 涉及的内核机制 |
|--------|----------------|
| 降低系统调用开销 | 自定义 syscall / `io_uring` / eBPF |
| 绕过协议栈 | 内核旁路（kernel bypass）、网卡驱动层直接收发 |
| 减少抖动 | `isolcpus` 隔离核心、`PREEMPT_RT` 实时补丁、中断亲和性绑定 |
| 内存延迟 | 大页（hugepage）、`mlockall` 防换页、NUMA 绑节点 |
| 零拷贝通道 | 自研字符设备 + `mmap` 共享内存（就是本路径的后续章节） |

理解 `insmod` 之后内核里发生了什么，是读懂上面这些调优手段的地基。

---

## 9. 自测题

<details>
<summary>点开做题（先自己想，再看答案）</summary>

**Q1. 模块入口函数返回 `-1` 会怎样？**
> 加载失败。`insmod` 会报 `Invalid parameters` 之类的错误，模块不会被加载。
> 内核约定：返回 0 成功，返回**负的错误码**（如 `-ENOMEM`、`-ENODEV`）表示失败。

**Q2. `__init` 修饰的函数，内存会怎样？**
> 被链进 `.init.text` 段，执行完后这段内存被释放。所以它只能用于"一次性"的初始化。

**Q3. 如果只写 `module_init` 不写 `module_exit` 会怎样？**
> 能加载，但无法卸载（`rmmod` 会报 Device or resource busy / 模块不支持卸载）。
> 只有清理工作真的为空、且模块永不卸载时才这么写。

**Q4. `pr_info` 的输出一定能在终端看到吗？去哪儿看？**
> 不一定。`pr_info` 是 KERN_INFO(6) 级别，若 console_loglevel 更低，就不会打到控制台。
> 但**一定会进内核 ring buffer**，用 `dmesg`（Debian 下常需 `sudo dmesg`）能看到。

**Q5. 升级内核后，原来的 `hello.ko` 还能加载吗？怎么判断？**
> 基本不能。用 `modinfo hello.ko | grep vermagic` 和 `uname -r` 比对，
> 版本号不一致就会 `Invalid module format`。重新 `make` 即可（这也是为什么仓库里提交 `.ko`
> 只是记录，实际用时都得重新编）。

**Q6. 内核模块里能调用 `printf` 吗？为什么？**
> 不能。内核没有链接 glibc，`printf` 最终依赖的 `write(2)` 系统调用在内核态根本不存在这个概念。
> 用 `printk` / `pr_info` 写 ring buffer，用户态再用 `dmesg` 读。

**Q7. 加载时看到 `taints kernel` 是出错了吗？**
> 不是。只是说明你加载了内核源码树之外的模块。GPL 许可证声明解决的是"能否使用 GPL-only 符号"，
> 与 out-of-tree 的 taint 是两回事。

**Q8. `MODULE_AUTHOR` 和 `MODULE_LICENSE` 都是元信息，性质一样吗？**
> 不一样，这是常见误区。
> `AUTHOR` / `DESCRIPTION` / `VERSION` 内核从不解析，纯给人看。
> 但 `MODULE_LICENSE` 会被内核解析成 `license_gplok`，**决定你能不能用 `EXPORT_SYMBOL_GPL`
> 导出的符号**——本机 19188 个导出符号里有 10989 个是 GPL-only。
> 而且从 v5.16 起，不写它连编译都过不去。

**Q9. 我写了 `MODULE_LICENSE("GPL v2 or later")`，为什么用到某个内核函数就编译失败了？**
> 因为内核只认 6 个字符串，`"GPL v2 or later"` 不在其中（`"GPLv2"` 也不在）。
> 它被当作非 GPL 兼容模块，一碰 GPL-only 符号 modpost 就报错：
> `ERROR: modpost: GPL-incompatible module xxx.ko uses GPL-only symbol 'yyy'`
> 改成 `"GPL"` 或 `"GPL v2"` 即可。

**Q10. `/proc/sys/kernel/tainted` 显示 4097，是什么意思？**
> 位图：`4096(bit 12, TAINT_OOT_MODULE) + 1(bit 0, TAINT_PROPRIETARY_MODULE)`。
> 说明系统加载过树外模块，且其中至少有一个是非自由许可证。
> `rmmod` 不会让这个值降回去，只有重启才会归零。

</details>

---

## 10. 下一步

现在的模块只是"打印两句话"，它还没有和外界交互的能力。接下来的关键是三件事：

1. **设备号与设备节点**：让模块在 `/dev` 下出现一个文件
2. **`file_operations`**：实现 `open` / `read` / `write` / `release`，让用户程序能操作它
3. **`copy_to_user` / `copy_from_user`**：安全地跨内核/用户边界搬数据

做到第 3 步，你就有了一条从用户态到内核态的完整数据通路——这也是后面做
`mmap` 零拷贝、字符设备驱动、乃至低延迟数据通道的起点。
