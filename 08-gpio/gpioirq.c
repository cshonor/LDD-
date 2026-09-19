// gpioirq.c —— GPIO 中断（阶段四之二）
//
// 妙处：不需要接线/按键——把引脚设为输出，自己翻转电平，
// GPIO 控制器的输入同步器会看到真实边沿，中断照样触发。
// （这是无硬件条件下验证中断最干净的招；真产品里引脚是输入接按键。）
//
// /dev/gpioirq：cat 它能读到中断计数。
// init：申请引脚(输出) → gpio_to_irq → request_irq(双边沿) → 翻转 6 次
// 期望：6 次翻转 = 6 个边沿 = handler 计数 6。

#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/atomic.h>

/* Pi 5: RP1 gpiochip 全局 base=569，40-pin GPIO17 = 569+17 = 586（gpiochip-base 可查
 * /sys/class/gpio/gpiochipN 的 base 文件）。老款 BCM2711 Pi 上直接就是 17。 */
static int gpio_num = 586;
module_param(gpio_num, int, 0444);
MODULE_PARM_DESC(gpio_num, "global GPIO number (Pi5 default 586 = rp1base+17)");

static int irq_num;
static atomic_t hit_count = ATOMIC_INIT(0);
static atomic_t in_handler = ATOMIC_INIT(0);

/* 中断上下文：不能睡眠！只能 atomic/变量访问，不能 printk 大量输出 */
static irqreturn_t irq_handler(int irq, void *dev)
{
    atomic_inc(&hit_count);
    atomic_set(&in_handler, 1);   /* 标记"中断真的进来了" */
    return IRQ_HANDLED;
}

static ssize_t qi_read(struct file *f, char __user *u,
                       size_t cnt, loff_t *ppos)
{
    char line[64];
    int n = snprintf(line, sizeof line, "irq_count=%d\n",
                     atomic_read(&hit_count));
    if (*ppos >= (loff_t)n)
        return 0;
    if (cnt > (size_t)n - *ppos)
        cnt = n - *ppos;
    if (copy_to_user(u, line + *ppos, cnt))
        return -EFAULT;
    *ppos += cnt;
    return cnt;
}

static const struct file_operations qi_fops = {
    .owner = THIS_MODULE,
    .read  = qi_read,
};

static struct miscdevice qi_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "gpioirq",
    .fops  = &qi_fops,
    .mode  = 0444,
};

static int __init qi_init(void)
{
    int ret, i;

    ret = gpio_request_one(gpio_num, GPIOF_OUT_INIT_LOW, "lxx-irq");
    if (ret) {
        pr_err("gpioirq: gpio%d request failed: %d\n", gpio_num, ret);
        return ret;
    }

    irq_num = gpio_to_irq(gpio_num);
    ret = request_irq(irq_num, irq_handler,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                      "lxx-gpioirq", NULL);
    if (ret) {
        pr_err("gpioirq: request_irq failed: %d\n", ret);
        gpio_free(gpio_num);
        return ret;
    }

    /* 翻转 6 次 = 6 个边沿；中断是异步的，给内核一点送达时间 */
    for (i = 1; i <= 6; i++) {
        gpio_set_value(gpio_num, i & 1);
        msleep(120);
    }
    msleep(200);

    pr_info("gpioirq: 6 toggles done, hit_count=%d (expect 6, irq=%d)\n",
            atomic_read(&hit_count), irq_num);
    pr_info("gpioirq: /dev/gpioirq ready (cat it)\n");
    return misc_register(&qi_dev);
}

static void __exit qi_exit(void)
{
    misc_deregister(&qi_dev);
    free_irq(irq_num, NULL);
    gpio_set_value(gpio_num, 0);
    gpio_free(gpio_num);
    pr_info("gpioirq: unloaded, total hits=%d\n", atomic_read(&hit_count));
}

module_init(qi_init);
module_exit(qi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIO interrupt self-triggered demo");
