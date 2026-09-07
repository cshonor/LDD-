# 实验：sysfs 模块参数的真实可写性

## 想回答的问题

`module_param(v, int, 0444)` 里的 `0444` 到底挡住了谁？

直觉答案是"挡住普通用户"。实测结果是**连 root 一起挡住**。本实验用 C 程序
直接读 errno，把每个参数的读/写权限测成一张表。

## 为什么必须用 C 而不是 shell

| 做法 | 问题 |
|---|---|
| `echo x > 文件` | 只能看到 shell 包装后的文案，看不到 errno |
| `sudo echo x > 文件` | **重定向是由当前 shell 打开的**，sudo 只作用于 echo 命令本身，失败不能证明 root 写不了 |
| dash 的报错 | 任何 errno 都显示成 "I/O error"（bash 才显示 Invalid argument） |

C 程序直接 `open(O_WRONLY)` + `write()`，把 `errno` 原样打出来，没有中间层。

## 跑法

```sh
make
cd ~/LDD-/03-module-param && sudo insmod param_demo.ko my_int=10 my_bool=1 my_str="hello" my_arr=10,20,30
cd experiments/param-perm

./param_probe          # 普通用户视角
sudo ./param_probe     # root 视角，对比"写"那一列
```

`results.log` 是这台机器上的完整输出归档（含 11 组测试）。

## 结论表（实测）

| 参数 | perm | mode | 普通用户读 | 普通用户写 | root 读 | root 写 |
|---|---|---|---|---|---|---|
| `my_int` | 0644 | `-rw-r--r--` | OK | **EACCES** | OK | OK |
| `my_ro_int` | 0444 | `-r--r--r--` | OK | **EACCES** | OK | **EACCES** ⭐ |
| `my_ro_arr` | 0444 | `-r--r--r--` | OK | **EACCES** | OK | **EACCES** |
| `my_wo_int` | 0200 | `--w-------` | **EACCES** | **EACCES** | **EACCES** ⭐ | OK |
| `my_hidden` | 0 | 文件不存在 | — | ENOENT | — | ENOENT |
| `my_arr` | 0644 | `-rw-r--r--` | OK | **EACCES** | OK | OK |
| `cb_int` | 0644 | `-rw-r--r--` | OK | **EACCES** | OK | OK |

⭐ 两处反直觉的地方：

1. **root 写 0444 → EACCES**：sysfs 的 `kernfs_fop_open()` 在
   `KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK` 下检查 mode 位，**不认 CAP_DAC_OVERRIDE**。
   源码见章节 README 第 4 节。
2. **root 读 0200 → EACCES**：同一段代码里读位也查一次，是对称的。
   所以 sysfs 上权限位对 root 是硬的。

## 非法值的真实 errno

| 写入 | errno |
|---|---|
| `my_int` ← `abc` | `EINVAL` |
| `my_int` ← `99999999999999` | `ERANGE` |
| `my_bool` ← `2` | `EINVAL` |
| `my_arr` ← 9 个元素（容量 8） | `EINVAL` |

程序写完会尝试恢复原值（`0200` 那种读不回来的就跳过恢复）。

## 一个额外发现：参数名打错不报错

```
$ sudo insmod param_demo.ko nosuch=1
$ echo $?
0                    ← 成功加载，dmesg 里连一行警告都没有
```

参数名拼错 = 配置静默失效。这是 module_param 最难查的坑之一。
