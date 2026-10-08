# 11-mmap · 内存映射零拷贝

> **本节目标**：10 章算了笔账——`read`/`write` 每次访问都是"一次 syscall + 一次拷贝"。
> 本章把这笔开销彻底消掉：用 `mmap` 把内核缓冲区**映射进用户地址空间**，
> 之后用户态访问它就是普通内存读写，**数据路径上一个 syscall、一次拷贝都没有**。
>
> **实测环境**：树莓派 5（aarch64），Debian 13 trixie，内核 `6.18.39+rpt-rpi-2712`。
> 前置章节：[10-copy-to-user](../10-copy-to-user/README.md)（要绕开的成本从哪来）。
>
> ⏳ **状态：代码与文档已完成，真机实测待回填**（跑 `test.sh` 后回填输出）。

---

## 本节讲什么

| 问题 | 一句话答案 | 展开 |
|------|-----------|------|
| mmap 之后用户态怎么访问内核内存？ | 页表条目指向**同一块物理页**，普通 load/store 直达 | 第 1 节 |
| 内核侧要做哪几件事？ | kmalloc 拿物理连续页 → `.mmap` 里 `remap_pfn_range` 填 PTE | 第 2 节 |
| 用户态要注意什么？ | `MAP_SHARED`、offset 页对齐、`munmap` 后再 `rmmod` | 第 3 节 |
| 和网卡零拷贝什么关系？ | 本章的 kbuf 就是微缩版 DMA ring buffer | 第 5 节 |

---

## 1. 两条路径的对比（承接 10 章第 4 节）

```
 read/write 路径（10 章）               mmap 路径（本章）
 ─────────────────────────────       ─────────────────────────────
 每次访问：                            一次性（mmap 时）：
   read(2) 陷入内核                      mmap(2) 陷入内核
   access_ok + PAN 切换                  remap_pfn_range 把 PTE 填好
   copy_to_user 拷一份                  ─────────────────────────
 每次访问都重复一遍                    之后每次访问：
                                        用户态直接 load/store
                                        = 同一块物理页，零 syscall 零拷贝
```

关键：**映射建立后内核完全退场**。用户态那 16KB 虚拟地址的页表条目，
直接指向内核缓冲区的物理页帧——读写不经过任何内核代码。

> 代价是没了"守门人"：共享内存上内核和用户态同时写就是数据竞争（第 4 节）。
> `read`/`write` 慢，但每次访问都过内核这道闸；`mmap` 快，但闸拆了，同步自己管。

---

## 2. 内核侧：三步把缓冲区交出去

### ① kmalloc 拿物理连续的页

```c
kbuf = kmalloc(BUF_SIZE, GFP_KERNEL);   /* 16 KB = 4 页，order-2 */
```

**为什么必须是 kmalloc 而不是 vmalloc**：`remap_pfn_range` 要一个起始物理页帧号（pfn），
`virt_to_phys(kbuf)` 能算出来，是因为 kmalloc 的内存在**线性映射区**——
虚拟地址和物理地址只差一个固定偏移。vmalloc 的内存物理页不连续、
页表映射关系任意，必须逐页 `vmalloc_to_page` 查（或干脆用 `remap_vmalloc_range`）。

### ② `.mmap` 回调里填页表

用户态 `mmap(2)` 陷入内核后，VFS 回调 `fops->mmap`，参数是已经建好的 `vm_area_struct`：

```c
static int mbuf_mmap(struct file *f, struct vm_area_struct *vma)
{
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long pfn = virt_to_phys(kbuf) >> PAGE_SHIFT;

    if (vma->vm_pgoff != 0 || size > BUF_SIZE)
        return -EINVAL;

    return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
}
```

`remap_pfn_range` 五个参数逐个看：

| 参数 | 含义 |
|------|------|
| `vma` | 内核已为这次 mmap 建好的虚拟内存区域 |
| `vma->vm_start` | 映射在用户地址空间的起始地址 |
| `pfn` | **物理页帧号**（物理地址 >> PAGE_SHIFT），不是物理地址本身 |
| `size` | 映射长度 |
| `vma->vm_page_prot` | 页保护属性（继承用户 mmap 时的 PROT_READ/WRITE） |

**它是一次性把整段 PTE 全填好**（eager），不是按需缺页——映射建立后访问零缺页中断。
大数据缓冲区的真实驱动有时反着来：注册 `.fault` 回调按需映射单页（lazy），
用初始化时间换内存利用率。本章缓冲区才 4 页，eager 一次填完最简单。

> **教程过时点**：v6.3 起 `remap_pfn_range` 内部自动置
> `VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP`。老教程里手工
> `vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;` 的写法已不需要。

### ③ SetPageReserved 保险

```c
for (i = 0; i < BUF_PAGES; i++)
    SetPageReserved(virt_to_page(kbuf + i * PAGE_SIZE));
```

把每页标记为保留：防止映射存续期间被内核内存管理回收/挪用。
kmalloc 的内存本就不可换出，这步是通行保险做法；`kfree` 前对应 `ClearPageReserved`。

---

## 3. 用户态：三个必须遵守的约定

```c
char *p = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
strcpy(p, "hello from userspace");      /* 直接写内核缓冲区 */
munmap(p, MAP_SIZE);
```

| 约定 | 违反后果 |
|------|----------|
| **必须 `MAP_SHARED`** | `MAP_PRIVATE` 是写时复制（COW）：读能看到内核内容，一写就拷贝到匿名页，**内核缓冲区永远收不到**——最隐蔽的"写了没效果" |
| **offset（第 6 参）必须页对齐**，内核侧体现为 `vma->vm_pgoff` | 不对齐直接 `EINVAL` |
| **`rmmod` 前必须 `munmap`**（或杀死持有映射的进程） | 模块 `kfree` 后用户页表还指着那块物理页，下次访问 = oops（第 6 节坑 #1） |

---

## 4. 共享之后：同步是自己的事

`mmap` 拆掉了内核这道闸，随之而来的是裸数据竞争：

- 用户态正在写一条消息，内核中断处理程序同时改同一块区域 → 撕裂（tearing）
- 没有 `copy_to_user` 那次"顺手"的原子拷贝当缓冲，编译器/CPU 乱序全都可见

真实方案是**环形缓冲区 + 内存屏障 + 单生产者单消费者（SPSC）约定**：
一方只写 head，另一方只写 tail，用 acquire/release 语义读对方的指针。
这正是 09 章中断下半部收数据、用户态 mmap 消费的经典组合，
也是 eBPF ringbuf、io_uring SQ/CQ、DPDK ring 的共同骨架——本章只立起
"共享内存需要同步协议"这个问题，环形缓冲区值得单独一章。

---

## 5. HFT / 嵌入式关联（本章的真正动机）

**HFT 方向**：低延迟网卡栈的核心手法就是本章的放大版——

| 方案 | 映射什么到用户态 | 效果 |
|------|------------------|------|
| Solarflare **ef_vi** / Xilinx **Onload** | NIC 的 DMA 收发包 ring buffer | 收包路径零拷贝零 syscall |
| **exanic** | 同上（ASIC 直接暴露） | 同上 |
| **DPDK / UIO / VFIO** | 整个网卡 BAR + DMA 大页 | 内核旁路（kernel bypass），驱动整个搬到用户态 |
| **AF_XDP (XSK)** | UMEM 区域 + 4 个 ring | 内核内 XDP 快路径 + 用户态零拷贝 |

本章的 `kbuf` 就是一个 16KB 的微缩 DMA ring：理解了"物理页帧 → PTE → 用户指针"
这条链，再看 `remap_pfn_range(vma, ..., pci_resource_start(pdev, 0) >> PAGE_SHIFT, ...)`
映射网卡 BAR 的真实驱动，只是换了个 pfn 来源。

**嵌入式方向**：帧缓冲（`/dev/fb0`）、V4L2 摄像头缓冲区、`/dev/mem` 寄存器映射，
全都是同一条 `remap_pfn_range` 路径。区别只在 pfn 来自 RAM 还是 MMIO——
MMIO 场景要换 `pgprot_noncached` / `pgprot_writecombine` 关掉缓存/合并写（第 6 节坑 #5）。

---

## 6. 坑表

| # | 现象 | 原因 | 解决 |
|---|------|------|------|
| 1 | `rmmod` 后用户程序一碰映射区就 oops | 模块已 `kfree`，用户页表还指着旧物理页 | 先 `munmap`/杀进程再 `rmmod`；真实驱动用 `vm_ops->open/close` 计引用 |
| 2 | mmap 后写数据，内核读不到 | 用了 `MAP_PRIVATE`（COW），写进了匿名页 | 改 `MAP_SHARED` |
| 3 | `mmap` 返回 `EINVAL` | offset 非页对齐 / 长度超缓冲区 | offset 必须 `PAGE_SIZE` 整数倍；内核侧查 `vm_pgoff` 和 `vm_end - vm_start` |
| 4 | `insmod` 偶发失败，`kmalloc` 返回 NULL | 大块连续物理页碎片化 | 教学用 ≤ 64KB；大块用 `alloc_pages` / CMA / 大页 |
| 5 | 映射 MMIO（寄存器/BAR）读写值不对 | 用了普通 RAM 的页属性，被 cache/乱序坑了 | MMIO 用 `pgprot_noncached()` 或 `pgprot_writecombine()` 替换 `vma->vm_page_prot` |
| 6 | 用 `vmalloc` 的缓冲区 + `virt_to_phys` 映射，数据错乱 | vmalloc 物理页不连续，线性换算不成立 | 逐页 `vmalloc_to_page` 或用 `remap_vmalloc_range` |
| 7 | 照老教程手工 `vma->vm_flags |= VM_PFNMAP...` 告警/冗余 | v6.3 起 `remap_pfn_range` 自动置这些位 | 删掉手工 flags 代码 |
| 8 | 内核和用户态同时写，数据撕裂 | 共享内存无同步 | 环形缓冲 + 内存屏障（见第 4 节） |

---

## 7. 自测题

<details>
<summary>点开做题（先自己想，再看答案）</summary>

**Q1. 说"mmap 零拷贝"，那 `mmap(2)` 这次 syscall 本身呢？**
> `mmap` 是一次性的**建立映射**开销（含 `remap_pfn_range` 填 PTE），
> 之后数据路径上的每次访问才是零 syscall 零拷贝。
> "零拷贝"说的是稳态数据路径，不是建链瞬间。

**Q2. 为什么 `remap_pfn_range` 要的是 pfn 而不是物理地址？**
> 页表条目里存的就是"页帧号 + 属性位"布局，物理地址低 12 位（页内偏移）不给 PTE。
> `phys >> PAGE_SHIFT` 得到 pfn；传给 `remap_pfn_range` 后由它组装 PTE。

**Q3. 用 `MAP_PRIVATE` 映射设备，读和写分别会发生什么？**
> 读：正常看到内核缓冲区内容（首次读触发 COW 源页映射）。
> 写：触发写时复制，写进匿名私有页——**内核缓冲区收不到**。
> 设备映射几乎永远要 `MAP_SHARED`。

**Q4. `virt_to_phys` 为什么不能用于 vmalloc 的内存？**
> 它做的是线性映射区的固定偏移减法。vmalloc 区物理页不连续、
> 映射关系是逐页建立的，必须逐页 `vmalloc_to_page` 查页表。

**Q5. 用户持有映射时 `rmmod`，接下来访问映射区为什么必死？**
> 模块退出 `kfree` 释放了物理页，但用户页表项还在，指向已归还给内核的页
> （可能已被别人分配改写）。访问即 use-after-free 级别的 oops。
> 正规做法：`vm_ops->open/close` 给映射计引用，阻止模块提前释放。

**Q6. 映射网卡 BAR（MMIO）时直接沿用 `vma->vm_page_prot` 有什么问题？**
> 默认页属性带 cache 和允许乱序/合并，MMIO 寄存器读写会被缓存和重排坑掉
> （读不到最新值、写被合并乱序到达）。要用 `pgprot_noncached()`（强序、不缓存）
> 或 `pgprot_writecombine()`（允许合并写，适合显存/大批量写）。

</details>

---

## 8. 文件清单

| 文件 | 作用 |
|------|------|
| `mmapbuf.c` | 模块：kmalloc 4 页 + `.mmap` 用 `remap_pfn_range` 映射；`.read` 作对照路径 |
| `test_mmap.c` | 用户态测试：open → mmap → 直接读写 → munmap |
| `Makefile` | kbuild 脚本 |
| `test.sh` | 真机验证脚本（含用 gcc 现编 test_mmap） |
| `artifacts/` | 真机产物归档（⏳ 待回填） |

---

## 9. 衔接

- 成本来源：[10-copy-to-user](../10-copy-to-user/README.md)（read/write 每次访问的开销拆解）
- 共享内存的同步问题 → 环形缓冲区 + 内存屏障（可成独立一章）；
  与 [09-irq-bottom-half](../09-irq-bottom-half/README.md) 组合：
  中断收数据写 ring，用户态 mmap 消费——一条完整的数据通路
