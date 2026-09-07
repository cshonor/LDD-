# 03-module-param — 内核模块参数

> 真机环境：树莓派 5 / Debian 13 trixie / `6.18.34+rpt-rpi-2712` (aarch64)
> 所有结论都在这台机器上实测过，不是抄书。

## 本节讲什么

模块参数让你**不用重新编译就能改模块行为**。它看起来只有一行宏，但权限位那
第三个参数的语义比"可读可写"复杂得多 —— 尤其 **root 能不能写 0444 的参数**
这个问题，答案和大多数人的直觉相反。

本章用 7 种参数（3 种权限 × 多种类型）把所有分支摆在同一模块里，一次性测完。

---

## 1. 一句话记住

```c
module_param(变量名, 类型, sysfs 权限位);
MODULE_PARM_DESC(变量名, "描述文本");
```

- 第一个参数是**代码里的全局变量**，sysfs 文件直接绑定它（改文件 = 改变量）
- 第二个是类型，决定内核用哪个"字符串 → 值"解析器
- 第三个是 **sysfs 文件的 mode**，决定谁能看、谁能改

---

## 2. 类型表

| 宏里的类型名 | C 变量声明 | 接受的输入 | 备注 |
|---|---|---|---|
| `int` | `int` | 十进制整数 | 溢出报 `ERANGE` |
| `uint` | `unsigned int` | 无符号整数 | 负数会被拒 |
| `long` / `ulong` | `long` / `unsigned long` | 整数 | 64 位机上 long 是 8 字节 |
| `short` / `ushort` | `short` / `unsigned short` | 整数 | |
| `charp` | `char *` | 任意字符串 | **内核会 kstrdup，模块不要 kfree** |
| `bool` | `bool` | `1/0/y/n/Y/N` | 实测读回来显示 `Y`/`N` 而不是 `1`/`0` |
| `invbool` | `bool` | 同上 | 取值**反转**（`y` → `false`） |
| 数组 | `int arr[N]` | `a,b,c` 逗号分隔 | 用 `module_param_array` |

> `bool` 读回 `Y` 这件事很容易踩：写进去 `1`，`cat` 出来是 `Y`。
> 这是 `param_get_bool` 的输出格式（`Y`/`N`），值本身没变。

---

## 3. 权限位：先把"谁能写"说清楚

### 3.1 三种典型取值

| perm | sysfs 文件形态 | 谁能读 | 谁能写 |
|---|---|---|---|
| `0644` | `-rw-r--r--` | 所有人 | **只有 root**（普通用户实测 `EACCES`） |
| `0444` | `-r--r--r--` | 所有人 | **谁都不行，包括 root** ← 见第 4 节 |
| `0200` | `--w-------` | **谁都不行，包括 root** | 只有 root |
| `0` | 文件不创建 | — | —（只能在 `insmod` 命令行传） |

**注意 `0644` 的常见误读**：它常被说成"可读可写"，但准确说法是
**root 可写、所有人可读**。普通用户写 `0644` 的参数实测是 `EACCES`：

```
普通用户视角：my_int  -rw-r--r--  读 OK   写 EACCES
root    视角：my_int  -rw-r--r--  读 OK   写 OK
```

想让普通用户也能改，得写 `0666` —— 但那等于允许任意进程修改内核模块的全局
变量，别这么干。

### 3.2 数据流：一次 sysfs 写入要经过几道门

```
用户态 write("1234")
    │
    ▼
① VFS open()  ──── inode_permission() ───── root 有 CAP_DAC_OVERRIDE，可绕过
    │
    ▼
② kernfs_fop_open()  ── KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK ──★ 不认 capability
    │                    !(inode->i_mode & S_IWUGO) → -EACCES
    ▼
③ param_attr_store()  ── 调用 kp->ops->set ───── 类型解析失败 → -EINVAL/-ERANGE
    │
    ▼
④ 写进你的 C 变量（普通 module_param 到此为止，模块收不到通知）
```

第 ② 道门就是 Quiz 的答案所在。

---

## 4. Quiz：权限 0444 时 `echo 10 > .../my_int` 会发生什么？

### 答案

**失败，Permission denied（EACCES），值不变。而且 —— 用 `sudo` 也一样失败。**

```
$ sudo sh -c 'echo 10 > /sys/module/param_demo/parameters/my_ro_int'
sh: 1: cannot create .../my_ro_int: Permission denied
$ cat /sys/module/param_demo/parameters/my_ro_int
7                              ← 没变
```

用 C 程序直接看 errno（这才是精确答案）：

| 身份 | 读 0444 | 写 0444 | 读 0200 | 写 0644 |
|---|---|---|---|---|
| 普通用户 (uid 1000) | OK | **EACCES** | EACCES | **EACCES** |
| root (uid 0, CAP_DAC_OVERRIDE) | OK | **EACCES** | **EACCES** | OK |

### 为什么 root 也不行？

因为在普通文件上 root 靠 `CAP_DAC_OVERRIDE` 绕过权限位，但 **sysfs 额外加了一道
不认 capability 的检查**。证据链（Linux v6.18 源码）：

**① sysfs 是唯一带这个 flag 的 kernfs 实例** —— `fs/sysfs/mount.c:101`

```c
sysfs_root = kernfs_create_root(NULL, KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK, NULL);
```

**② 检查在 open 时做，读写位各查一次** —— `fs/kernfs/file.c` 的 `kernfs_fop_open()`

```c
int error = -EACCES;
...
/* see the flag definition for details */
if (root->flags & KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK) {
        if ((file->f_mode & FMODE_WRITE) &&
            (!(inode->i_mode & S_IWUGO) || !has_write))
                goto err_out;

        if ((file->f_mode & FMODE_READ) &&
            (!(inode->i_mode & S_IRUGO) || !has_read))
                goto err_out;
}
```

两点值得注意：

- 函数的 `error` **初始值就是 `-EACCES`**，所以 `goto err_out` 出来的就是 EACCES
- 判断是 `mode & S_IWUGO`（三类里**任意一类**有写位即可），不是逐类匹配
- 读位同理：`perm=0200` 的参数 **root 也 `cat` 不出来**（实测确认）

**③ 结论**：在 sysfs 上，权限位对 root 是**硬**的。想留后路就别写 `0444`。

---

## 5. 数组参数：第 3 个参数是【输出】

```c
static int my_arr[8];
static int my_arr_cnt;                                    /* 接收实际个数 */
module_param_array(my_arr, int, &my_arr_cnt, 0644);
```

| 传参 | 实测 `my_arr_cnt` | 结果 |
|---|---|---|
| `my_arr=10,20,30` | 3 | `my_arr[3] = { 10 20 30 }` |
| `my_arr=1,2,3,4,5,6,7,8` | 8 | `my_arr[8] = { 1 2 3 4 5 6 7 8 }` 刚好装满，OK |
| `my_arr=1,...,9`（9 个） | — | **加载失败** `Invalid parameters` |
| 不传 | 0 | `my_arr[0] = { }` |

- 容量由 C 声明决定（`[8]`），传超了**不会静默截断**，而是直接不让模块加载
- 遍历前一定先判 `my_arr_cnt`，否则会把未初始化的元素当有效值

---

## 6. 模块怎么知道参数被改了？—— `module_param_cb`

**普通 `module_param` 被改时，模块不会收到任何通知。** sysfs 写入直接改掉那个
C 变量就结束了：没有回调、没有事件、没有日志。

实测对照（同一个模块里两个参数）：

| 操作 | 结果 |
|---|---|
| `echo 1234 > my_int`（普通 param） | 值变了，dmesg **一行都没有** |
| `echo 4321 > cb_int`（cb 版） | 值变了 + `cb_int changed: 0 -> 4321 (module was notified)` |

需要"参数变了要重新配置硬件/重算"时，用 `module_param_cb` 接管 `set`：

```c
static int cb_int_set(const char *val, const struct kernel_param *kp)
{
        int old = *(int *)kp->arg;
        int ret = param_set_int(val, kp);   /* 先让内核完成解析和赋值 */
        if (ret)
                return ret;                 /* 错误必须原样返回，别吞 */
        pr_info("cb_int changed: %d -> %d\n", old, *(int *)kp->arg);
        return 0;
}

static const struct kernel_param_ops cb_int_ops = {
        .set = cb_int_set,
        .get = param_get_int,               /* 读可以复用标准的 */
};
module_param_cb(cb_int, &cb_int_ops, &cb_int, 0644);
```

要点：`param_set_xxx(val, kp)` 必须**先调**，它负责字符串解析和实际赋值；
它的返回值要原样返回，否则用户会以为改成功了其实没有。

---

## 7. `MODULE_PARM_DESC` 与 `modinfo`

```c
module_param(my_int, int, 0644);
MODULE_PARM_DESC(my_int, "int, perm=0644: root writable, world readable");
```

`modinfo` 里的样子（实测输出）：

```
parm:           my_int:int, perm=0644: root writable, world readable (int)
parm:           my_str:charp, perm=0644: kernel strdup's it, do NOT kfree (charp)
parm:           my_arr:int array, perm=0644: count is an OUTPUT param (array of int)
```

格式是 `名字:描述 (类型)`。这是**唯一**能让使用者不读源码就知道参数含义的地方，
别省。没写 `MODULE_PARM_DESC` 的参数仍然会出现在 `modinfo` 里，只是没有描述。

---

## 8. 错误处理：加载期 vs 运行期

### 8.1 `insmod` 阶段（参数值非法 → 模块根本进不来）

| 输入 | 结果 |
|---|---|
| `my_int=abc` | `Invalid parameters`，返回码 1 |
| `my_bool=maybe` | `Invalid parameters` |
| `my_arr=1,...,9`（超容量） | `Invalid parameters` |
| **`nosuch=1`**（参数名不存在） | **静默通过！返回码 0，dmesg 连警告都没有** |

⚠️ 最后一条是 module_param 最阴的坑：**参数名打错不报错，配置静默失效**。
改了半天发现没生效，第一件事就是回去逐字核对参数名。

### 8.2 运行期（sysfs 写入非法值）

| 写入 | 真实 errno |
|---|---|
| `my_int` ← `abc` | `EINVAL` |
| `my_int` ← `99999999999999` | `ERANGE`（超出 int 范围） |
| `my_bool` ← `2` | `EINVAL` |
| `my_arr` ← 9 个元素 | `EINVAL` |

**shell 的报错不可信** —— 同一个 `EINVAL`，不同 shell 说法不一样：

```
dash  (sudo sh -c ...)     : sh: 1: echo: echo: I/O error      ← 误导
bash  (sudo bash -c ...)   : echo: write error: Invalid argument
C 程序直接读 errno         : EINVAL                            ← 真实
```

排查 sysfs 写入问题时，用 `bash` 或者干脆写个 C 程序读 errno，别信 dash。

---

## 9. 坑表

| # | 坑 | 表现 | 正确做法 |
|---|---|---|---|
| 1 | 以为 0444 只是"限制普通用户" | root 写也 `EACCES` | 要留后路用 0644；要彻底禁用用 0 |
| 2 | 以为 0644 是"所有人可写" | 普通用户写 `EACCES` | 0644 = 只有 root 可写 |
| 3 | 参数名打错 | **静默忽略**，无任何提示 | 逐字核对；必要时 `modinfo` 确认 |
| 4 | 数组传超容量 | 加载失败 `Invalid parameters` | 容量由 C 声明决定，不会截断 |
| 5 | 遍历数组不看 count | 读到未初始化元素 | 先判 `my_arr_cnt` |
| 6 | 以为改参数模块会收到通知 | 没有任何回调 | 需要通知就用 `module_param_cb` |
| 7 | 在 exit 里 `kfree(my_str)` | charp 内存不是你分配的 → double free | 别释放 |
| 8 | `echo "abc" > charp参数` | **换行符被一起存进内核字符串** | 用 `printf "abc"` 写入 |
| 9 | 用 dash 看写入报错 | 统一显示 "I/O error" | 用 bash 或 C 程序读 errno |
| 10 | `sudo echo x > 文件` | 失败时误以为 root 也不行 | 重定向由当前 shell 打开，要用 `sudo sh -c` 或 `tee` |

第 7、8 条展开说：

**charp 的内存谁管？** `param_set_charp` 会 `kstrdup` 一份，模块拿到的是内核
分配的内存。模块卸载时内核**不会**替你释放（只泄漏几十字节，实际无所谓），
但**你也绝对不能 `kfree`** —— 那块内存在下次 set 时由内核自己回收。

**换行符**：`echo` 写入会带尾随 `\n`，内核原样保存。实测 `cat -A`：

```
printf "abc" 写入 → abc$              ← 干净
echo xyz     写入 → xyz$              ← 多出一个空行，说明存了 "xyz\n"
                     $
```

如果模块里拿这个字符串去 `strcmp` 或拼路径，就会莫名其妙对不上。

---

## 10. 速查

```sh
make                        # 编译
sudo insmod param_demo.ko my_int=10 my_bool=1 my_str="hello" my_arr=10,20,30
modinfo param_demo.ko | grep parm          # 看所有参数的名字/类型/描述
ls -l /sys/module/param_demo/parameters/   # 看权限位与是否存在
cat /sys/module/param_demo/parameters/my_int
sudo sh -c 'echo 99 > /sys/module/param_demo/parameters/my_int'   # 只有 root 能写
sudo dmesg | tail                          # 看 cb_int 的回调日志
sudo rmmod param_demo                      # 卸载时会打印所有参数的最终值
```

**实验**：[experiments/param-perm/](experiments/param-perm/README.md) 里的
`param_probe.c` 是个用户态 C 程序，把上表所有权限组合各测一遍并打印真实 errno。
分别用普通用户和 `sudo` 各跑一次，对比"写"那一列。

---

## 11. 衔接

- **上一章** [02-log-levels](../02-log-levels/README.md)：`printk` 与日志级别。
  本章的 `cb_int` 回调就是靠 `pr_info` 才看得见的。
- **下一章** 04-char-device：字符设备与 `file_operations`。届时会有真正的
  设备节点 `/dev/xxx`，模块参数通常用来传入主设备号、缓冲区大小这类配置。

> 模块参数适合"加载时配一次的开关"。需要频繁交互的控制接口，应该走 ioctl
> 或自建 sysfs attribute —— 参数只是图省事的产物。
