# -*- coding: utf-8 -*-
"""把 STUN 探测脚本上传到树莓派并执行。"""
import os
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
import paramiko

HOST = os.environ.get("RPI_HOST", "192.168.31.109")
USER = os.environ.get("RPI_USER", "wzp")
PASSWORD = os.environ.get("RPI_PASSWORD")
if not PASSWORD:
    print("[错误] 请设置环境变量 RPI_PASSWORD")
    sys.exit(2)

LOCAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stun_probe.py")
REMOTE = "/tmp/stun_probe.py"

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, port=22, username=USER, password=PASSWORD,
          timeout=15, allow_agent=False, look_for_keys=False)
print("=== 已连接 %s@%s ===\n" % (USER, HOST))

sftp = c.open_sftp()
sftp.put(LOCAL, REMOTE)
sftp.close()
print("[已上传] %s -> %s\n" % (LOCAL, REMOTE))

stdin, stdout, stderr = c.exec_command("python3 %s" % REMOTE, timeout=120)
out = stdout.read().decode("utf-8", "replace")
err = stderr.read().decode("utf-8", "replace")
print(out)
if err.strip():
    print("[stderr]", err)

c.exec_command("rm -f %s" % REMOTE)
c.close()
