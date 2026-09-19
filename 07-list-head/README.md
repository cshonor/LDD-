# 07-list-head · 内核侵入式双向链表

> **本节目标**：吃透 `list_head`——内核最常用的数据结构，以及它与
> container_of、柔性数组的组合拳。**实测环境**：Pi 5，内核 `6.18.39`。

## 例子：`listmsg.c` —— 一个链表版消息队列字符设备

- **侵入式**：`struct list_head` 嵌在宿主结构体里，遍历用
  `list_for_each_entry`（内部 container_of 从节点指针反查宿主）
- **柔性数组**：`struct msg { struct list_head list; size_t len; char data[]; }`，
  节点+消息体 `kmalloc(sizeof(*m)+cnt)` 一次分配（呼应 01-ch1 的 1.5）
- **private_data 新玩法**：这次存的是**遍历游标**——每次 open 得到一个独立游标，
  多个 cat 互不干扰
- API：`LIST_HEAD` / `list_add_tail` / `list_for_each_entry_safe`（安全删除版）/
  `list_first_entry_or_null` / `list_is_last` + `list_next_entry`

| 操作 | 行为 |
|------|------|
| `echo xxx > /dev/listmsg` | 消息入链 |
| `cat /dev/listmsg` | 按序遍历输出 `N: msg` |
| ioctl `_IO('L',1)` | 清空并释放全部节点 |
| ioctl `_IOR('L',2,int)` | 查当前消息数 |

## 实测记录（Pi 5, 6.18.39）

```
$ printf "first message" > /dev/listmsg && printf "second msg" > /dev/listmsg && printf "third" > /dev/listmsg
$ cat /dev/listmsg
1: first message
2: second msg
3: third
$ (ioctl LISTMSG_CLEAR)
$ dmesg: listmsg: cleared 3 messages
$ cat /dev/listmsg
(empty)                     ← 清空生效，kmalloc 的节点全部释放
```

```bash
make && sudo insmod listmsg.ko
printf "first message" > /dev/listmsg
cat /dev/listmsg
sudo rmmod listmsg
```

## 衔接

- 驱动里"一套驱动管多实例"（05 章 cdev 嵌结构体）与本例"list 嵌消息"是同一模式
- 正是 01-c-language CH5 data-and-pointers 讲的"数据结构内嵌链接指针"思想的内核版
