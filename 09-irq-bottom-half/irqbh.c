// irqbh.c —— 中断下半部：workqueue vs threaded_irq（阶段四之三）
//
// 08 章遗留的问题：handler（急件窗口）不能睡眠、不能 GFP_KERNEL，
// 慢活怎么办？本章给两个"外包"方案，一个模块里都实现了，module_param 切换：
//
//   mode=0  workqueue    : handler 里 schedule_work() 扔回条，慢活在进程上下文的工作线程跑
//   mode=1  threaded_irq : request_threaded_irq，handler 只 return IRQ_WAKE_THREAD，
//                          慢活在内核自动生成的 irq 线程里跑（每个中断一个专用线程）
//
// 两处"慢活"都是故意犯规动作——msleep(20) + kzalloc(GFP_KERNEL)。
// 这两行放进上半部 handler = 死机/锁中断，放下半部 = 合法。这就是本章全部重点。
//
// 自触发手法沿用 08 章：引脚输出自翻转，6 次翻转 = 6 边沿 = 6 中断。
//
// /dev/irqbh 读两行：irq_hits（上半部收到的中断数）/ bh_done（下半部处理完数）
//
// 教学点（实测会看到）：workqueue 模式下 bh_done 可能 < 6 —— 同一个 work
// 重复 schedule 会合并（pending 期间再 schedule 不排队）；threaded 模式
// 每次中断必跑一次线程，bh_done == 6。这差异本身就是两种方案的特性对比。

#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/slab.h>

static int gpio_num = 586;   /* Pi 5: rp1base(569) + 17；老 BCM2711 直接 17 */
module_param(gpio_num, int, 0444);
MODULE_PARM_DESC(gpio_num, "global GPIO number (Pi5 default 586)");

static int mode;             /* 0 = workqueue（默认）, 1 = threaded_irq */
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "0=workqueue, 1=threaded_irq");

static int irq_num;
static atomic_t irq_hits = ATOMIC_INIT(0);
static atomic_t bh_done  = ATOMIC_INIT(0);
static struct work_struct bh_work;

/* ---------- 下半部方案 A：workqueue（进程上下文，可睡） ---------- */
static void wq_fn(struct work_struct *w)
{
    int seq = atomic_inc_return(&bh_done);
    int hit = atomic_read(&irq_hits);

    /* 犯规双雄——上半部里干这两样是事故，这里（内核线程）完全合法 */
    char *buf = kzalloc(64, GFP_KERNEL);   /* 可以等内存 */
    msleep(20);                            /* 可以睡 */

    if (buf) {
        snprintf(buf, 64, "wq: processed irq#%d (hits=%d) in process ctx\n",
                 seq, hit);
        pr_info("irqbh: %s", buf);
        kfree(buf);
    }
}

/* ---------- 上半部：两种模式共用，只干"快进快出" ---------- */
static irqreturn_t hard_handler(int irq, void *dev)
{
    atomic_inc(&irq_hits);
    if (mode == 0) {
        schedule_work(&bh_work);   /* 扔回条就走，微秒级 */
        return IRQ_HANDLED;
    }
    return IRQ_WAKE_THREAD;        /* 交给 irq/ 线程 */
}

/* ---------- 下半部方案 B：threaded_irq 的线程部分 ---------- */
static irqreturn_t thread_fn(int irq, void *dev)
{
    int seq = atomic_inc_return(&bh_done);

    /* 同样的犯规双雄，线程上下文合法 */
    char *buf = kzalloc(64, GFP_KERNEL);
    msleep(20);
    if (buf) {
        snprintf(buf, 64, "thread: processed irq#%d in irq/%d thread\n",
                 seq, irq);
        pr_info("irqbh: %s", buf);
        kfree(buf);
    }
    return IRQ_HANDLED;
}

/* ---------- /dev/irqbh：读计数 ---------- */
static ssize_t bh_read(struct file *f, char __user *u,
                       size_t cnt, loff_t *ppos)
{
    char line[96];
    int n = snprintf(line, sizeof line,
                     "irq_hits=%d bh_done=%d mode=%s\n",
                     atomic_read(&irq_hits), atomic_read(&bh_done),
                     mode ? "threaded" : "workqueue");
    if (*ppos >= (loff_t)n)
        return 0;
    if (cnt > (size_t)n - *ppos)
        cnt = n - *ppos;
    if (copy_to_user(u, line + *ppos, cnt))
        return -EFAULT;
    *ppos += cnt;
    return cnt;
}

static const struct file_operations bh_fops = {
    .owner = THIS_MODULE,
    .read  = bh_read,
};

static struct miscdevice bh_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "irqbh",
    .fops  = &bh_fops,
    .mode  = 0444,
};

static int __init bh_init(void)
{
    int ret, i;

    INIT_WORK(&bh_work, wq_fn);

    ret = gpio_request_one(gpio_num, GPIOF_OUT_INIT_LOW, "lxx-irqbh");
    if (ret) {
        pr_err("irqbh: gpio%d request failed: %d\n", gpio_num, ret);
        return ret;
    }

    irq_num = gpio_to_irq(gpio_num);
    if (mode == 0)
        ret = request_irq(irq_num, hard_handler,
                          IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                          "lxx-irqbh", NULL);
    else
        ret = request_threaded_irq(irq_num, hard_handler, thread_fn,
                                   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                                   "lxx-irqbh", NULL);
    if (ret) {
        pr_err("irqbh: request irq failed: %d\n", ret);
        gpio_free(gpio_num);
        return ret;
    }

    /* 自触发：6 次翻转 = 6 边沿；翻转间隔 > 下半部耗时，看两种方案的到达差异 */
    for (i = 1; i <= 6; i++) {
        gpio_set_value(gpio_num, i & 1);
        msleep(120);
    }
    msleep(300);

    pr_info("irqbh: mode=%s 6 toggles done, irq_hits=%d bh_done=%d (irq=%d)\n",
            mode ? "threaded" : "workqueue",
            atomic_read(&irq_hits), atomic_read(&bh_done), irq_num);
    return misc_register(&bh_dev);
}

static void __exit bh_exit(void)
{
    misc_deregister(&bh_dev);
    free_irq(irq_num, NULL);
    cancel_work_sync(&bh_work);      /* wq 模式：等在途的 work 跑完再卸 */
    gpio_set_value(gpio_num, 0);
    gpio_free(gpio_num);
    pr_info("irqbh: unloaded, hits=%d done=%d\n",
            atomic_read(&irq_hits), atomic_read(&bh_done));
}

module_init(bh_init);
module_exit(bh_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIO irq bottom-half: workqueue vs threaded_irq, self-triggered");
