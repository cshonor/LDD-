# 04-char-block-net · 三种设备，三个最小例子

> **本节目标**：上一章（00-dev-and-partitions）只看了设备节点"长什么样"；
> 这一章把 **字符 / 块 / 网络** 三类设备各写一个能跑的最小例子，
> 亲手体会"同一个 open/read/write 接口，内核里是完全不同的机器"。
>
> **实测环境**：树莓派 5（aarch64），Debian 13 trixie，内核 `6.18.39+rpt-rpi-2712`。
> 三个例子的实测输出见各节「实测记录」。

---

## 0. 三类设备一句话定位

| 类型 | 访问单位 | /dev 节点 | 内核入口 | 本目录例子 |
|------|----------|-----------|----------|------------|
| **字符设备** | 字节流 | 有 | `file_operations` 回调 | `char/misc_echo.c` |
| **块设备** | 512B–4KB 块 | 有 | blk-mq `queue_rq`（请求队列） | `block/ramdisk.c` |
| **网络设备** | 报文（skb） | **没有** | socket / 网络协议栈 | `net/sniff.c`（用户态视角） |

关键记忆点：

- 字符设备：应用调一次 `read` → 内核**立刻回调你的函数**，流式、不可 seek。
- 块设备：应用 `write` 先进**页缓存**（不属于你），块层攒成 request 才到你手上；可随机寻址。
- 网络设备：`ls /dev` 里没有 eth0 —— 它根本不进"设备文件"体系，走 socket。

### 0.1 常见静态主号速查表（+ 一条修正）

官方静态预留的常见主号（`Documentation/admin-guide/devices.txt`）：

| 主号 | 驱动 | 常见节点示例 |
|------|------|--------------|
| 4 | tty（串口/控制台） | `ttyS0` = 4:64、`ttyS1` = 4:65 |
| 29 | fb 帧缓冲（屏幕） | `fb0` = 29:0 |
| 116 | ALSA 音频 | `controlC0` / `pcmC0D0c`（录音）/ `pcmC0D0p`（播放） |
| 179 | mmc 块设备 | `mmcblk0` = 179:0、`mmcblk0p1` = 179:1（分区靠次号） |

两条修正（对照"主号=硬件大类"的常见说法）：

1. **主号定位的是驱动，不是硬件大类**。major=10（misc）下面挂着完全不相干的设备——
   本章 `misc_echo` 是 10:264，和它同主号的可能是鼠标、也可能是随机数发生器。
2. **静态表只覆盖一半现实**。现代驱动多数动态申请主号（本章 ramdisk 拿到 253），
   卸载重载不保证还是同一个号——用户态脚本别硬编码动态主号，现场读
   `lsblk` / `cat /proc/devices`。

一句话记忆：**主号找驱动，次号找该驱动下的第几个实例（设备/分区）。**

构建坑（Pi 实测踩过）：顶层 Makefile 的目标名 `char`/`block` 与同名目录冲突时，
GNU make 会把目录当"已存在的目标"直接跳过——必须 `.PHONY` 声明，否则只编译了用户态程序、
两个 `.ko` 根本没生成。

---

## 1. char/ —— misc 字符设备 echo

`misc_echo.c`：注册一个 misc 设备 `/dev/misc_echo`（mode 0666），
写入的最后一串字节被保存，read 原样吐回。

看点：

- 字符设备的全部核心就是一张 `struct file_operations` 函数表
- `copy_to_user / copy_from_user`：用户指针绝不能直接解引用
- `read` 返回 0 = EOF；`*ppos` 由内核代管，fd 才携带文件位置
- misc 封装 = 字符设备三件套（alloc_chrdev_region / cdev / device_create）的一行版

实测步骤：

```bash
make char userspace
sudo insmod char/misc_echo.ko
ls -l /dev/misc_echo          # major 10（misc 固定），minor 动态
./char/test_char              # write → read → EOF
sudo dmesg | tail -2
sudo rmmod misc_echo
```

### 实测记录（Pi 5, 6.18.39）

```
crw-rw-rw- 1 root root 10, 264 Sep 19 15:57 /dev/misc_echo
write() -> 20 bytes
read()  -> 20 bytes: "hello from userspace"
read again -> 0 bytes (0 = EOF)
[1050830.834974] misc_echo: /dev/misc_echo ready
```

major=10 正是 misc 的固定主设备号（对照 `/proc/devices` 的 `10 misc` 行），
minor=264 是内核动态分配的——与 00 章看到的"major 找驱动、minor 找设备"完全吻合。

---

## 2. block/ —— blk-mq 内存盘

`ramdisk.c`：一块 4 MiB 内存当盘（`/dev/lxx-ramdisk`），
能被 `lsblk` 看到、能 `dd` 读写、能 `mkfs`——块设备的"盘的样子"它全有。

看点：

- 块驱动不实现 read/write，实现的是 `queue_rq`：**request 到了 → 搬数据**
- `rq_for_each_segment` 遍历 request 的所有 bio 段，`kmap_atomic` 临时映射页缓存页
- 三个注册层次：tag set（工位）→ gendisk（盘）→ `device_add_disk`（上线）
- **6.18 的 blk-mq API 变了**（相对老教材/6.12 前的写法，本例已按新 API 写）：
  - `map_queues` 收 `struct blk_mq_tag_set *`（不再是 `blk_mq_queue_map *`）
  - `BLK_MQ_F_SHOULD_MERGE` 已删——合并由块层总是执行
  - `blk_mq_alloc_disk(set, lim, queuedata)` 三参，`lim=NULL` 走默认限制
  - 清理用 `blk_mq_free_tag_set`（`blk_mq_cleanup_tag_set` 不存在）
- 对照 00 章：这里的 major 是 `register_blkdev(0, ...)` 动态分的，
  `lsblk` 里能看到 major:minor 成对出现

实测步骤：

```bash
make block
sudo insmod block/ramdisk.ko
lsblk | grep lxx
head -c 1M /dev/urandom > /tmp/r1
sudo dd if=/tmp/r1 of=/dev/lxx-ramdisk bs=1M count=1
sudo dd if=/dev/lxx-ramdisk of=/tmp/r2 bs=1M count=1
cmp /tmp/r1 /tmp/r2 && echo DATA-OK      # 写进去 == 读出来
sudo rmmod ramdisk                        # 内存盘：卸载即数据消失
```

### 实测记录（Pi 5, 6.18.39）

```
lxx-ramdisk 253:1    0     4M  0 disk
1048576 bytes (1.0 MB, 1.0 MiB) copied, 0.00107618 s, 974 MB/s
1048576 bytes (1.0 MB, 1.0 MiB) copied, 0.00111447 s, 941 MB/s
DATA-OK
[1050846.119663] ramdisk: /dev/lxx-ramdisk ready, 4MiB, major=253
```

`cmp` 无输出且 `DATA-OK` 打印 = 写入/读回逐字节一致。
注意 lsblk 显示的 `253:1`：major=253 是 `register_blkdev(0,...)` 动态拿的，
minor=1 对应 `first_minor=1`——00 章的"动态 major"现场版。

---

## 3. net/ —— AF_PACKET 看网络设备

`sniff.c`：用户态原始套接字，绑到指定网卡收 N 个以太网帧并打印
源/目的 MAC 与以太类型。

看点：

- `ls /dev` 找不到 eth0 —— 网络设备不走设备文件体系
- `socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))` 直接挂到链路层，
  相当于站在驱动出口看报文
- 每次 recv 收到的是**完整一帧**：报文是原子的，这是它与字节流的本质区别
- 需要 root（cap_net_raw），和 misc_echo 的 0666 形成对比

实测步骤：

```bash
gcc net/sniff.c -o net/sniff
sudo ./net/sniff wlan0 5      # 跑的同时 ping 网关喂包（Pi 5 的 eth0 默认 DOWN）
```

### 实测记录（Pi 5, 6.18.39）

```
sniffing on wlan0 (idx=3), waiting for 5 packets...
#1   66 B  98:fe:54:0f:8f:08 -> 32:30:f9:38:2a:45  type=0x0800 (IPv4)
#2   98 B  98:fe:54:0f:8f:08 -> 4c:c6:4c:ce:82:00  type=0x0800 (IPv4)
#3   98 B  4c:c6:4c:ce:82:00 -> 98:fe:54:0f:8f:08  type=0x0800 (IPv4)
#4   98 B  98:fe:54:0f:8f:08 -> 4c:c6:4c:ce:82:00  type=0x0800 (IPv4)
#5   98 B  4c:c6:4c:ce:82:00 -> 98:fe:54:0f:8f:08  type=0x0800 (IPv4)
```

同一对 MAC 反复出现：一个是 Pi 的 wlan0，一个是网关——这就是"没有 /dev 节点
也能收到设备的数据"的现场证明；帧长 66/98 B 是 ICMP echo 请求/响应的真实尺寸。

---

## 4. 与其他章节的衔接

- **00-dev-and-partitions**：major:minor、/dev 节点从哪来 —— 本章亲眼制造了一个 char 和一个 block
- **01-first**：hello.ko 的生命周期骨架（init/exit/kbuild）本章全部复用，只是注册的东西从 printk 换成了设备
- **后续 05-char-device**（待开）：把 misc 换成完整三件套 + `llseek`/`poll`/`ioctl`
- **05-linux-kernel Ch14/16（块 IO 与页缓存）**：ramdisk 的 request 在 blk 层怎么排队合并，LKD3rd 笔记有全图

## 5. 清理

```bash
make clean        # 清模块与用户态二进制
sudo rmmod ramdisk misc_echo   # 若还挂着
```

## 6. 自测题：主/次设备号

> 出题点全部来自本仓库实测现象：00 章的 nvme 分区、本章 ramdisk 的动态 major。

### 题 1（静态预留 + 分区次号）

在 Pi 上执行 `ls -l /dev/mmcblk0p2`，看到：

```
brw-rw---- 1 root disk 179, 2 ... /dev/mmcblk0p2
```

问：主号 179 定位到什么？次号 2 与 `mmcblk0`（179:0）是什么关系？
为什么权限位前面的字符是 `b` 而不是 `c`？

<details><summary>答案</summary>

- 主号 179 → **mmc 块设备驱动**（内核官方静态预留段，见
  `Documentation/admin-guide/devices.txt`）。
- 次号 2 = `mmcblk0` 整盘（179:0）之后的**第 2 个分区**（即 `mmcblk0p2`）——
  同一套驱动靠次号同时管理"整盘 + 各分区"，这正是"主号找驱动、次号找实例"的现场版。
- `b` = block device，`c` = character device；`ls -l` 的第一个字符直接标注
  该节点属于哪一类（00 章里 nvme 节点同样是 `b` 开头）。
</details>

### 题 2（动态主号 vs 静态预留）

本章 ramdisk 加载后 `lsblk` 显示：

```
lxx-ramdisk 253:1    0     4M  0 disk
```

问：253 在内核官方静态表里查得到吗？
"块设备主号固定"的老教材说法为什么现在要打折扣？
卸载重载模块后 253 还会是 253 吗？

<details><summary>答案</summary>

- 查不到——253 是 `register_blkdev(0, ...)` 向内核**动态申请**的；
  静态表只覆盖官方预留段（如 mmc 的 179、tty 的 4）。
- 现代驱动大量走动态分配，主号不能硬编码进用户脚本/udev 规则，
  应该现场读 `lsblk`、`/proc/devices` 或用 udev 的动态查询。
- 不保证。动态号分配规则是"拿下一个可用的"，
  取决于当时系统里其他动态驱动的占用情况，重载后可能变号。
  （对照：misc_echo 的 major 永远是 10——misc 框架是静态预留的，变的只是 minor。）
</details>
