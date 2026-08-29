/*
 * taint.tmpl.c —— 模板：对照「写 GPL」与「写 Proprietary」时内核说什么
 *
 * 这是最普通的 hello 模块，不碰任何 GPL-only 符号，所以两种许可证都能编过。
 * 用 sed 把 @LIC@ 换成许可证字符串，加载后看 dmesg：
 *
 *   sed 's|@LIC@|GPL|g'         taint.tmpl.c > taint.c && make MOD=taint
 *   sed 's|@LIC@|Proprietary|g' taint.tmpl.c > taint.c && make MOD=taint
 *
 * 关键观察点：
 *   写 "GPL"         → 只有 out-of-tree taint（bit 12）
 *   写 "Proprietary" → 额外多出 license taint（bit 0）
 * 两者是两回事。见同目录 results.log。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static int __init taint_init(void)
{
	pr_info("Taint: loaded with license \"@LIC@\"\n");
	return 0;
}

static void __exit taint_exit(void)
{
	pr_info("Taint: unloaded\n");
}

module_init(taint_init);
module_exit(taint_exit);

MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("Compare kernel taint messages for different MODULE_LICENSE values");
MODULE_LICENSE("@LIC@");
