// misc_echo.c —— 最小字符设备例子（misc 封装）
//
// 字符设备的本质：实现一个 struct file_operations，
// 应用每次 read()/write() 这个设备节点，内核就回调你这里的函数。
// misc 是字符设备的"简化注册封装"：一行注册，自动出 /dev/misc_echo，
// 省掉自己 alloc_chrdev_region + cdev_init + device_create 三板斧。
//
// 行为：写入的最后一串字节会被保存，read 时原样吐回（echo）。

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define MSG_MAX 256

static char msg[MSG_MAX];          /* 内核侧缓冲区 */
static size_t msg_len;             /* 当前有效字节数 */
static DEFINE_MUTEX(lock);         /* 保护 msg，防并发写坏 */

/* 应用 read(/dev/misc_echo) 时被回调 */
static ssize_t ed_read(struct file *f, char __user *ubuf,
                       size_t cnt, loff_t *ppos)
{
    ssize_t ret;

    mutex_lock(&lock);
    if (*ppos >= msg_len) {        /* 读完了，返回 0 = EOF */
        ret = 0;
        goto out;
    }
    if (cnt > msg_len - *ppos)     /* 别多给 */
        cnt = msg_len - *ppos;

    /* 内核内存 → 用户内存 必须走 copy_to_user（用户指针不可直接解引用） */
    if (copy_to_user(ubuf, msg + *ppos, cnt)) {
        ret = -EFAULT;
        goto out;
    }
    *ppos += cnt;                  /* 文件位置推进 → 支持多次 read 拼接 */
    ret = cnt;
out:
    mutex_unlock(&lock);
    return ret;
}

/* 应用 write(/dev/misc_echo) 时被回调 */
static ssize_t ed_write(struct file *f, const char __user *ubuf,
                        size_t cnt, loff_t *ppos)
{
    if (cnt > MSG_MAX)
        cnt = MSG_MAX;             /* 装不下就截断 */

    mutex_lock(&lock);
    if (copy_from_user(msg, ubuf, cnt)) {
        mutex_unlock(&lock);
        return -EFAULT;
    }
    msg_len = cnt;                 /* 每次写都覆盖：简单，够教学 */
    mutex_unlock(&lock);
    return cnt;                    /* 返回值是"我收下了几个字节" */
}

/* 这就是字符设备与块设备最直观的差异点：
   块设备没有这种"一次一调"的回调，而是把 I/O 攒成 request 交给请求队列 */
static const struct file_operations ed_fops = {
    .owner  = THIS_MODULE,
    .read   = ed_read,
    .write  = ed_write,
    /* 不设 .llseek = 不可 seek（6.12 起 no_llseek 已删，NULL 即此语义） */
};

static struct miscdevice ed_dev = {
    .minor = MISC_DYNAMIC_MINOR,   /* 让内核挑一个次设备号 */
    .name  = "misc_echo",          /* → /dev/misc_echo */
    .fops  = &ed_fops,
    .mode  = 0666,                 /* 免 sudo 读写，方便实验 */
};

static int __init ed_init(void)
{
    int ret = misc_register(&ed_dev);
    if (ret)
        pr_err("misc_echo: register failed: %d\n", ret);
    else
        pr_info("misc_echo: /dev/misc_echo ready\n");
    return ret;
}

static void __exit ed_exit(void)
{
    misc_deregister(&ed_dev);
    pr_info("misc_echo: unloaded\n");
}

module_init(ed_init);
module_exit(ed_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("minimal char device (misc) echo demo");
