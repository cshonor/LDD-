// twodev.c —— 完整字符设备三件套 + 双实例 + private_data + ioctl（阶段二大综合）
//
// 与 04 章 misc_echo 的区别：
//   misc_register 一行搞定 → 这里拆开完整三件套：
//     1. alloc_chrdev_region   动态申请设备号区间
//     2. cdev_init + cdev_add  把 file_operations 挂到 cdev
//     3. class_create + device_create  生成 /dev 节点
//   并创建两个实例（/dev/twodev0、/dev/twodev1），
//   靠 container_of(ino->i_cdev) 反查宿主结构体存进 file->private_data，
//   两个 fd 各自隔离——这就是"一套驱动管多个实例"的标准姿势。

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define NDEV    2
#define BUF_MAX 256

/* ioctl 命令编码：幻数 'W'，方向 + 序号 + 载荷类型编进一个 32 位整数 */
#define TWODEV_MAGIC    'W'
#define TWODEV_GET_BUFSZ _IOR(TWODEV_MAGIC, 1, int)   /* 读：内核→用户 */
#define TWODEV_SET_BUFSZ _IOW(TWODEV_MAGIC, 2, int)   /* 写：用户→内核 */
#define TWODEV_DOUBLE    _IOWR(TWODEV_MAGIC, 3, int)  /* 双向：进×2 出 */

struct twodev {
    char buf[BUF_MAX];
    size_t len;        /* buf 当前有效字节数 */
    size_t bufsz;      /* ioctl 可调的有效上限 */
    struct mutex lock;
    struct cdev cdev;  /* 嵌在宿主里 → inode->i_cdev 反查回来 */
};

static dev_t devid;
static struct class *cls;
static struct twodev devs[NDEV];

static int td_open(struct inode *ino, struct file *f)
{
    /* 阶段二核心一：从 inode 里的 cdev 指针反查宿主结构体 */
    struct twodev *d = container_of(ino->i_cdev, struct twodev, cdev);
    f->private_data = d;   /* 之后 read/write/ioctl 从 private_data 取回 */
    pr_info("twodev: open /dev/twodev%d\n", iminor(ino));
    return 0;
}

static ssize_t td_write(struct file *f, const char __user *ubuf,
                        size_t cnt, loff_t *ppos)
{
    struct twodev *d = f->private_data;

    if (cnt > d->bufsz)
        cnt = d->bufsz;                /* 按 ioctl 调好的上限截断 */
    mutex_lock(&d->lock);
    if (copy_from_user(d->buf, ubuf, cnt)) {
        mutex_unlock(&d->lock);
        return -EFAULT;
    }
    d->len = cnt;
    mutex_unlock(&d->lock);
    return cnt;
}

static ssize_t td_read(struct file *f, char __user *ubuf,
                       size_t cnt, loff_t *ppos)
{
    struct twodev *d = f->private_data;

    if (*ppos >= d->len)
        return 0;
    if (cnt > d->len - *ppos)
        cnt = d->len - *ppos;
    mutex_lock(&d->lock);
    if (copy_to_user(ubuf, d->buf + *ppos, cnt)) {
        mutex_unlock(&d->lock);
        return -EFAULT;
    }
    *ppos += cnt;
    mutex_unlock(&d->lock);
    return cnt;
}

/* 阶段二核心二：ioctl 三种方向各来一个命令 */
static long td_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct twodev *d = f->private_data;
    int val;

    switch (cmd) {
    case TWODEV_GET_BUFSZ:             /* _IOR：内核读给用户 */
        val = (int)d->bufsz;
        if (copy_to_user((int __user *)arg, &val, sizeof val))
            return -EFAULT;
        return 0;
    case TWODEV_SET_BUFSZ:             /* _IOW：用户写给内核 */
        if (copy_from_user(&val, (int __user *)arg, sizeof val))
            return -EFAULT;
        if (val < 1 || val > BUF_MAX)
            return -EINVAL;
        mutex_lock(&d->lock);
        d->bufsz = val;
        mutex_unlock(&d->lock);
        return 0;
    case TWODEV_DOUBLE:                /* _IOWR：进一个数，吐回×2 */
        if (copy_from_user(&val, (int __user *)arg, sizeof val))
            return -EFAULT;
        val *= 2;
        if (copy_to_user((int __user *)arg, &val, sizeof val))
            return -EFAULT;
        return 0;
    }
    return -ENOTTY;                    /* 非本驱动命令的标准返回 */
}

static int td_release(struct inode *ino, struct file *f)
{
    pr_info("twodev: close /dev/twodev%d\n", iminor(ino));
    return 0;
}

static const struct file_operations td_fops = {
    .owner          = THIS_MODULE,
    .open           = td_open,
    .read           = td_read,
    .write          = td_write,
    .unlocked_ioctl = td_ioctl,
    .release        = td_release,
};

static int __init td_init(void)
{
    int ret, i;

    /* 三件套之一：设备号 */
    ret = alloc_chrdev_region(&devid, 0, NDEV, "twodev");
    if (ret)
        return ret;
    pr_info("twodev: got major %d (%d minors)\n", MAJOR(devid), NDEV);

    cls = class_create("twodev");

    for (i = 0; i < NDEV; i++) {
        struct twodev *d = &devs[i];

        memset(d->buf, 0, sizeof d->buf);
        d->len = 0;
        d->bufsz = BUF_MAX;
        mutex_init(&d->lock);

        /* 三件套之二：cdev 绑定 fops */
        cdev_init(&d->cdev, &td_fops);
        d->cdev.owner = THIS_MODULE;
        ret = cdev_add(&d->cdev, MKDEV(MAJOR(devid), i), 1);
        if (ret)
            goto err_undo;

        /* 三件套之三：/dev/twodevN 节点 */
        if (IS_ERR(device_create(cls, NULL, MKDEV(MAJOR(devid), i),
                                 NULL, "twodev%d", i))) {
            ret = -PTR_ERR(&devs[i]);
            cdev_del(&d->cdev);
            goto err_undo;
        }
    }
    pr_info("twodev: /dev/twodev0 and /dev/twodev1 ready\n");
    return 0;

err_undo:
    while (--i >= 0) {
        device_destroy(cls, MKDEV(MAJOR(devid), i));
        cdev_del(&devs[i].cdev);
    }
    class_destroy(cls);
    unregister_chrdev_region(devid, NDEV);
    return ret;
}

static void __exit td_exit(void)
{
    int i;

    for (i = 0; i < NDEV; i++) {
        device_destroy(cls, MKDEV(MAJOR(devid), i));
        cdev_del(&devs[i].cdev);
    }
    class_destroy(cls);
    unregister_chrdev_region(devid, NDEV);
    pr_info("twodev: unloaded\n");
}

module_init(td_init);
module_exit(td_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("full char device: 3-step registration + private_data + ioctl");
