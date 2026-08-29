/*
 * gplonly.tmpl.c —— 模板：验证「许可证字符串」决定能否用 GPL-only 符号
 *
 * 内核导出符号分两类：
 *   EXPORT_SYMBOL       谁都能用（本内核 8199 个）
 *   EXPORT_SYMBOL_GPL   只有 GPL 兼容许可证的模块能用（本内核 10989 个，占 57%）
 *
 * 本模块引用 pm_power_off（<linux/pm.h> 声明，EXPORT_SYMBOL_GPL 导出），
 * 只取地址打印，不真的关电。用 sed 把 @LIC@ 换成不同许可证，看 modpost 反应：
 *
 *   sed 's|@LIC@|GPL|g'              gplonly.tmpl.c > gplonly.c && make MOD=gplonly   # ✅
 *   sed 's|@LIC@|GPL v2 or later|g'  gplonly.tmpl.c > gplonly.c && make MOD=gplonly   # ❌
 *
 * 完整结果见同目录 results.log。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pm.h>      /* pm_power_off: void (*)(void)，EXPORT_SYMBOL_GPL */

static int __init gplonly_init(void)
{
	pr_info("GplOnly: license=\"@LIC@\" pm_power_off=%ps\n", pm_power_off);
	return 0;
}

static void __exit gplonly_exit(void)
{
	pr_info("GplOnly: bye\n");
}

module_init(gplonly_init);
module_exit(gplonly_exit);

MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("Test whether license string permits GPL-only symbols");
MODULE_LICENSE("@LIC@");
