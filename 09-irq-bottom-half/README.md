# 09 中断下半部：workqueue vs threaded_irq

> 收 08 章的伏笔：**急件窗口快进快出，慢件外包给后台同事**。
> 本章把"外包"的两种姿势都实现了，一个模块，参数切换，全部 Pi 5 实测。

## 0. 为什么需要下半部（30 秒版）

中断上半部（handler）运行在**中断上下文**：

- 不能睡眠（没有"当前进程"可挂起，睡着 = 整个中断线卡死）
- 不能用 `GFP_KERNEL` 分配（内存不够时要等 = 睡眠）
- 不能拿 mutex，只能 spinlock（自旋短暂等待，不睡）

但真实驱动的慢活一大把：DMA 缓冲整理、协议解析、延时去抖、写日志。
**方案：handler 只做"必须立刻做"的事（应答硬件、拷走数据、标记），其余打包丢给下半部。**

## 1. 两种外包姿势（irqbh.c，module_param 切换）

```
mode=0  workqueue（默认）
  handler: atomic_inc 计数 → schedule_work() 扔回条 → return IRQ_HANDLED
  慢活:    内核工作线程（进程上下文）跑 wq_fn —— 可睡、可 GFP_KERNEL

mode=1  threaded_irq
  request_threaded_irq(irq, hard_handler, thread_fn, ...)
  handler: return IRQ_WAKE_THREAD（什么都不干，只喊醒线程）
  慢活:    内核自动创建的 irq/191 专用线程跑 thread_fn
```

代码里的**犯规双雄**（`msleep(20)` + `kzalloc(GFP_KERNEL)`）故意放在下半部：
这两行放进上半部 handler 就是事故现场——这正是 08 章"handler 快进快出"的完整版答案。

两方案速查：

| | workqueue | threaded_irq |
|---|---|---|
| 上下文 | 共享的内核工作线程 | 每个中断一个专属 irq/N 线程 |
| 创建成本 | 低（复用 kworker） | 每中断一线程 |
| 时效保证 | 无（排队等 kworker 空闲） | 较强（专属线程） |
| 多中断合并风险 | **有**：同一 work pending 时重复 schedule 会合并 | 无：每次中断必跑线程 |
| 适用 | 一般延时任务、可以攒批的活 | 实时性要求高的中断（如 codec、传感器） |

## 2. 构建 & 实测（Pi 5, 6.18.39）

```bash
make
sudo insmod irqbh.ko            # mode=0 workqueue
cat /dev/irqbh                  # irq_hits=6 bh_done=6 mode=workqueue
sudo dmesg | grep irqbh
sudo rmmod irqbh

sudo insmod irqbh.ko mode=1     # threaded
cat /dev/irqbh                  # irq_hits=6 bh_done=6 mode=threaded
sudo dmesg | grep irqbh
sudo rmmod irqbh
```

### 实测记录

```
===== mode=0 workqueue =====
irq_hits=6 bh_done=6 mode=workqueue
wq: processed irq#1 (hits=1) in process ctx
...（共 6 条，间隔约 124ms = 120ms 翻转间隔 + 下半部处理）
mode=workqueue 6 toggles done, irq_hits=6 bh_done=6 (irq=191)

===== mode=1 threaded =====
irq_hits=6 bh_done=6 mode=threaded
thread: processed irq#1 in irq/191 thread     ← 线程名 irq/191 肉眼可见
...（共 6 条）
mode=threaded 6 toggles done, irq_hits=6 bh_done=6 (irq=191)
```

**两种模式都是 6 中断 = 6 处理**，因为翻转间隔（120ms）远大于下半部耗时（20ms），
workqueue 的合并特性没机会显形。想亲眼看合并：把 `msleep(120)` 改成 `msleep(5)` 再跑
mode=0——6 次 schedule 里 pending 期间的会被吞，`bh_done` 会小于 6，
而 mode=1 永远 6/6。这个"合并"不是 bug，是 workqueue 的设计特性（可以攒批），
驱动作者要知道它存在。

## 3. 关键记忆点

- **上半部判断标准**：这件事 5 微秒内干得完吗？干不完 → 下半部
- **workqueue**：进程上下文、可睡、会被合并；`schedule_work` + `cancel_work_sync`（卸载前）
- **threaded_irq**：专属线程、不被合并、handler 只需 `return IRQ_WAKE_THREAD`
- 下半部里 `GFP_KERNEL`/`msleep` 合法——**但 spinlock 依旧不能拿睡的版本**（mutex 可以）
- 卸载纪律：先 `free_irq` 再 `cancel_work_sync`，防卸载时 work 还引用已释放资源
