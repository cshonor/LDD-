// memdemo.c —— 内核内存分配机制对比（阶段三之一）
//
// 内核态没有 malloc。本模块在 init 时用四种方式各分配一块，
// 把"内核虚拟地址"和"物理地址"（可翻译的才翻）打进 dmesg：
//   kmalloc   物理连续、小快，GFP_KERNEL 可睡眠 / GFP_ATOMIC 中断里用
//   kzalloc   kmalloc + 清零
//   vmalloc   虚拟连续、物理可碎、适合大块（页级开销）
//   get_zeroed_page  单页老接口
// 同时注册字符设备 /dev/memdemo：cat 它能看到一份报告。

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

static int *small_p;      /* kmalloc(16 * sizeof int) */
static char *zero_p;      /* kzalloc(1KB) */
static void *big_v;       /* vmalloc(1MB) */
static char report[2048];
static size_t report_len;

static ssize_t md_read(struct file *f, char __user *u,
                       size_t cnt, loff_t *ppos)
{
    if (*ppos >= report_len)
        return 0;
    if (cnt > report_len - *ppos)
        cnt = report_len - *ppos;
    if (copy_to_user(u, report + *ppos, cnt))
        return -EFAULT;
    *ppos += cnt;
    return cnt;
}

static const struct file_operations md_fops = {
    .owner = THIS_MODULE,
    .read  = md_read,
};

static struct miscdevice md_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "memdemo",
    .fops  = &md_fops,
    .mode  = 0444,
};

static int __init md_init(void)
{
    phys_addr_t pa;
    int n;

    small_p = kmalloc(16 * sizeof(int), GFP_KERNEL);
    zero_p  = kzalloc(1024, GFP_KERNEL);
    big_v   = vmalloc(1 << 20);

    if (!small_p || !zero_p || !big_v) {
        pr_err("memdemo: alloc failed\n");
        kfree(small_p); kfree(zero_p); vfree(big_v);
        return -ENOMEM;
    }

    for (n = 0; n < 16; n++)
        small_p[n] = n * n;

    pr_info("memdemo: kmalloc  %p  phys=%pap\n", small_p, &pa);
    pa = virt_to_phys(small_p);
    pr_info("memdemo: kmalloc  virt=%px phys=%pap (物理连续,直接映射区)\n",
            small_p, &pa);
    pr_info("memdemo: kzalloc  virt=%px zero_p[0]=%d (自动清零)\n",
            zero_p, zero_p[0]);
    pr_info("memdemo: vmalloc  virt=%px (虚拟连续,物理可碎,VMALLOC区地址明显不同)\n",
            big_v);

    n = snprintf(report, sizeof report,
        "kmalloc  64B  virt=%px phys=%pap  (直接映射区,物理连续)\n"
        "kzalloc  1KB  virt=%px            (同 kmalloc,清零版)\n"
        "vmalloc  1MB  virt=%px            (VMALLOC 区,虚拟连续)\n",
        small_p, &pa, zero_p, big_v);
    report_len = n;

    pr_info("memdemo: /dev/memdemo ready (cat it)\n");
    return misc_register(&md_dev);
}

static void __exit md_exit(void)
{
    misc_deregister(&md_dev);
    kfree(small_p);
    kfree(zero_p);
    vfree(big_v);
    pr_info("memdemo: freed and unloaded\n");
}

module_init(md_init);
module_exit(md_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kernel memory allocators: kmalloc/kzalloc/vmalloc demo");
