// gpioblink.c —— GPIO 输出控制（阶段四之一）
//
// 模块参数 gpio=<编号>（默认 17，Pi 5 的 40-pin 第 11 脚）。
// init：申请引脚 → 输出方向 → 低高低高翻转 4 次（每次读回实际电平）→ 释放前复位
// 验证手段（无 LED 也行）：dmesg 读回电平；接了 LED（串联 330Ω 到 GND）就肉眼可见闪。
//
// 注：教学用传统 gpio_* 接口（无设备树绑定也能跑）；产品驱动推荐 gpiod 描述符 API。

#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/delay.h>

/* Pi 5: RP1 gpiochip 全局 base=569，40-pin GPIO17 = 569+17 = 586（gpiochip-base 可查
 * /sys/class/gpio/gpiochipN 的 base 文件）。老款 BCM2711 Pi 上直接就是 17。 */
static int gpio_num = 586;
module_param(gpio_num, int, 0444);
MODULE_PARM_DESC(gpio_num, "GPIO number to blink (default 17)");

static int __init blink_init(void)
{
    int ret, i, v;

    ret = gpio_request_one(gpio_num, GPIOF_OUT_INIT_LOW, "lxx-blink");
    if (ret) {
        pr_err("blink: gpio%d request failed: %d\n", gpio_num, ret);
        return ret;
    }

    for (i = 0; i < 4; i++) {
        gpio_set_value(gpio_num, i & 1);
        msleep(300);                       /* 进程上下文，可以睡 */
        v = gpio_get_value(gpio_num);      /* 读回实际电平做验证 */
        pr_info("blink: round %d set=%d read=%d\n", i, i & 1, v);
    }

    gpio_set_value(gpio_num, 0);           /* 离场前复位为低 */
    gpio_free(gpio_num);
    pr_info("blink: done, gpio%d released\n", gpio_num);
    return 0;
}

static void __exit blink_exit(void)
{
    pr_info("blink: exit\n");
}

module_init(blink_init);
module_exit(blink_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIO output blink demo (one-shot in module init)");
