#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
纯标准库实现的 STUN 探测 —— 判断 NAT 类型与外部映射地址。

用途：评估从公司远程连回家里树莓派的可行性。
  - 外部映射 IP 是否等于路由器 WAN IP   -> 判断是否 CGNAT
  - 同一本地端口访问不同目的地址时映射是否稳定 -> 判断 UDP 打洞（Tailscale）能否成功

RFC 5389 / RFC 4787
"""
import socket
import struct
import random
import sys

MAGIC = 0x2112A442

SERVERS = [
    ("stun.l.google.com", 19302, "Google"),
    ("stun1.l.google.com", 19302, "Google-1"),
    ("global.stun.twilio.com", 3478, "Twilio"),
    ("stun.miwifi.com", 3478, "MiWiFi"),
    ("stun.voipbuster.com", 3478, "VoipBuster"),
    ("stun.sipgate.net", 3478, "Sipgate"),
]


def make_request():
    tid = bytes(random.getrandbits(8) for _ in range(12))
    return struct.pack("!HHI", 0x0001, 0, MAGIC) + tid


def parse_response(data):
    """从 Binding Success Response 里解出映射地址（优先 XOR-MAPPED-ADDRESS）。"""
    if len(data) < 20:
        return None
    mtype, _mlen, cookie = struct.unpack("!HHI", data[:8])
    if mtype != 0x0101 or cookie != MAGIC:
        return None
    body = data[20:]
    mapped = xor = None
    i = 0
    while i + 4 <= len(body):
        atype, alen = struct.unpack("!HH", body[i:i + 4])
        val = body[i + 4:i + 4 + alen]
        if atype == 0x0001 and alen >= 8:
            port = struct.unpack("!H", val[2:4])[0]
            mapped = (socket.inet_ntoa(val[4:8]), port)
        elif atype == 0x0020 and alen >= 8:
            port = struct.unpack("!H", val[2:4])[0] ^ (MAGIC >> 16)
            ipb = bytes(b ^ m for b, m in zip(val[4:8], struct.pack("!I", MAGIC)))
            xor = (socket.inet_ntoa(ipb), port)
        i += 4 + ((alen + 3) // 4) * 4
    return xor or mapped


def stun(server, port, src_port=0, timeout=5):
    """发一次 Binding Request；src_port=0 表示由系统分配端口。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.bind(("", src_port))
        local = s.getsockname()
        s.sendto(make_request(), (server, port))
        data, _ = s.recvfrom(2048)
        return parse_response(data), local
    except Exception:
        return None, None
    finally:
        s.close()


def find_servers():
    """找出至少两个可用的 STUN 服务器。

    关键：必须是**不同的 IP**。很多 STUN 域名走任播会解析到同一地址
    （例如 stun.l.google.com 与 stun1.l.google.com 都是 74.125.250.129），
    若两次探测发往同一目的地，映射行为测试将失去意义。
    """
    ok = []
    seen = set()
    for host, port, name in SERVERS:
        try:
            ip = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)[0][4][0]
        except Exception:
            print("  %-22s DNS 解析失败" % name)
            continue
        if ip in seen:
            print("  %-22s %-18s 跳过（IP 与已选服务器重复）" % (name, ip))
            continue
        res, _ = stun(ip, port)
        if res:
            seen.add(ip)
        if res:
            print("  %-22s %-18s 可用 (返回 %s:%d)" % (name, ip, res[0], res[1]))
            ok.append((ip, port, name))
        else:
            print("  %-22s %-18s 无响应" % (name, ip))
        if len(ok) >= 2:
            break
    return ok


def main():
    print("=" * 66)
    print("STUN NAT 探测 —— 评估远程连回树莓派的可行性")
    print("=" * 66)

    # 本机地址（用于对比是否被 NAT）
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("223.5.5.5", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except Exception:
        local_ip = "未知"
    print("\n[1] 本机内网 IP: %s" % local_ip)

    print("\n[2] 探测可用的 STUN 服务器:")
    servers = find_servers()
    if not servers:
        print("\n所有 STUN 服务器均无响应——出网 UDP 可能被封锁，")
        print("Tailscale 类打洞方案需要改用 TCP/HTTPS 中继模式。")
        return

    ip1, port1, name1 = servers[0]
    print("\n[3] NAT 映射行为测试 (RFC 4787):")

    # 测试 A：固定本地端口 -> 服务器1
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    fixed_port = s.getsockname()[1]
    s.close()
    m1, _ = stun(ip1, port1, src_port=fixed_port)

    # 测试 B：同一个本地端口 -> 服务器2（目的地址不同）
    m2 = None
    if len(servers) > 1:
        ip2, port2, name2 = servers[1]
        m2, _ = stun(ip2, port2, src_port=fixed_port)
    else:
        # 只有一个服务器时，换端口测（弱判断）
        m2, _ = stun(ip1, 19303 if port1 != 19303 else 3479, src_port=fixed_port)

    print("    本地端口 %d -> %-18s 得到外部映射: %s" % (
        fixed_port, name1, ("%s:%d" % m1) if m1 else "失败"))
    print("    本地端口 %d -> 另一目的          得到外部映射: %s" % (
        fixed_port, ("%s:%d" % m2) if m2 else "失败"))

    print("\n" + "=" * 66)
    print("结论")
    print("=" * 66)

    if not m1:
        print("未拿到映射结果，无法判断。")
        return

    ext_ip = m1[0]
    print("\n外部映射地址: %s:%d" % m1)
    print("本机内网地址: %s" % local_ip)

    # 判断是否 CGNAT
    cgnat = ext_ip.startswith("100.64.") or ext_ip.startswith("10.") \
        or ext_ip.startswith("172.16.") or ext_ip.startswith("192.168.")
    print("\n① NAT 层级判断:")
    if ext_ip == local_ip:
        print("   外部 IP == 本机 IP，说明树莓派直接拥有公网地址（无 NAT）。")
    elif cgnat:
        print("   外部映射落在私有/共享地址段 %s —— 处于运营商级 NAT (CGNAT) 之后。" % ext_ip)
        print("   => 从公司直接 SSH 进来是不可能的，必须走打洞或中继。")
    else:
        print("   外部映射是公网地址 %s —— 路由器 WAN 口大概率是真公网 IPv4。" % ext_ip)
        print("   => 在路由器上做端口转发 + DDNS，有望直接 SSH 进来。")

    print("\n② UDP 打洞可行性 (Tailscale / ZeroTier):")
    if m1 and m2:
        if m1[0] == m2[0] and m1[1] == m2[1]:
            print("   两次访问不同目的，映射结果完全一致 -> 端点独立映射 (EIM)")
            print("   => UDP 打洞成功率高，Tailscale 大概率能建立直连（不走中继，延迟低）。")
        elif m1[0] == m2[0]:
            print("   IP 相同但端口变化 -> 地址相关映射")
            print("   => 打洞可能成功，部分场景会降级到中继节点。")
        else:
            print("   IP 和端口都变化 -> 对称型 NAT")
            print("   => 打洞通常失败，会走中继（Tailscale DERP），能用但延迟较高。")
    else:
        print("   数据不足，无法判断。")

    print("\n③ 提示: 上述结论仅针对 UDP。SSH 原生是 TCP，即使 NAT 是锥型，")
    print("   外网也无法直接发起 TCP 入站连接（路由器没有映射条目会直接丢弃）。")


if __name__ == "__main__":
    main()
