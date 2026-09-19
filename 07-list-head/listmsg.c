// listmsg.c —— 侵入式双向链表 list_head（阶段三之二）
//
// 内核链表的核心思想：不是"链表节点里挂数据"，
// 而是 list_head 嵌进数据结构里——遍历时用 list_for_each_entry
// （内部就是 container_of）从节点指针反查宿主。
// 这和驱动里"一个 cdev 嵌在设备结构体里"是同一个套路。
//
// 行为：/dev/listmsg
//   write  → 把消息 kmalloc 一个节点，list_add_tail 入链
//   read   → 按序遍历，输出 "N: msg"
//   ioctl  → LISTMSG_CLEAR 清空并释放全部节点
// 用柔性数组 data[] 装消息（呼应 01-ch1 的 1.5 零长度/柔性数组）。

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define MSG_MAX 120
#define LISTMSG_MAGIC 'L'
#define LISTMSG_CLEAR _IO(LISTMSG_MAGIC, 1)   /* 无参数命令用 _IO */
#define LISTMSG_COUNT _IOR(LISTMSG_MAGIC, 2, int)

struct msg {
    struct list_head list;   /* 侵入点：嵌在宿主里 */
    size_t len;
    char data[];             /* 柔性数组：节点和消息体一次 kmalloc */
};

static LIST_HEAD(msg_list);  /* 链表头 */
static int msg_count;
static DEFINE_MUTEX(lock);

/* read 的游标：上次读到的节点（private_data 存当前 fd 的遍历位置） */
struct cursor { struct msg *pos; int idx; };

static ssize_t lm_write(struct file *f, const char __user *u,
                        size_t cnt, loff_t *ppos)
{
    struct msg *m;

    if (cnt > MSG_MAX)
        cnt = MSG_MAX;
    if (cnt == 0)
        return 0;

    m = kmalloc(sizeof(*m) + cnt, GFP_KERNEL);  /* 柔性数组：一次分配 */
    if (!m)
        return -ENOMEM;

    if (copy_from_user(m->data, u, cnt)) {
        kfree(m);
        return -EFAULT;
    }
    m->len = cnt;

    mutex_lock(&lock);
    list_add_tail(&m->list, &msg_list);
    msg_count++;
    mutex_unlock(&lock);
    return cnt;
}

static ssize_t lm_read(struct file *f, char __user *u,
                       size_t cnt, loff_t *ppos)
{
    struct cursor *cur = f->private_data;
    char line[MSG_MAX + 32];
    int n;

    /* 每次读吐一条消息；读完返回 0（EOF） */
    if (!cur->pos)
        return 0;

    mutex_lock(&lock);
    n = snprintf(line, sizeof line, "%d: %.*s\n",
                 cur->idx, (int)cur->pos->len, cur->pos->data);
    cur->pos = list_is_last(&cur->pos->list, &msg_list)
             ? NULL : list_next_entry(cur->pos, list);
    cur->idx++;
    mutex_unlock(&lock);

    if (cnt > (size_t)n)
        cnt = n;
    if (copy_to_user(u, line, cnt))
        return -EFAULT;
    return cnt;
}

static int lm_open(struct inode *ino, struct file *f)
{
    struct cursor *cur;

    cur = kzalloc(sizeof *cur, GFP_KERNEL);
    if (!cur)
        return -ENOMEM;
    mutex_lock(&lock);
    cur->pos = list_first_entry_or_null(&msg_list, struct msg, list);
    cur->idx = 1;
    mutex_unlock(&lock);
    f->private_data = cur;   /* 这次 open 的遍历游标 = 私有字段 */
    return 0;
}

static int lm_release(struct inode *ino, struct file *f)
{
    kfree(f->private_data);
    return 0;
}

static long lm_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    int val;

    switch (cmd) {
    case LISTMSG_COUNT:
        mutex_lock(&lock);
        val = msg_count;
        mutex_unlock(&lock);
        if (copy_to_user((int __user *)arg, &val, sizeof val))
            return -EFAULT;
        return 0;
    case LISTMSG_CLEAR: {
        struct msg *m, *tmp;
        mutex_lock(&lock);
        list_for_each_entry_safe(m, tmp, &msg_list, list) {
            list_del(&m->list);
            kfree(m);
        }
        val = msg_count; msg_count = 0;
        mutex_unlock(&lock);
        pr_info("listmsg: cleared %d messages\n", val);
        /* 已打开的 fd 游标悬空了——重置它（真实驱动要更周全） */
        ((struct cursor *)f->private_data)->pos = NULL;
        return 0;
    }
    }
    return -ENOTTY;
}

static const struct file_operations lm_fops = {
    .owner          = THIS_MODULE,
    .open           = lm_open,
    .read           = lm_read,
    .write          = lm_write,
    .unlocked_ioctl = lm_ioctl,
    .release        = lm_release,
};

static struct miscdevice lm_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "listmsg",
    .fops  = &lm_fops,
    .mode  = 0666,
};

static int __init lm_init(void)
{
    pr_info("listmsg: /dev/listmsg ready\n");
    return misc_register(&lm_dev);
}

static void __exit lm_exit(void)
{
    struct msg *m, *tmp;
    mutex_lock(&lock);
    list_for_each_entry_safe(m, tmp, &msg_list, list) {
        list_del(&m->list);
        kfree(m);
    }
    mutex_unlock(&lock);
    misc_deregister(&lm_dev);
    pr_info("listmsg: unloaded\n");
}

module_init(lm_init);
module_exit(lm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("intrusive list_head demo: message queue char device");
