// SPDX-License-Identifier: GPL-2.0
/*
 * deref.c — 故意作死实验：内核态直接解引用用户指针
 *
 * ⚠️ 危险：预期在 cat 时触发 oops，杀死发起读的进程，并给内核置 taint D 位。
 *    实验前确认 /proc/sys/kernel/panic_on_oops 为 0（为 1 会整机 panic）。
 *    taint 置位后需重启才能清除。
 *
 * 对照组：04-char-block-net/char/misc_echo.c（同样的 misc 设备，用 copy_to_user，安全）。
 * 实验目的：亲眼看看在 ARM64 + PAN 的机器上，绕开 uaccess 路径直接 *ubuf 死成什么样。
 */
#define pr_fmt(fmt) "deref: " fmt

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

static ssize_t deref_read(struct file *f, char __user *ubuf,
			  size_t cnt, loff_t *ppos)
{
	volatile char *p = (volatile char *)ubuf;	/* volatile 防编译器把这次加载优化掉 */
	char c;

	pr_info("about to dereference user pointer %px directly\n", ubuf);
	c = *p;		/* ⚠️ 作死现场：不走 copy_to_user，直接解引用 */
	pr_info("survived?! read '%c' — 本内核未启用 PAN 或行为异常\n", c);
	return 0;
}

static const struct file_operations deref_fops = {
	.owner	= THIS_MODULE,
	.read	= deref_read,
	.llseek	= no_llseek,
};

static struct miscdevice deref_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "deref",
	.fops	= &deref_fops,
	.mode	= 0666,	/* 让普通用户也能 cat，不用 sudo 遮罩错误来源 */
};

module_misc_device(deref_dev);

MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("Deliberately dereference a user pointer without copy_to_user (oops demo)");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");
