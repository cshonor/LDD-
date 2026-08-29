# -*- coding: utf-8 -*-
"""
实验：printk 到底流向哪里？

连接 A：后台实时跟随内核日志（dmesg -W，只跟新消息 + 行缓冲）
连接 B：加载模块产生 printk
观察：连接 A 能否实时收到；SSH 终端自身会不会自动显示。
"""
import sys, io, os, time, threading
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


def run(c, cmd, timeout=60):
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode('utf-8', 'replace').strip()
    err = stderr.read().decode('utf-8', 'replace').strip()
    return out, err


def background_tail(c, cmd, sink, stop_evt, tag):
    try:
        stdin, stdout, stderr = c.exec_command(cmd, timeout=90)
        for line in iter(stdout.readline, ''):
            if stop_evt.is_set():
                break
            sink.append((tag, line.rstrip()))
    except Exception as e:
        sink.append((tag, "[异常] %s: %s" % (type(e).__name__, e)))


def main():
    ca = connect()
    sink = []
    stop = threading.Event()

    print("=== 实验准备：清空缓冲区 ===")
    run(ca, sudo("rmmod log_levels 2>/dev/null; dmesg -C; echo ok"))
    print("已清空\n")

    # 只跟新消息（-W），套 stdbuf 强制行缓冲
    cmd = "timeout 15 sudo stdbuf -oL dmesg -W 2>/dev/null"
    t1 = threading.Thread(target=background_tail,
                          args=(ca, cmd, sink, stop, "dmesg-W"),
                          daemon=True)
    t1.start()
    time.sleep(2.5)
    print("=== 后台跟随已启动（dmesg -W）===\n")

    # 连接 B：加载模块
    print("=== 连接 B：sudo insmod log_levels.ko ===")
    cb = connect()
    out, err = run(cb, sudo("cd %s && insmod ./log_levels.ko" % REMOTE))
    print("insmod 命令自身输出: %r" % (out,))
    if err:
        print("insmod stderr: %s" % err)
    print("  → SSH 终端没有自动冒出任何内核消息（命令返回为空）\n")

    time.sleep(3)
    stop.set()
    t1.join(timeout=3)

    print("=== 连接 A 实时捕获到的新消息 ===")
    hits = [(t, l) for t, l in sink if 'log_levels' in l]
    if not hits:
        print("(没抓到，共收到 %d 行)" % len(sink))
        for t, l in sink[:5]:
            print("[%s] %s" % (t, l))
    for t, l in hits:
        print("[%s] %s" % (t, l))

    # 清理
    run(cb, sudo("rmmod log_levels 2>/dev/null; echo done"))
    ca.close()
    cb.close()


if __name__ == "__main__":
    main()
