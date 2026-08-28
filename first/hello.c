#include <linux/module.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");

static int my_init(void)
{
    printk("Hello: Hello, Kernel\n");
    return 0;
}

static void my_exit(void)
{
    printk("Hello: Goodbye, Kernel\n");
}

module_init(my_init);
module_exit(my_exit);
