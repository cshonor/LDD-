// SPDX-License-Identifier: GPL-2.0
/*
 * mmapbuf.c — mmap 零拷贝：把内核缓冲区直接映射进用户地址空间
 *
 * 配套文档：README.md（11-mmap）
 * 实测环境：树莓派 5，内核 6.18.39+rpt-rpi-2712（aarch64）
 *
 * 设计：
 *   - init 时 kmalloc 4 页（16KB），写入初始字符串
 *   - .mmap 用 remap_pfn_range 把这 4 页物理页填进用户 VMA 的 PTE
 *   - 之后用户态读写这块内存 = 普通内存访问，零 syscall、零拷贝
 *   - .read 保留（10 章的 copy_to_user 对照路径），用来验证"同一块内存"
 *
 * 验证流程（test.sh）：
 *   cat /dev/mmapbuf          → read 路径看到 init 写入的内容
 *   ./test_mmap               → mmap 路径直接读 + 直接写
 *   cat /dev/mmapbuf          → 内核侧确认用户态的写入（共享内存生效）
 */
#define pr_fmt(fmt) "mmapbuf: " fmt

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#define BUF_PAGES	4
#define BUF_SIZE	(PAGE_SIZE * BUF_PAGES)	/* 16 KB */

static char *kbuf;	/* 内核缓冲区（线性映射区，物理连续） */

static int mbuf_open(struct inode *inode, struct file *filp)
{
	pr_info("open\n");
	return 0;
}

static int mbuf_release(struct inode *inode, struct file *filp)
{
	pr_info("release\n");
	return 0;
}

/* 对照路径：10 章的 read + copy_to_user（每次访问都要 syscall + 拷贝） */
static ssize_t mbuf_read(struct file *f, char __user *ubuf,
			 size_t cnt, loff_t *ppos)
{
	if (*ppos >= BUF_SIZE)
		return 0;
	if (cnt > BUF_SIZE - *ppos)
		cnt = BUF_SIZE - *ppos;
	if (copy_to_user(ubuf, kbuf + *ppos, cnt))
		return -EFAULT;
	*ppos += cnt;
	return cnt;
}

/* 零拷贝路径：mmap(2) 时调用一次，把物理页帧填进用户 VMA 的页表 */
static int mbuf_mmap(struct file *f, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long pfn;

	/* offset（mmap 第 6 参）映射成 vma->vm_pgoff，必须页对齐且为 0；
	 * 映射长度不能超过缓冲区 */
	if (vma->vm_pgoff != 0 || size > BUF_SIZE) {
		pr_err("mmap rejected: pgoff=%lu size=%zu\n",
		       vma->vm_pgoff, size);
		return -EINVAL;
	}

	/* virt_to_phys 只对线性映射区（kmalloc）有效；
	 * vmalloc 的内存要逐页 vmalloc_to_page，不能这么算 */
	pfn = virt_to_phys(kbuf) >> PAGE_SHIFT;

	/* v6.3 起 remap_pfn_range 内部自动置 VM_PFNMAP|VM_DONTEXPAND|VM_DONTDUMP，
	 * 老教程手工 vm_flags_set 的代码已不需要 */
	if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
		pr_err("remap_pfn_range failed\n");
		return -EAGAIN;
	}

	pr_info("mmap: %zu bytes mapped (pfn=0x%lx)\n", size, pfn);
	return 0;
}

static const struct file_operations mbuf_fops = {
	.owner		= THIS_MODULE,
	.open		= mbuf_open,
	.release	= mbuf_release,
	.read		= mbuf_read,
	.mmap		= mbuf_mmap,
	.llseek		= no_llseek,
};

static struct miscdevice mbuf_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "mmapbuf",
	.fops	= &mbuf_fops,
	.mode	= 0666,
};

static int __init mbuf_init(void)
{
	int i, ret;

	kbuf = kmalloc(BUF_SIZE, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	memset(kbuf, 0, BUF_SIZE);
	strcpy(kbuf, "hello from kernel init (via module_init)");

	/* 把每页置 Reserved：防止映射期间被内核回收/挪用。
	 * kmalloc 内存本就不可换出，这是通行保险做法。 */
	for (i = 0; i < BUF_PAGES; i++)
		SetPageReserved(virt_to_page(kbuf + i * PAGE_SIZE));

	ret = misc_register(&mbuf_dev);
	if (ret) {
		for (i = 0; i < BUF_PAGES; i++)
			ClearPageReserved(virt_to_page(kbuf + i * PAGE_SIZE));
		kfree(kbuf);
		return ret;
	}

	pr_info("loaded, /dev/mmapbuf ready, buffer %zu bytes @ pfn=0x%lx\n",
		(size_t)BUF_SIZE, virt_to_phys(kbuf) >> PAGE_SHIFT);
	return 0;
}

static void __exit mbuf_exit(void)
{
	int i;

	/* ⚠️ 教学简化：若用户态还持有映射就 rmmod，其页表项将指向已释放的页，
	 * 下次访问即 oops。真实驱动用 vm_ops open/close 给映射计引用。
	 * 测试顺序务必：先 kill/munmap 所有使用者，再 rmmod。 */
	misc_deregister(&mbuf_dev);
	for (i = 0; i < BUF_PAGES; i++)
		ClearPageReserved(virt_to_page(kbuf + i * PAGE_SIZE));
	kfree(kbuf);
	pr_info("unloaded\n");
}

module_init(mbuf_init);
module_exit(mbuf_exit);

MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("mmap zero-copy: map a kmalloc'd kernel buffer into userspace");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");
