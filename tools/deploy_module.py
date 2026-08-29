# -*- coding: utf-8 -*-
"""
内核模块真机构建工具：上传源码 → 编译 → 加载验证 → 取回产物。

前置条件:
    pip install paramiko
    （Windows 自带的 OpenSSH 不支持命令行传密码，所以走 paramiko）

环境变量（凭据一律不写进脚本 —— 本仓库是公开的）:
    RPI_HOST       树莓派地址，默认 192.168.31.109
    RPI_USER       登录用户，默认 wzp
    RPI_PASSWORD   登录密码，必填

用法:
    python deploy_module.py <本地源目录> <远端目录> [本地产物目录] [模块名]

例:
    python deploy_module.py "C:/path/to/LLD/01-first" \
                            /home/wzp/linux-device/01-first \
                            "C:/path/to/LLD/01-first/artifacts" \
                            hello

模块名：默认自动从 <本地源目录> 下唯一的 .c 文件名推断。

产物目录（建议是各章节的 artifacts/）里会得到:
    <模块名>.ko      真机编译出的模块
    build.log       make 的完整输出
    modinfo.txt     modinfo <模块名>.ko 的输出（含 vermagic，可核对内核版本）
"""
import sys, io, os
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')
import paramiko

HOST = os.environ.get("RPI_HOST", "192.168.31.109")
USER = os.environ.get("RPI_USER", "wzp")
PASSWORD = os.environ.get("RPI_PASSWORD", "")

if not PASSWORD:
    sys.exit("请先设置环境变量 RPI_PASSWORD（树莓派登录密码）。\n"
             "本仓库是公开的，密码绝不能写进脚本 —— 一律从环境变量读取。\n"
             "  Windows (cmd):  set RPI_PASSWORD=xxx\n"
             "  Windows (PS):   $env:RPI_PASSWORD=\"xxx\"\n"
             "  Linux/macOS:    export RPI_PASSWORD=xxx")

SRC_EXTS = (".c", ".h", "Makefile", "Kbuild")


def sudo(cmd):
    """通过 sudo -S 从 stdin 喂密码"""
    return "echo '%s' | sudo -S bash -c \"%s\" 2>&1" % (PASSWORD, cmd.replace('"', '\\"'))


def ssh_run(c, cmd, timeout=300):
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode("utf-8", "replace").strip()
    err = stderr.read().decode("utf-8", "replace").strip()
    rc = stdout.channel.recv_exit_status()
    return out, err, rc


def mkdirs_p(sftp, path):
    cur = ""
    for p in path.strip("/").split("/"):
        cur += "/" + p
        try:
            sftp.stat(cur)
        except IOError:
            sftp.mkdir(cur)
            print("  创建远端目录 %s" % cur)


def infer_module_name(local_dir):
    """从目录里唯一的顶层 .c 文件名推断模块名。"""
    c_files = [fn for fn in os.listdir(local_dir) if fn.endswith(".c")]
    if len(c_files) == 1:
        return c_files[0][:-2]
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    local_dir = sys.argv[1].replace("\\", "/")
    remote_dir = sys.argv[2]
    out_dir = sys.argv[3].replace("\\", "/") if len(sys.argv) > 3 else None
    mod_name = sys.argv[4] if len(sys.argv) > 4 else infer_module_name(local_dir)
    if not mod_name:
        print("[错误] 无法推断模块名，请在第 4 个参数显式指定")
        sys.exit(2)
    artifacts = ["%s.ko" % mod_name, "build.log", "modinfo.txt"]
    print("=== 模块名: %s ===" % mod_name)

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, port=22, username=USER, password=PASSWORD,
              timeout=15, banner_timeout=30, auth_timeout=30,
              allow_agent=False, look_for_keys=False)
    print("=== 已连接 %s@%s ===" % (USER, HOST))

    # 1. 上传源码
    sftp = c.open_sftp()
    mkdirs_p(sftp, remote_dir)
    for fn in sorted(os.listdir(local_dir)):
        if fn.endswith(SRC_EXTS):
            sftp.put(local_dir + "/" + fn, remote_dir.rstrip("/") + "/" + fn)
            print("  上传 %s" % fn)

    # 2. 编译（先 clean 保证是全新产物，日志落盘）
    print("\n===== 编译 =====")
    build_cmd = ("cd %s && make clean >/dev/null 2>&1; make 2>&1 | tee build.log; "
                 "/sbin/modinfo %s.ko > modinfo.txt 2>&1" % (remote_dir, mod_name))
    out, err, rc = ssh_run(c, build_cmd)
    print(out)
    if rc != 0:
        print("[stderr]", err)
        print("[exit code] %d" % rc)

    # 3. 加载验证（确认产物真能在这个内核上跑起来）
    print("\n===== 加载验证 =====")
    out, err, rc = ssh_run(c, sudo("cd %s && insmod ./%s.ko" % (remote_dir, mod_name)))
    print(out or err or ("rc=%d" % rc))
    out, _, _ = ssh_run(c, sudo("dmesg | tail -12"))
    print(out)
    out, _, _ = ssh_run(c, sudo("rmmod %s" % mod_name))
    print(out)
    out, _, _ = ssh_run(c, sudo("dmesg | tail -3"))
    print(out)

    # 4. 取回产物
    if out_dir:
        print("\n===== 取回产物 =====")
        os.makedirs(out_dir, exist_ok=True)
        for fn in artifacts:
            rp = remote_dir.rstrip("/") + "/" + fn
            try:
                sftp.stat(rp)
            except IOError:
                print("  [跳过] 远端无 %s" % fn)
                continue
            sftp.get(rp, out_dir + "/" + fn)
            size = os.path.getsize(out_dir + "/" + fn)
            print("  下载 %s (%d 字节) -> %s" % (fn, size, out_dir))

    sftp.close()
    c.close()
    print("\n完成。")


if __name__ == "__main__":
    main()
