# 06-kernel-memory · 内核内存分配机制

> **本节目标**：内核态没有 malloc——搞清 kmalloc/kzalloc/vmalloc 三兄弟的
> 地址特征、适用场景与 GFP 标志。**实测环境**：Pi 5，内核 `6.18.39`。

## 例子：`memdemo.c`

init 时三种方式各分配一块，dmesg 打印地址、`/dev/memdemo` 输出报告：

| 分配器 | 特征 | 何时用 |
|--------|------|--------|
| `kmalloc(n, GFP_KERNEL)` | **物理连续**，直接映射区，小而快 | 默认选择；GFP_ATOMIC 用于中断上下文（不可睡） |
| `kzalloc(n, GFP_KERNEL)` | kmalloc + 清零 | 结构体初始化（省一次 memset） |
| `vmalloc(size)` | **虚拟连续、物理可碎**，VMALLOC 区 | 大块（≥1 页），慢，别在中断里 |

## 实测记录（Pi 5, 6.18.39）

```
kmalloc  64B  virt=ffff800003fcd600 phys=0x0000000003fcd600  (直接映射区,物理连续)
kzalloc  1KB  virt=ffff80014063bc00            (同 kmalloc,清零版)
vmalloc  1MB  virt=ffffc000857a8000            (VMALLOC 区,虚拟连续)
```

读数要点：

- kmalloc 的 virt − 固定偏移 = phys（直接映射区特征，`virt_to_phys` 可直接翻）；
  这就是它"物理连续"的体现
- vmalloc 的 `ffffc000...` 明显在另一个地址区段——物理页可以散落各处，
  页表把它们拼成连续虚拟区间
- kzalloc 分配的 `zero_p[0]=0`：dmesg 里可见"自动清零"生效

```bash
make && sudo insmod memdemo.ko && cat /dev/memdemo && sudo rmmod memdemo
```

## 衔接

- 07 章 listmsg 的消息节点就是 `kmalloc(sizeof(*m)+cnt)`（柔性数组一次分配）
- LKD3rd 内存管理章节是这里的原理底座
