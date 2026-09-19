# 08-gpio · GPIO 引脚控制与中断

> **本节目标**：把驱动从"纯软件"接到真硬件——GPIO 输出控制、GPIO 中断。
> **实测环境**：Pi 5（RP1 南桥），内核 `6.18.39`。

## ⚠️ Pi 5 的 GPIO 全局编号坑（实测踩过）

Pi 5 的 40-pin 不在 SoC 上而在 **RP1** 芯片，legacy 全局号 = gpiochip base + 线号：

```
/sys/class/gpio/gpiochip569 (pinctrl-rp1, base=569)  →  40-pin GPIO17 = 569+17 = 586
```

直接用 17 会 `-EBUSY`（insmod Unknown error 517）。
老款 BCM2711（Pi 4 及更早）才是直接 17。两个模块默认 586，
其他板子用 `gpio=<全局号>` 模块参数覆盖。

## 例子 1：`gpioblink.c` —— 输出控制

`gpio_request_one`（申请+方向+初值）→ `gpio_set_value` 翻转 →
`gpio_get_value` **读回实际电平**验证 → 复位释放。

### 实测记录

```
blink: round 0 set=0 read=0
blink: round 1 set=1 read=1
blink: round 2 set=0 read=0
blink: round 3 set=1 read=1
blink: done, gpio586 released
```

想肉眼可见：40-pin 第 11 脚（GPIO17）串 330Ω 接 LED 到 GND，
insmod 时会闪两下（高低高低各 300ms）。

## 例子 2：`gpioirq.c` —— 中断（无需接线的自触发验证）

传统验证中断要按键/跳线，这里用更干净的招：
**引脚设为输出，自己翻转电平**——GPIO 控制器的输入同步器看到真实边沿，
中断照样触发。6 次翻转 = 6 个边沿 = handler 应命中 6 次。

```
$ cat /dev/gpioirq
irq_count=6
$ dmesg
gpioirq: 6 toggles done, hit_count=6 (expect 6, irq=191)
```

`irq=191`：`gpio_to_irq(586)` 映射出的中断号，每次开机可能不同（又是动态分配）。

**中断上下文的纪律**（本例 handler 里只有 atomic 计数）：

- 不能睡眠：不能 mutex、不能 `kmalloc(GFP_KERNEL)`、不能 msleep
- 越短越好：耗时活推迟到下半部（threaded_irq / workqueue，后续章节）
- `request_irq` 的 flags：`IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING` 双边沿

```bash
make && sudo insmod gpioblink.ko && sudo dmesg | grep blink
sudo insmod gpioirq.ko && cat /dev/gpioirq && sudo rmmod gpioirq
```

## 衔接

- 中断上半部/下半部机制 → LKD3rd Ch10 笔记有全图，下一章可做 workqueue 版
- 产品代码应迁到 gpiod 描述符 API（`gpiod_get/gpiod_set_value`），
  传统 `gpio_*` 在设备树绑定齐全的正式驱动里已不推荐
