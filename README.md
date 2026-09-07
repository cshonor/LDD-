# LLD — Linux 内核模块 / 驱动学习笔记

从「Hello, Kernel」开始，逐章啃 Linux 设备驱动。**所有代码都在真机（树莓派 5）上编译、加载、验证过**，
不是抄教程，也不是纸上推演。文档里出现的每一条输出都是 `dmesg` 里真实抄下来的。

面向读者：学过 C、接触过 Linux，但没写过内核模块的初学者。

---

## 真机环境

| 项目 | 值 |
|---|---|
| 硬件 | 树莓派 5（8 GB RAM） |
| 架构 | aarch64 |
| 系统 | Debian GNU/Linux 13 (trixie) |
| 内核 | `6.18.34+rpt-rpi-2712`（`uname -r`） |
| 内核头文件 | `/lib/modules/6.18.34+rpt-rpi-2712/build` |
| 编译器 | gcc 14.2 + make |
| 网络 | `wlan0` 192.168.31.109（WiFi）；`eth0` 未接线 |

> ⚠️ **内核版本号很关键**。模块二进制里锁了 vermagic，换内核版本就得重编。
> 如果你的机器不是这个版本，代码照样能编，但 `artifacts/` 里的 `.ko` 加载不进去——
> 需要自己重编，见下方「快速开始」。

---

## 学习路线

> `00-` 开头的是**前置基础**（Linux 通用知识，不占主线编号），主线从 `01-first` 起。

| 章节 | 主题 | 关键知识点 | 状态 |
|---|---|---|---|
| [00-dev-and-partitions](00-dev-and-partitions/README.md) | `/dev` 与分区：硬件 vs 软件 | 分区表写在介质上（MBR 16 字节逐字解析）、`/dev/xxx` 是 devtmpfs 里的内存节点、名字由驱动+探测顺序决定、**破坏分区表后节点不会立刻消失** | ✅ |
| [01-first](01-first/README.md) | 第一个内核模块 | kbuild 两阶段构建、`insmod`/`rmmod`、`__init`/`__exit` 段机制、`MODULE_LICENSE` 与 taint、GPL-only 符号 | ✅ |
| [02-log-levels](02-log-levels/README.md) | printk 日志级别 | 8 个级别、`console_loglevel` 过滤、字符串拼接语法 | ✅ |
| [02-log-levels/docs](02-log-levels/docs/printk-output-path.md) | printk 输出链路 | ring buffer、`/dev/kmsg`、console vs 终端、为什么 SSH 里看不到 | ✅ |
| [03-module-param](03-module-param/README.md) | 模块参数 | `module_param` 三元组、权限位的真实含义、**为什么 root 也写不了 0444**、`module_param_cb` | ✅ |
| 04-char-device | 字符设备 + `file_operations` | 主次设备号、`register_chrdev`、在 `/dev` 下冒出文件 | 待开始 |
| 05-copy-to-user | 内核 ↔ 用户数据交换 | `copy_to_user` / `copy_from_user`、为什么要拷贝 | 待开始 |
| 06-ioctl | 设备控制接口 | `unlocked_ioctl`、命令码编码 | 待开始 |
| 07-mmap | 内存映射零拷贝 | `mmap`、`remap_pfn_range`、用户态直接访问设备内存 | 待开始 |

**工具**（与主线和章节并列）：

| 目录 | 内容 |
|---|---|
| [tools/remote-access](tools/remote-access/README.md) | 从公司 SSH 回家里树莓派的完整方案（Tailscale）+ NAT 诊断工具 |

---

## 目录约定

```
LLD/
├── README.md                  ← 本文件，总纲
├── 01-first/                  ← 章节：NN-topic 命名
│   ├── README.md              章节主文档（全仓库统一大写）
│   ├── hello.c                源码
│   ├── Makefile               kbuild 脚本
│   ├── artifacts/             真机构建产物（见下）
│   ├── experiments/           验证某个结论的对照实验
│   └── docs/                  章节的补充文档（可选）
├── 02-log-levels/
└── tools/                     与章节无关的工具与方案
```

### 章节目录命名：`NN-topic`

统一用**两位数字 + 连字符**，如 `01-first`、`02-log-levels`。

不用下划线（`02-log_levels`）的原因：连字符在 URL、终端、文档链接里都不需要转义，
而且数字段和主题段的视觉分隔更清楚。

### `artifacts/` 是构建产物归档，不是预编译包

每个章节的 `artifacts/` 里放着真机编译出的 `.ko`、`build.log`、`modinfo.txt`，
**有意提交进 git**（所以 `.gitignore` 里给它开了例外）。

为什么不叫 `prebuilt/`：

`prebuilt` 在开源项目里是约定俗成的说法，意思是「预先编译好、让别人免编译直接用的二进制」
（比如 Android 源码树里的 `prebuilts/`）。但这些 `.ko` 的 vermagic 锁死了内核版本，
换台机器 `insmod` 直接报 `invalid module format`——**对别人没有复用价值**，
所以它不可能是「预编译分发包」。真实用途是**归档**：

| 文件 | 归档目的 |
|---|---|
| `build.log` | 记录「这次编译确实通过了」，含当时的警告信息 |
| `modinfo.txt` | 留一份 vermagic 样本，换内核 / 换机器时对照 |
| `*.ko` | 配套产物实体 |

另外别用 `build/` 当目录名——它几乎出现在所有 `.gitignore` 模板里，会被默认忽略。

### `experiments/` 放对照实验

章节里如果有一个结论值得单独验证（比如「不写 `MODULE_LICENSE` 到底会怎样」），
就把可复现的实验代码放在这里，配一份 `README.md` 说明怎么跑。

---

## 快速开始

在自己的机器上跑通任意一章，三步：

```bash
# 1. 确认内核头文件存在（没有就先装）
ls /lib/modules/$(uname -r)/build/Makefile

# 2. 进入章节目录编译
cd 01-first
make

# 3. 加载并看日志
sudo insmod hello.ko
sudo dmesg | tail -5
sudo rmmod hello
```

编译产物会出现在当前目录，随后可以手动拷进 `artifacts/` 归档。

**从 Windows 上传代码到树莓派编译**：用 [tools/deploy_module.py](tools/deploy_module.py)
（Windows 的 OpenSSH 不支持命令行传密码，所以走 paramiko）：

```bash
python tools/deploy_module.py <本地章节目录> <远端目录> <本地 artifacts 目录> [模块名]
```

它会自动完成：SFTP 上传 → `make` → `insmod` → 抓 `dmesg` → `rmmod` → 把产物和日志取回本地。
模块名可省略，自动从目录里唯一的 `.c` 文件名推断。

---

## 仓库级约定

### 1. 行尾统一 LF

根目录的 `.gitattributes` 里写了 `* text=auto eol=lf`，`*.ko` 标记为二进制。

原因：Makefile 和内核源码在 Linux 上编译，行尾必须是 LF。如果 Windows 检出时被
`core.autocrlf` 转成 CRLF，再传到 Linux，每行末尾会多一个 `\r`，报错长这样：

```
*** missing separator. Stop.
/bin/sh: line 1: $'\r': command not found
```

### 2. 公开仓库，禁止提交凭据

`cshonor/LLD` 是**公开**仓库。任何脚本都**不允许**硬编码密码、密钥、token。

统一做法是从环境变量读取，缺失时直接退出：

```bash
export RPI_HOST=192.168.31.109
export RPI_USER=wzp
export RPI_PASSWORD=你的密码
```

提交前自查：

```bash
git diff --cached | grep -iE 'password|token|secret|wzp123456'
```

### 3. Makefile 缩进必须是真 Tab

kbuild 的规命令行以 Tab 开头，用空格会报 `missing separator`。
写完后自查：`grep -cP '^\t' Makefile`（应等于规则条数）。

---

## 跨章节的常见坑索引

| 坑 | 出处 |
|---|---|
| 破坏分区表后 `/dev/mmcblk0p1` **不会立刻消失**，必须 `BLKRRPART`/`partprobe`/重启让内核重扫 | [00-dev-and-partitions](00-dev-and-partitions/README.md) |
| loop 设备没设 `LO_FLAGS_PARTSCAN` 时，`BLKRRPART` 返回 `EINVAL`（errno 22） | [00-dev-and-partitions](00-dev-and-partitions/README.md) |
| `statfs("/dev").f_type` 是 `TMPFS_MAGIC` 而不是 `DEVFS_SUPER_MAGIC`（devtmpfs 借 shmem 实现） | [00-dev-and-partitions](00-dev-and-partitions/README.md) |
| 不写 `MODULE_LICENSE` 在内核 ≥ 5.16 上是**构建失败**，不只是警告 | [01-first](01-first/README.md) |
| 内核只认 6 个许可证字符串，`"GPL v2 or later"` 和 `"GPLv2"` 都不在清单里 | [01-first](01-first/README.md) |
| `taints kernel` 有两类：树外模块（bit 12，躲不掉）和许可证（bit 0） | [01-first/experiments/license-taint](01-first/experiments/license-taint/results.log) |
| `KERN_INFO` 后面**不能加逗号**，它是字符串拼接不是参数 | [02-log-levels](02-log-levels/README.md) |
| printk 不会出现在 SSH 终端里，得用 `dmesg -W` | [输出链路文档](02-log-levels/docs/printk-output-path.md) |
| 树莓派默认 `console_loglevel` 是 `3 4 1 3`，比常见发行版更严格 | [02-log-levels](02-log-levels/README.md) |
| 模块参数写 `0444` 时 **root 也写不了**：sysfs 有 kernfs 额外检查，不认 `CAP_DAC_OVERRIDE` | [03-module-param](03-module-param/README.md) |
| 模块参数名打错**不报错**（返回码 0、无警告），配置静默失效 | [03-module-param](03-module-param/README.md) |
| `echo` 写 `charp` 参数会把尾随换行一起存进内核字符串 | [03-module-param](03-module-param/README.md) |
| Windows 写的代码传到 Linux 编不过，多半是 CRLF | 见上方 `.gitattributes` |

---

## 关于这套笔记的写法

- **每个结论都要在真机上验证过**。教程和文档会过时（比如 LICENSE 的行为在 5.16 变过），
  真机输出不会。
- **区分「事实」和「常见说法」**。遇到和主流教程不一致的地方，优先跑实验，并把实验代码留下来。
- **写给三个月后的自己**。当时的困惑、踩过的坑、为什么这么选，都记下来。
