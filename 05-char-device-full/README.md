# 05-char-device-full · 完整字符设备：三件套 + private_data + ioctl

> **本节目标**：把 04 章 `misc_register` 一行搞定的东西拆开——
> 字符设备完整三件套注册、多实例管理（private_data + container_of）、ioctl 三方向命令。
> **实测环境**：Pi 5（aarch64），内核 `6.18.39+rpt-rpi-2712`。

## 例子：`twodev.c`（一个驱动，两个实例，三种 ioctl）

三件套对应关系（misc 是它们的"一行版"）：

| 步骤 | misc 版 | 完整版 |
|------|---------|--------|
| 设备号 | 内部固定 10 | `alloc_chrdev_region` 动态拿 |
| fops 绑定 | 内部 cdev | `cdev_init` + `cdev_add` |
| /dev 节点 | misc 框架自动 | `class_create` + `device_create` |

两个核心机制：

1. **private_data + container_of**：`cdev` 嵌在 `struct twodev` 里，
   open 时 `container_of(ino->i_cdev, struct twodev, cdev)` 反查宿主，
   存进 `file->private_data`——之后每次 read/write/ioctl 取回来就是"本次打开的那个实例"。
   多实例隔离全靠这一招（正是 01-c-language CH2 container_of 的驱动实战）。
2. **ioctl 命令编码**：`_IOR/_IOW/_IOWR(幻数'W', 序号, 类型)`——
   方向+序号+载荷大小编进一个 32 位整数，防止不同驱动命令撞码。
   非本驱动命令返回 `-ENOTTY`。

## 实测记录（Pi 5, 6.18.39）

```
crw------- 1 root root 509, 0 ... /dev/twodev0
crw------- 1 root root 509, 1 ... /dev/twodev1
[/dev/twodev0] write 15 B
[/dev/twodev0] read  15 B: "hello-from-dev0"
[/dev/twodev1] read  15 B: "HELLO-FROM-DEV1"      ← 双实例互不串扰
[twodev0] after SET_BUFSZ=8, write 20 B -> accepted 8 B   ← _IOW 生效
[twodev0] GET_BUFSZ back = 8
[twodev0] DOUBLE(21) -> 42                       ← _IOWR 生效
```

注意 major=509：`alloc_chrdev_region` 动态分配的（0.1 节说的"动态主号"现场版）。

## 运行

```bash
make && sudo insmod twodev.ko && sudo ./test_twodev
sudo rmmod twodev
```

## 衔接

- 04 章的 misc_echo = 本章三件套的"简写"；06/07 章继续用 misc 但读端复杂化
- 多实例管理到 07 章 listmsg 会进一步用到 private_data 存**遍历游标**
