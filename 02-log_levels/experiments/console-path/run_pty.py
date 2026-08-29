# -*- coding: utf-8 -*-
"""
验证：给 SSH 分配一个真正的 pty（模拟你坐在机器前用键盘操作），
printk 会不会自己冒到终端上？

对照三组：
  A. 无 pty 的 exec_command   -> 等价于脚本里执行
  B. 有 pty 的 exec_command   -> 等价于 ssh 登录后的 shell
  C. 交互式 shell（invoke_shell）-> 最接近真实终端

同时看 /proc/consoles 与当前 tty 的关系。
"""
import sys, io, os, time
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')
import paramiko

# 凭据一律从环境变量读取，不硬编码（本仓库是公开的）
HOST = os.environ.get("RPI_HOST", "192.168.31.109")
USER = os.environ.get("RPI_USER", "wzp")
PASSWORD = os.environ.get("RPI_PASSWORD", "")
REMOTE = os.environ.get("RPI_MODDIR", "/home/wzp/linux-device/02-log_levels")

if not PASSWORD:
    sys.exit("请先设置环境变量 RPI_PASSWORD（树莓派登录密码），不要写进脚本")


def connect():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, port=22, username=USER, password=PASSWORD,
              timeout=15, banner_timeout=30, auth_timeout=30,
              allow_agent=False, look_for_keys=False)
    return c


def sudo(cmd):
    return "echo '%s' | sudo -S bash -c \"%s\" 2>&1" % (PASSWORD, cmd.replace('"', '\\"'))


def main():
    c = connect()

    # 准备
    print("=== 准备：清空缓冲区并卸载 ===")
    stdin, stdout, stderr = c.exec_command(sudo("rmmod log_levels 2>/dev/null; dmesg -C; echo ok"), timeout=30)
    print(stdout.read().decode('utf-8', 'replace').strip())
    print()

    # A. 无 pty
    print("=== A. 无 pty（get_pty=False，脚本执行）===")
    cmd = sudo("cd %s && insmod ./log_levels.ko; sleep 1; echo '--- 上面是 insmod 的输出 ---'" % REMOTE)
    stdin, stdout, stderr = c.exec_command(cmd, timeout=30)
    out = stdout.read().decode('utf-8', 'replace')
    print("终端收到内容:")
    print(repr(out[:300]) if out.strip() else "(空 —— 没有收到任何内核消息)")
    stdin, stdout, _ = c.exec_command(sudo("rmmod log_levels; dmesg | grep -c log_levels"), timeout=30)
    n = stdout.read().decode('utf-8', 'replace').strip()
    print("但 dmesg 里实际有 %s 行 log_levels 消息\n" % n)

    # B. 有 pty
    print("=== B. 有 pty（get_pty=True，等价于 ssh 登录后敲命令）===")
    run(c, sudo("dmesg -C"))
    chan = c.get_transport().open_session()
    chan.get_pty()
    chan.settimeout(20)
    chan.exec_command(sudo("cd %s && insmod ./log_levels.ko; sleep 1; echo MARKER_A" % REMOTE) + "\n")
    buf = b""
    deadline = time.time() + 8
    while time.time() < deadline:
        if chan.recv_ready():
            buf += chan.recv(65536)
        elif chan.exit_status_ready():
            break
        else:
            time.sleep(0.2)
    # 再收一次残留
    while chan.recv_ready():
        buf += chan.recv(65536)
    text = buf.decode('utf-8', 'replace')
    print("伪终端收到的内容:")
    print("---8<---")
    print(text if text.strip() else "(空)")
    print("--->8---")
    print("是否包含内核消息: %s\n" % ("是" if 'log_levels' in text else "否 —— pty 也不显示"))
    chan.close()

    # C. 交互式 shell
    print("=== C. 交互式 shell（invoke_shell，最接近真实终端）===")
    run(c, sudo("rmmod log_levels 2>/dev/null; dmesg -C"))
    sh = c.invoke_shell()
    sh.settimeout(3)
    time.sleep(1.5)
    sh.recv(65536)  # 吞掉 banner
    sh.send("cd %s && sudo insmod ./log_levels.ko\n" % REMOTE)
    time.sleep(1.5)
    sh.send(PASSWORD + "\n")
    time.sleep(2)
    buf2 = b""
    while sh.recv_ready():
        buf2 += sh.recv(65536)
    text2 = buf2.decode('utf-8', 'replace')
    print("交互式 shell 收到的内容:")
    print("---8<---")
    print(text2 if text2.strip() else "(空)")
    print("--->8---")
    print("是否包含内核消息: %s\n" % ("是" if 'log_levels' in text2 else "否 —— 交互式 shell 也不显示"))
    sh.close()

    # 收尾：看看 dmesg 里到底有没有（证明消息确实产生了）
    stdin, stdout, _ = c.exec_command(sudo("dmesg | grep log_levels | head -3"), timeout=30)
    print("=== 收尾：dmesg 里确认消息确实产生了 ===")
    print(stdout.read().decode('utf-8', 'replace').strip())
    run(c, sudo("rmmod log_levels 2>/dev/null; echo cleaned"))

    c.close()


def run(c, cmd, timeout=30):
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    return stdout.read().decode('utf-8', 'replace').strip()


if __name__ == "__main__":
    main()
