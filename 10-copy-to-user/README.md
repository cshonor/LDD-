# 10-copy-to-user · 为什么不能直接解引用用户指针

> **本节目标**：`copy_to_user` / `copy_from_user` 从 04 章起每章都在用，但一直是"会用"。
> 本章回答三个为什么：为什么不能 `*ubuf` 直接取值、`access_ok` 到底检查了什么、
> 拷贝失败时内核靠什么兜底。
>
> **实测环境**：树莓派 5（aarch64），Debian 13 trixie，内核 `6.18.39+rpt-rpi-2712`。
> 前置章节：[04-char-block-net](../04-char-block-net/README.md)（copy_to_user 首次出现）。
>
> ⏳ **状态：文档完成，实验代码已备，真机实测待回填**（`experiments/deref-direct`）。

---

## 本节讲什么

| 问题 | 一句话答案 | 展开 |
|------|-----------|------|
| 为什么不能 `*ubuf`？ | 三个独立理由：缺页、安全、硬件（PAN） | 第 1 节 |
| `access_ok` 查什么？ | **只查范围**（< TASK_SIZE），不查页面是否存在 | 第 2 节 |
| 拷贝到一半缺页怎么办？ | 异常表（extable）兜底，返回"还剩几个字节没拷" | 第 3 节 |
| 拷贝的真实成本？ | syscall + 检查 + PAN 切换 + 内存带宽 → mmap 零拷贝的动机 | 第 4 节 |

---

## 1. 不能 `*ubuf` 的三个独立理由

驱动 `read` 收到的 `char __user *ubuf` 是**用户进程地址空间里的虚拟地址**。
`__user` 只是个标记（sparse 检查用），对编译器没有任何约束力——直接解引用能编译通过，
但运行时可能死。三个理由互相独立，任何一个成立都足够判死刑：

### 1.1 缺页：那个地址此刻可能根本没有物理页

用户指针指向的页可能：还没被 lazy allocation 映射、被 swap 换出、或者干脆是野指针。

- 用户态访问它 → 缺页异常 → fault handler 换入/分配，正常流程。
- **内核态**直接解引用 → 同样触发缺页，但这次 fault 发生在内核上下文、
  又不在任何"允许缺页"的白名单里 → **oops**：
  `Unable to handle kernel paging request at virtual address ...`，当前进程被杀。

### 1.2 安全：用户程序可以传一个指向内核空间的地址

如果 `read` 傻乎乎地 `memcpy(buf, kbuf, n)`，用户传 `buf = 0xffff8000...`（内核地址），
内核就把自己的内存拷给了用户——**经典内核信息泄露漏洞**（infoleak）。
所以 uaccess 第一步永远是 `access_ok` 范围检查：用户缓冲区必须完全落在用户地址空间内
（ARM64 48 位 VA 下即 `< TASK_SIZE`）。

### 1.3 硬件：ARM64 PAN 直接禁止内核访问用户页

就算地址合法、页面在内存里——**硬件层面内核态默认就是不许碰用户页**。

- ARMv8.1 起有 **PAN**（Privileged Access Never）：PSTATE.PAN=1 时，内核态（EL1）
  访问任何用户映射的页，直接 permission fault。Cortex-A76（Pi 5）支持且默认启用。
- x86 的对应物是 **SMAP**（supervisor mode access prevention），靠 `stac`/`clac` 开关。
- `copy_to_user` 内部在拷贝前临时清 PAN（`uaccess_enable`）、拷完恢复（`uaccess_disable`）。
  **绕开 uaccess 路径自己解引用，等于撞在硬件墙上。**

> 这三层正好对应三个时代：缺页问题（一直有）→ access_ok 安全检查（软件防线）→
> PAN/SMAP（硬件防线，Meltdown 之后普及）。[实验](experiments/deref-direct/README.md)
> 就是在 PAN 机器上亲眼看看直接解引用死成什么样。

---

## 2. `access_ok` 的常见误解：它不查"页面在不在"

```c
if (!access_ok(ubuf, cnt))
    return -EFAULT;
```

`access_ok` 只做**范围检查**：`ubuf` 到 `ubuf+cnt` 是否整个落在用户地址空间内（防 1.2 的泄露）。
它**不检查**这些页是否已映射、是否在物理内存里——那是缺页异常和异常表的活（见第 3 节）。

| 检查 | 谁做 | 时机 |
|------|------|------|
| 地址范围合法（在用户空间内） | `access_ok` | 拷贝前 |
| 页面已映射 / 换入 | 缺页异常 handler | 拷贝中（按需） |
| 拷贝失败优雅收场 | 异常表（`__ex_table`）fixup | 拷贝中 |

> 推论：`access_ok` 通过了，拷贝照样可能失败（页面不存在）。所以返回值**必须检查**，
> 两个检查是互补关系，不是重复。

---

## 3. 异常表：uaccess 的"安全气囊"

`copy_to_user` 的拷贝指令（ARM64 上是 `ldtrb`/`sttrb` 这类 unprivileged load/store）
的地址，编译时被登记进内核的 **`__ex_table`**（异常表）。一旦拷贝中发生缺页：

1. fault handler 查到 fault 地址在 `__ex_table` 里 → 不走 oops 流程
2. 跳到对应的 **fixup 代码**：把"还没拷完的字节数"算出来作为返回值
3. `copy_to_user` 把这个数返回给调用者

所以返回值的语义是 **未拷贝的字节数**，0 = 全部成功：

```c
if (copy_to_user(ubuf, kbuf, cnt))   /* 非零 = 有字节没拷过去 */
    return -EFAULT;
```

> 这也是 04/05/06/07 章所有 `read`/`write` 都用 `if (copy_to_user(...)) return -EFAULT;`
> 这个固定写法的原因。更精细的做法是用剩余字节数算出实际拷了多少、返回 partial，
> 但教学代码里直接 `-EFAULT` 即可。

## API 速查

| 接口 | 用途 | 返回值 |
|------|------|--------|
| `copy_to_user(to, from, n)` | 内核 → 用户 | 未拷贝字节数 |
| `copy_from_user(to, from, n)` | 用户 → 内核 | 未拷贝字节数 |
| `put_user(v, ptr)` / `get_user(v, ptr)` | 单个标量（1/2/4/8 字节） | 0 / `-EFAULT` |
| `__copy_to_user(...)` | 跳过 `access_ok` 的版本 | 未拷贝字节数 |
| `strncpy_from_user(dst, src, n)` | 拷贝用户字符串 | 已拷长度 / `-EFAULT` |

`__copy_` 前缀版只在**已经自己做过 access_ok**、且要分多段拷贝（避免重复检查）时用，
日常别碰。

---

## 4. 性能视角：一次 `read` 的真实成本

以 `cat /dev/xxx` 读 4KB 为例，开销分解：

| 环节 | 量级 | 能否消除 |
|------|------|----------|
| syscall 进出（用户↔内核上下文切换） | 数百 ns | `mmap` 后数据路径无 syscall |
| `access_ok` + PAN 切换 | 数十 ns | 同上 |
| 内存拷贝 4KB | 内存带宽决定，~百 ns | **零拷贝则根本不拷** |
| 页表/缓存副作用 | TLB、cache 污染 | — |

> 🔗 **HFT 关联**：行情/交易数据路径上，"一次拷贝 + 一次 syscall"就是不可接受的抖动源。
> 这就是为什么网卡厂商的低延迟栈（ef_vi、exanic）和 11 章的 `mmap` 方案，
> 都把 DMA 环形缓冲区直接映射进用户态——数据路径上**一个 syscall、一次拷贝都没有**，
> `copy_to_user` 只留在控制路径。本章理解拷贝成本，下章理解怎么绕开它。

---

## 5. 实验：直接解引用会死成什么样

见 [`experiments/deref-direct/`](experiments/deref-direct/README.md)：
同一个 misc 字符设备，唯一区别是 `read` 里不用 `copy_to_user`，直接 `*ubuf`。

⏳ 待真机验证。预期（ARM64 + PAN）：第一次 `cat` 即 permission fault → oops，
发起读的进程被杀，内核 taint 置 `D` 位；`rmmod` 后需重启才能清 taint。
如果竟然"活下来了"，说明该内核未启用 PAN——同样是有效结论。

---

## 6. 坑表

| # | 现象 | 原因 | 解决 |
|---|------|------|------|
| 1 | oops：`Unable to handle kernel paging request` | 驱动里直接解引用用户指针 | 一律 `copy_to_user` / `copy_from_user` |
| 2 | `copy_to_user` 明明页面在却返回非零 | 用户指针非法（越出用户空间/未映射） | 检查返回值，返回 `-EFAULT`；别当"不可能发生" |
| 3 | 以为 `access_ok` 通过就万事大吉 | 它只查范围，不查页面存在性 | 返回值仍必须检查（第 2 节） |
| 4 | 用 `memcpy` 拷贝用户数据"也能跑" | 测试机上恰好页面都在 + 没开 PAN | 这是未定义行为，换台机器就 oops；无任何理由这么写 |
| 5 | 老代码 `verify_area` / `segment` 相关编译失败 | 远古 API（2.0/2.2 时代）早已删除 | 现代内核就是 `access_ok` + `copy_*_user` |
| 6 | 在 uaccess 中混入 printk 大字符串导致行为怪异 | uaccess 临界区内应快进快出 | 拷贝和日志分开，先拷完再打印 |

---

## 7. 自测题

<details>
<summary>点开做题（先自己想，再看答案）</summary>

**Q1. `__user` 标记会阻止编译器生成直接解引用的代码吗？**
> 不会。`__user` 只是给 sparse（静态检查工具）看的注解，对 GCC 是空的。
> 直接 `*ubuf` 能编译通过，运行时靠缺页/PAN 出事。

**Q2. `access_ok` 通过后，`copy_to_user` 还可能返回非零吗？为什么？**
> 可能。`access_ok` 只查地址范围是否在用户空间，不查页面是否已映射；
> 页面不存在时拷贝中途失败，由异常表 fixup 返回剩余字节数。两者是互补检查。

**Q3. 用户给 `read` 传一个内核地址，`copy_to_user` 会怎么办？**
> `access_ok` 范围检查直接失败（地址 ≥ TASK_SIZE），返回全部字节数（未拷贝），
> 驱动按惯例返回 `-EFAULT`。这正是它要防的内核信息泄露。

**Q4. ARM64 上内核明明"有能力"映射所有内存，为什么直接读用户页还会 fault？**
> PAN（Privileged Access Never）。PSTATE.PAN=1 时 EL1 访问用户映射的页
> 触发 permission fault；uaccess 路径拷贝前临时清 PAN、拷完恢复。
> x86 的等价物是 SMAP + stac/clac。

**Q5. `copy_to_user` 拷贝中途缺页，为什么不会 oops？**
> 拷贝指令的地址登记在 `__ex_table`（异常表）里。fault handler 发现 fault 地址在表中，
> 跳到 fixup 代码算出剩余字节数作为返回值，而不是走 oops 流程。

**Q6. 什么时候用 `__copy_to_user`（带下划线版）？**
> 已经自己做过 `access_ok`、且需要分多段拷贝时，避免每段重复范围检查。
> 日常驱动直接用 `copy_to_user`，别用下划线版。

</details>

---

## 8. 衔接

- 用法回顾：[04-char-block-net](../04-char-block-net/README.md)（首次出现）、
  [05-char-device-full](../05-char-device-full/README.md)（ioctl 里的 `put_user`/`get_user` 场景）
- 下一步：[11-mmap]（待写）——`remap_pfn_range` 把内核缓冲区直接映射进用户地址空间，
  数据路径上彻底消灭 `copy_to_user` 和 syscall
