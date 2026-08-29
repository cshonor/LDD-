# -*- coding: utf-8 -*-
"""
给树莓派上的 tailscaled 补 network-online.target 启动依赖。

凭据从环境变量读取（RPI_HOST / RPI_USER / RPI_PASSWORD），不硬编码。
"""
import os
import sys
import io
import shlex

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import paramiko

HOST = os.environ.get("RPI_HOST", "192.168.31.109")
USER = os.environ.get("RPI_USER", "wzp")
PASSWORD = os.environ.get("RPI_PASSWORD")
if not PASSWORD:
    print("[错误] 未设置环境变量 RPI_PASSWORD")
    sys.exit(2)

LOCAL_CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "tailscaled-override.conf")
REMOTE_DIR = "/etc/systemd/system/tailscaled.service.d"
REMOTE_CONF = REMOTE_DIR + "/override.conf"

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, port=22, username=USER, password=PASSWORD,
          timeout=15, allow_agent=False, look_for_keys=False)


def sudo(cmd, timeout=60):
    """用 echo <pwd> | sudo -S 传密码。

    注意：不要用 `sudo -S -p ''` 再从 stdin 写入 —— 实测在部分环境下
    sudo 拿不到密码，命令静默失败（而 stderr 又被丢弃，极难排查）。
    用管道喂密码最稳。
    """
    _, stdout, stderr = c.exec_command(
        "echo '%s' | sudo -S sh -c %s" % (PASSWORD, shlex.quote(cmd)), timeout=timeout
    )
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    if err.strip() and "sudo" not in err.lower():
        out += "[stderr] " + err
    return out


sftp = c.open_sftp()
tmp = "/tmp/ts-override.conf"
sftp.put(LOCAL_CONF, tmp)
sftp.close()
print("[1] 已上传 drop-in -> %s" % tmp)

print("\n[2] 安装到 %s" % REMOTE_CONF)
print(sudo("mkdir -p %s && install -m 0644 %s %s && cat %s"
           % (REMOTE_DIR, tmp, REMOTE_CONF, REMOTE_CONF)))

print("\n[3] 重新加载 systemd 并校验")
out = sudo("systemctl daemon-reload; systemd-analyze verify /lib/systemd/system/tailscaled.service 2>&1 | head -10; echo '(以上为空或仅 warning 即正常)'")
print(out)

print("\n[4] 生效后的启动顺序")
print(sudo("systemctl cat tailscaled | grep -E '^(After|Wants|Requires|Restart|RestartSec)'"))

print("\n[5] 服务状态（注意：未重启，当前实例仍在跑，配置下次启动生效）")
print(sudo("systemctl is-active tailscaled; systemctl is-enabled tailscaled"))

c.close()
