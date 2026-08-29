// SPDX-License-Identifier: GPL-2.0
/*
 * 02-log_levels - printk 日志级别演示模块
 *
 * 目标：在真机上观察 printk 的 8 个日志级别，以及默认
 *      console_loglevel 对哪些消息可见的过滤效果。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static int __init log_levels_init(void)
{
	pr_info("log_levels: module loaded\n");

	/*
	 * 下面按 printk 的 8 个级别从"最紧急"到"最调试"依次打印。
	 *
	 * KERN_xxx 本质是一个字符串，例如 KERN_INFO 就是 "<6>"。
	 * 它紧跟在格式字符串前面， printk 会把 "<N>msg" 中的数字 N
	 * 解析为当前这条消息的日志级别。
	 */
	printk(KERN_EMERG  "log_levels: EMERGENCY   - system unusable\n");
	printk(KERN_ALERT   "log_levels: ALERT       - action must be taken\n");
	printk(KERN_CRIT     "log_levels: CRITICAL    - critical condition\n");
	printk(KERN_ERR      "log_levels: ERROR       - error condition\n");
	printk(KERN_WARNING  "log_levels: WARNING     - warning condition\n");
	printk(KERN_NOTICE   "log_levels: NOTICE      - normal but significant\n");
	printk(KERN_INFO     "log_levels: INFO        - informational\n");
	printk(KERN_DEBUG    "log_levels: DEBUG       - debug-level message\n");

	pr_info("log_levels: all 8 levels printed, check which ones you can see\n");
	return 0;
}

static void __exit log_levels_exit(void)
{
	pr_info("log_levels: module unloaded\n");
}

module_init(log_levels_init);
module_exit(log_levels_exit);

MODULE_AUTHOR("MPCoding - LDD");
MODULE_DESCRIPTION("printk log levels demo for Linux kernel module");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
