# 00-dev-and-partitions — `/dev` 与分区：分清硬件、分区、设备文件

> 真机环境：树莓派 5 / Debian 13 trixie / `6.18.34+rpt-rpi-2712` (aarch64)
> 本章所有输出都在这台机器上实测过：真实 SD 卡（`mmcblk0`）+ 真实空盘（`nvme0n1`）+ loop 镜像。
> 实验程序全部用 C 写（`experiments/partition-table-lab/ptab_lab.c`），没有 shell/Python 拼凑。

## 本节讲什么

学驱动之前必须先拆掉一个混淆：**"分区"和"/dev 下的设备文件"是两层东西**。

- 分区的定义（起始 LBA、长度、类型）**写在 SD 卡的 0 号扇区里**，拔下来换台机器还在。
- `/dev/mmcblk0`、`/dev/mmcblk0p1` 这些**名字**，是内核开机扫描时**在内存里现拼出来的**，
  SD 卡里一个字节都没存过它们。

这一章把这条链路从头到尾摸一遍，并回答章末的 Quiz：**分区表被破坏之后，
`/dev/mmcblk0` 还在吗？`mmcblk0p1` 还在吗？**

---

## 1. 三层模型

```
   用户态            open("/dev/mmcblk0p1")  ──┐
                                               │  通过主次设备号找到驱动
   ─────────────────────────────────────────── ┼────────────────────────
   /dev（devtmpfs，纯内存）                     │
     mmcblk0      179:0   ← 整盘节点           │
     mmcblk0p1    179:1   ← 分区节点  ◄────────┘
     /dev/disk/by-partuuid/e928d444-01 -> ../../mmcblk0p1   ← udev 建的符号链接
         ▲                    ▲
         │ devtmpfs 建节点     │ udev 建链接/改权限
   ──────┼────────────────────┼────────────────────────────────────────
   内核   │                    │
     块设备层：读 LBA0 的分区表 → 为每个有效分区项建一个 struct block_device
         ▲
         │ 驱动探测（mmc 驱动 / nvme 驱动 / sd 驱动）
   ──────┼────────────────────────────────────────────────────────────
   硬件   │
     SD 卡闪存：LBA0 = MBR 分区表（4×16 字节）+ 0x55AA 签名
                LBA 16384 起 = p1 的内容（vfat）
                LBA 1064960 起 = p2 的内容（ext4）
```

一句话：**介质上存的是"从哪到哪、什么类型"，内核把它翻译成"叫什么名字"。**

---

## 2. 硬件侧：分区表确实写在 SD 卡介质上

`ptab_lab dump /dev/mmcblk0` 直接读 0 号扇区（真机输出）：

```
== /dev/mmcblk0 ==
  容量            : 31914983424 字节 (30436.5 MiB, 62333952 个 512B 扇区)
  LBA0 结尾签名   : 55 aa  -> 0x55AA，有效 MBR
  LBA0 磁盘签名   : e928d444  -> udev 会据此生成 /dev/disk/by-partuuid/e928d444-01 ...
  LBA1            : 不是 GPT header（MBR 分区表）
  分区项（介质上实实在在写的 4x16 字节）:
    #  status  type   start LBA     sectors    size
    1  0x00    0x0c    16384        1048576    512.0 MiB
    2  0x00    0x83    1064960      61268992   29916.5 MiB
```

### 2.1 MBR 分区项的 16 个字节

| 偏移 | 长度 | 含义 | 本卡 p1 实测值 |
|---|---|---|---|
| `0x00` | 1 | 引导标志（`0x80` = 可引导） | `00`（Pi 不用它引导，GPU 固件直接找 FAT 分区） |
| `0x01` | 3 | 起始 CHS（古董字段） | `00 01 80` |
| `0x04` | 1 | 分区类型 | `0c` = FAT32 (LBA)；`83` = Linux；`ee` = GPT 保护 |
| `0x05` | 3 | 结束 CHS | `03 e0 ff` |
| `0x08` | 4 | **起始 LBA**（小端） | `00 40 00 00` = 16384 |
| `0x0C` | 4 | **扇区数**（小端） | `00 00 10 00` = 1048576 → 512 MiB |

**重点看这 16 个字节里有什么、没有什么**：全是数字，**没有任何一个字节存着
"mmcblk0p1" 这种设备名**。所谓 `p1` 的编号，只是"这张表的第 1 个表项"这个位置含义，
名字是内核按 `设备名 + p + 表项序号` 拼出来的。

> GPT 的分区项是 128 字节，里面确实有 36 字节的 UTF-16 名字——但那是**分区标签**
> （`PARTLABEL`），用来给人看的，和 `/dev/xxx` 的设备文件名仍然是两回事。

### 2.2 内核读完就记在内存里

```
/sys/block/mmcblk0/mmcblk0p1/start = 16384      ← 就是分区项里的 0x08 字段
/sys/block/mmcblk0/mmcblk0p1/size  = 1048576    ← 就是分区项里的 0x0C 字段
```

sysfs 这两个数字和介质上的字节**逐位对应**——分区信息的源头在介质，sysfs 只是缓存。

---

## 3. 软件侧：`/dev` 里的名字是内核现拼的

### 3.1 `/dev` 不是 SD 卡上的目录

```
/dev         挂载点 /dev    类型 devtmpfs   源 udev
/              st_dev = 45826 (major:minor = 179:2)     ← 179:2 就是 mmcblk0p2，SD 卡上的 ext4
/dev           st_dev = 6    (major:minor = 0:6)        ← 匿名 devtmpfs，跟 SD 卡没关系
-> st_dev 不同：两个路径分属不同的文件系统实例

$ df -h /dev
Filesystem      Size  Used Avail Use% Mounted on
udev            3.9G     0  3.9G   0% /dev          ← 占的是内存，不是 SD 卡
```

`stat("/")` 拿到的 `st_dev` 展开就是 **179:2**，即 `mmcblk0p2`（SD 卡上的 ext4 根分区）；
而 `stat("/dev")` 的 `st_dev` 是另一个文件系统实例。`/dev` 里的东西**一个字节都不在 SD 卡上**。

> ⚠️ 坑：`statfs("/dev").f_type` 实测是 `0x01021994`（`TMPFS_MAGIC`），**不是** `0x1373`
> （`DEVFS_SUPER_MAGIC`）。因为 devtmpfs 借用了 shmem 的实现，共用 tmpfs 的 magic。
> 判断文件系统类型要看 `/proc/mounts` 的 type 字段，别只看 magic。

### 3.2 谁建节点、谁建链接

| 产物 | 谁建的 | 在哪 |
|---|---|---|
| `/dev/mmcblk0`、`/dev/mmcblk0p1` | **内核 devtmpfs**（驱动 `add_disk()` / 分区扫描时） | 内存 |
| `/dev/disk/by-partuuid/e928d444-01` | **udev**（用户态守护进程，按规则建的符号链接） | 内存 |
| 节点权限/owner | udev 规则 | 内存 |

实测：

```
$ ls -l /dev/disk/by-partuuid/
e928d444-01 -> ../../mmcblk0p1
e928d444-02 -> ../../mmcblk0p2
```

注意 `e928d444` 这个前缀——它**就是介质上 MBR 的 4 字节磁盘签名**（见 2.1 的 dump 输出）。
所以这条链接是"介质里的数据" → "软件生成的名字" 的完整链条：
**卡上存数字 → 内核读出数字 → udev 按数字起名字**。

### 3.3 名字从哪来：驱动决定前缀，探测顺序决定序号

```
$ sed -n '/Block devices/,$p' /proc/devices
  7 loop
  8 sd          ← USB 读卡器 / U 盘走的驱动，名字 sd{a,b,c}
179 mmc         ← SD 卡走的驱动，名字 mmcblk{0,1}
259 blkext      ← 动态分配的 major（loop 的分区就落在这里）
```

major 号是**驱动注册时向内核申请的**（`register_blkdev`），跟卡里存了什么毫无关系。

所以"同一张卡换台机器变成 `/dev/sdb`"这件事的真相是：

| | SD 卡插槽（mmc 驱动） | USB 读卡器（sd 驱动） |
|---|---|---|
| 设备名 | `mmcblk0` | `sdb`（a/b/c 看当时已占了几个） |
| major | 179 | 8 |
| 分区名 | `mmcblk0p1` | `sdb1` |
| 分区表里的内容 | **完全不变** | **完全不变** |

---

## 4. Quiz：破坏分区表后，`mmcblk0` 还在吗？`mmcblk0p1` 还在吗？

**答案要分三种情形**，实测（loop 镜像 + 真实 238 GB 空盘各跑一遍，结果一致）：

| 情形 | `/dev/mmcblk0`（整盘） | `/dev/mmcblk0p1`（分区） | 原因 |
|---|---|---|---|
| 刚破坏完，**没重启、没重扫** | ✅ 在 | ✅ **还在！** | 内核内存里还留着上次读到的分区信息，没人通知它介质变了 |
| 重扫之后（重启 / 拔插 / `partprobe` / `BLKRRPART`） | ✅ 在 | ❌ **消失** | 内核重读 LBA0，签名不是 `0x55AA` → 不认这张表 → 不建分区节点 |
| 卡坏到驱动都认不出 | ❌ 消失 | ❌ 消失 | 连设备都没探测到，整盘节点都不会有 |
| 把 `0x55AA` 写回去 + 重扫 | ✅ 在 | ✅ **又冒出来** | 分区项的字节一直没动，只改了 2 个字节的签名 |

真盘实测片段（`ptab_lab disklab /dev/nvme0n1`）：

```
真盘 第 2 步：写 MBR + BLKRRPART 重读
    /dev/nvme0n1p1   块设备  major:minor = 259:3     ← 出现
    /dev/nvme0n1     块设备  major:minor = 259:0

真盘 第 3 步：只抹掉 0x55AA，不通知内核
    /dev/nvme0n1p1   块设备  major:minor = 259:3     ← 还在！内核还蒙在鼓里
    /dev/nvme0n1     块设备  major:minor = 259:0

真盘 第 4 步：BLKRRPART 重读
    /dev/nvme0n1     块设备  major:minor = 259:0     ← 整盘还在，分区节点没了

  （此时 dump 这张盘：分区项数据依然完整地躺在介质上 —— LBA 2048 + 500116144 扇区，
    但结尾是 00 00，内核就是不认。数据没丢，只是"不被承认"。）
```

### 4.1 两个容易想错的地方

1. **"分区表坏了，数据就没了"是错的。** 破坏 `0x55AA` 只动了 2 个字节，分区项里
   的起止 LBA 原封不动。把签名写回去、重扫一次，分区立刻回来。
   真正毁数据的是 `mkfs` 覆盖或者把分区项整块清零后**重新分区**。
2. **分区节点消失，不影响已经挂载的文件系统。** 挂载关系（`vfsmount`）指向的是
   superblock，不是 `/dev` 下的那个文件。所以你在一个已经起来的系统里把分区表搞坏，
   只要不去动已经挂载的分区，系统照样能跑——**重启才开机拷**。这也是"改了分区表
   却不重启"能苟活很久的原因。

### 4.2 怎么主动让内核重扫

| 手段 | 本质 |
|---|---|
| 重启 / 拔插卡 | 驱动重新探测 + 重新解析分区表 |
| `partprobe /dev/mmcblk0`（`parted` 包） | 发 `BLKRRPART` ioctl |
| `blockdev --rereadpt /dev/mmcblk0` | 同上 |
| 实验里的 `ptab_lab reread <dev>` | 直接 `ioctl(fd, BLKRRPART, 0)` |

---

## 5. 你的理解 → 实测校正

| 你的说法 | 判定 | 实测补充 |
|---|---|---|
| 分区表写（烧）在 SD 卡介质上，换机器也读得到 | ✅ | 实测 dump 出 `55AA` + 两个分区项；`by-partuuid` 前缀 `e928d444` 就是介质上的磁盘签名 |
| 分区表记录"哪里是 p1 启动分区" | ⚠️ 措辞 | 分区表只存 **[起始 LBA, 扇区数, 类型]** 三元组；"p1"是表项序号的位置含义，不是存进去的字符串 |
| `/dev` 是 devtmpfs，开机内核扫描后动态创建 | ✅ | `st_dev` 对比 + `/proc/mounts type=devtmpfs` + `df` 显示占内存 |
| SD 卡里没有 `/dev` 目录、没有 `mmcblk0` 这个名字 | ✅ | 16 字节分区项里全是数字，无名字字段 |
| 拔卡后 `/dev/mmcblk*` 立刻消失 | ✅ | 真卡不能拔（是根盘），用 loop 解绑等价验证：整盘节点 + 分区节点一起消失 |
| 换机器可能变成 `/dev/sdb` | ✅ | `/proc/devices`：`179 mmc` vs `8 sd`，前缀由驱动决定 |
| （补充）破坏分区表后节点立刻消失 | ❌ | **不会**！不重扫就还在，这是本 Quiz 最容易答错的点 |
| （补充）整盘节点依赖分区表 | ❌ | 空盘（LBA0 全零）照样有 `/dev/nvme0n1` |

---

## 6. 实验：`experiments/partition-table-lab/`

全 C 实现，只用 `pread/pwrite` + `ioctl(BLKRRPART/BLKGETSIZE64)` + `statfs` + `scandir`，
还能用 `LOOP_CONFIGURE` 自己把普通文件变成块设备（不依赖 `losetup`）。

```bash
gcc -O2 -Wall -Wextra -o ptab_lab ptab_lab.c     # 零告警
sudo ./ptab_lab lab /tmp/lab.img /dev/mmcblk0    # loop 镜像完整对照流程
sudo ./ptab_lab disklab /dev/nvme0n1             # 真实空盘：破坏-恢复（会复原现场）
```

| 子命令 | 作用 |
|---|---|
| `dump <dev>` | 解析并打印介质上的 MBR/GPT 分区表 + 分区项 16 字节 hex |
| `mkpt <dev> [签名]` | 写一张合法 MBR（p1 占满剩余空间） |
| `corrupt <dev> <sig\|entry\|all>` | 三种破坏：只抹签名 / 只清分区项 / 整块清零 |
| `reread <dev>` | `BLKRRPART`：让内核重读分区表 |
| `lsdev <前缀>` | 枚举 `/dev` 下的节点（含主次设备号） |
| `fstype <路径>` / `cmpdev <A> <B>` | 看这个路径属于哪个文件系统 / 两者是否同一实例 |
| `attach <文件> [--partscan]` / `detach` | 把文件绑成 loop 设备（等价于"插卡/拔卡"） |
| `lab` / `disklab` | 跑完整流程 |

归档输出：`results-loop.log`（loop 镜像）、`results-disk.log`（真实 NVMe 空盘）。

---

## 7. 坑表

| # | 坑 | 真相 |
|---|---|---|
| 1 | 以为破坏分区表后分区节点会立刻消失 | 不会。必须 `BLKRRPART`/`partprobe`/重启/拔插让内核重扫 |
| 2 | `BLKRRPART` 在 loop 设备上返回 `EINVAL`（实测 errno=22） | loop 设备**没设 `LO_FLAGS_PARTSCAN`** 时不支持重扫。用 `losetup -P` 或 `LOOP_CONFIGURE` 带上 partscan |
| 3 | 用 `statfs().f_type` 判断 `/dev` 是不是 devtmpfs | 实测是 `TMPFS_MAGIC`（devtmpfs 借 shmem 实现）。看 `/proc/mounts` 的 type 字段 |
| 4 | 以为 `dmesg` 里能看到分区表解析过程 | 看不到：启动早期的日志早就被环形缓冲冲掉了（见 02-log-levels） |
| 5 | 以为"分区表 = 分区数据" | 分区表只在 LBA0（GPT 还有盘尾备份），只占几百字节；真正的数据在分区区间里。改签名 ≠ 删数据 |
| 6 | 写死次设备号 | 实测同一张盘的 `loop1p1` 销毁重建后从 `259:1` 变成 `259:3`，minor 是动态分配的 |
| 7 | 以为分区节点没了文件系统就不能用了 | 已挂载的文件系统靠 superblock 活着，不靠 `/dev` 节点 |
| 8 | MBR 能随便用 | MBR 只有 4 个主分区、最大 2 TiB；超过就得 GPT |
| 9 | `partprobe` 一定成功 | 分区正被使用（挂载/swap/LVM）时内核会拒绝重扫，先卸载 |
| 10 | 拿真实根盘做破坏实验 | 千万别。用 loop 镜像或确认全空的盘，跑完必须复原（本实验第 5 步清零 LBA0） |

---

## 8. 衔接：下一站字符设备

这一章说的是**别人（内核块设备层）**怎么在 `/dev` 下冒出文件。
下一章 `01-first` 之后、主线 `04-char-device` 要做的，是**你自己**在 `/dev` 下冒出文件：

```
本章（被动/块设备）          下一阶段（主动/字符设备）
  驱动探测硬件                 module_init
  add_disk()                   register_chrdev() / cdev_add()
  内核解析分区表                device_create()  ← 这一步才让 /dev/xxx 出现
  devtmpfs 建节点              devtmpfs 建节点（同样是内核干的）
```

共同点是：**`/dev` 下的文件永远是内核对象的"门牌号"**，不是磁盘上的实体。
区别只在于这个门牌号背后挂的是块设备、还是你写的 `file_operations`。

---

## 9. 自测

<details>
<summary>Q1：SD 卡上到底存了什么，使得内核知道有 p1、p2 两个分区？</summary>

LBA0 的 MBR：446 字节引导代码 + 4×16 字节分区项（起始 LBA、扇区数、类型）+ 4 字节磁盘签名
+ 2 字节 `0x55AA`。内核看到 `0x55AA` 就按 MBR 解析，为每个非空项建一个分区设备。
本卡实测：p1 = LBA 16384 + 1048576 扇区（FAT32），p2 = LBA 1064960 + 61268992 扇区（ext4）。
</details>

<details>
<summary>Q2：`/dev/mmcblk0p1` 这个名字，SD 卡里存了吗？</summary>

没有。分区项的 16 字节里全是数字，没有任何名字字段（实测 hex：
`00 00 01 80 0c 03 e0 ff 00 40 00 00 00 00 10 00`）。名字是内核按
"驱动前缀 + 探测序号 + `p` + 表项序号" 现拼的，存在 devtmpfs（内存）里。
</details>

<details>
<summary>Q3（Quiz）：破坏分区表后节点还在吗？</summary>

分情形（实测）：刚破坏、未重扫 → 整盘在、**分区节点也还在**；重扫之后 → 整盘在、
分区节点消失；卡坏到驱动认不出 → 两个都没。把 `0x55AA` 写回去再重扫 → 分区节点回来。
</details>

<details>
<summary>Q4：为什么分区节点消失后，已经挂载的根分区还能继续用？</summary>

挂载关系指向 superblock，不指向 `/dev` 下的文件。`/dev/mmcblk0p2` 只是个门牌号，
`mount` 一次之后内核就自己持有 sb 了，门牌号没了不影响已经建立的挂载。
</details>

<details>
<summary>Q5：同一张 SD 卡在别的机器上叫 `/dev/sdb1`，是不是分区表变了？</summary>

分区表一个字节都没变。变的是驱动前缀：走 `mmc` 驱动叫 `mmcblk0p1`（major 179），
走 USB 的 `sd` 驱动叫 `sdb1`（major 8）。major 号是驱动注册的，序号是探测顺序。
</details>

<details>
<summary>Q6：怎么证明 `/dev` 不在 SD 卡上？</summary>

`stat("/").st_dev` = `179:2`（mmcblk0p2，SD 卡上的 ext4），
`stat("/dev").st_dev` = `0:6`（匿名 devtmpfs）。`st_dev` 不同 = 不同的文件系统实例；
再配合 `df -h /dev` 显示 `udev 3.9G 0%`（占内存），以及 `umount` 之后 `/dev` 里啥也没有。
</details>
