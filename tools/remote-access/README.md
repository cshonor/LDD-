# 远程访问家里的树莓派（Tailscale 方案）

> 场景：笔记本要带去公司，树莓派放家里，需要随时 SSH 回去做内核/驱动实验。
> 真机环境：树莓派 5，Debian 13 (trixie)，aarch64，内核 6.18.34+rpt-rpi-2712，走 WiFi。
> 全部结论均为本机实测，非抄文档。

**章节导航**：[01-first](../01-first/README.md) · [02-log-levels](../02-log-levels/README.md) · **tools/remote-access（本文）**

---

## 本节讲什么

回答一个问题：**不在家里的局域网时，怎么 SSH 回树莓派？**

顺带回答一个更实用的问题：**淘宝三十几块钱的 4G 随身 WiFi，能不能解决这件事？**（答案是不能，而且会更糟，见第 1 节。）

本文包含：

1. 为什么「树莓派能上网」和「你能连回树莓派」是两件完全相反的事
2. 怎么自己诊断家里的网络到底能不能穿透（附 STUN 探测脚本）
3. 三种方案对比与选型理由
4. Tailscale 完整部署步骤
5. **两个不做就会后悔的加固**（开机自愈 + 密钥过期）
6. 常见坑、故障排查决策树、自测题

---

## 0. 结论先说

| 你想做的事 | 可行性 | 说明 |
|---|---|---|
| 买 4G 随身 WiFi 让树莓派上网，然后从公司连回来 | ❌ **更糟** | 4G 走 CGNAT，连公网地址都没有，从「挡一层」变「挡两层」 |
| 什么也不做，直接 `ssh wzp@192.168.31.109` | ❌ | `192.168.x.x` 是私有地址，只在家里有效 |
| 路由器端口转发 + DDNS | ⚠️ 可行但有代价 | SSH 暴露公网，会被全网扫描爆破；公司常封 22/2222 |
| **Tailscale（本文方案）** | ✅ **推荐** | 免费、不动路由器、不暴露任何端口 |
| FRP + 云服务器 | ✅ 最稳 | 走 443 能穿透绝大多数公司防火墙，约 24 元/月 |

**本仓库的方案**：Tailscale 为主力。若到公司发现 UDP 被封，再叠一层 FRP 即可，两者不冲突。

> ⚠️ 动手前先确认公司允许私建隧道。部分公司对这类行为有合规要求。

---

## 1. 为什么 4G 随身 WiFi 解决不了问题

### 1.1 卡住的是「入站」，不是「出站」

先厘清一个很多人混淆的点。树莓派现在**上网完全正常**（能 `curl` 到公网、能 `apt update`）。所以问题从来不是「树莓派怎么连出去」，而是：

> 你在公司发起一个连接，数据包到了你家路由器，路由器查 NAT 表，发现「没有人主动要过这条连接」，**直接丢弃**。

```
   出网（正常）                          入网（被拒）
   树莓派 → 路由器 → 互联网               你的笔记本 → 互联网 → 你家路由器 → ❓

   路由器记下：                           路由器查表：
   192.168.31.109:54321                  没有 (101.204.67.155:22) 这条记录
     → 101.204.67.155:27400                 → 丢弃，连 ICMP 都不回
   （NAT 表有条目，回程能找到路）           （NAT 表没有条目，不知道该给谁）
```

NAT 表的条目是**内网主机主动发包时创建的**。外网想主动进来，在表里有条目之前一律被挡。

### 1.2 4G 随身 WiFi 会让它更糟

家宽（本例）至少有**真公网 IPv4 `101.204.67.155`**——这个地址在全球是唯一的，可以被寻址。

4G 网络几乎全部走 **CGNAT（运营商级 NAT）**：

| | 你家宽 | 4G 随身 WiFi |
|---|---|---|
| 拿到什么地址 | 公网 IPv4 `101.204.67.155` | `100.64.x.x`（RFC 6598 共享地址空间） |
| 能否被外网寻址 | ✅ 能 | ❌ 不能，几十个用户共享同一个出口 |
| 挡了几层 | 1 层（你家路由器） | **2 层（运营商 NAT + 设备 NAT）** |
| 端口转发能不能配 | 能（进路由器后台） | 不能，你没有运营商 NAT 的控制权 |

`100.64.0.0/10` 这段地址是专门给 CGNAT 保留的，**在互联网上不可路由**。换了 4G 之后，你连「被寻址的资格」都没了。

### 1.3 那 IPv6 呢？

理论上 IPv6 地址足够多，每设备一个全球地址，直连即可，无需穿透。但实测本机：

```bash
$ ip -6 -br addr show | grep -v lo
fe80::xxxx:xxxx:xxxx:xxxx/64     # 只有 fe80:: 开头的链路本地地址
```

`fe80::` 只在同一个二层链路内有效，出不了网。小米路由器没有下发 IPv6 前缀（需要在路由器后台开启，且运营商要支持）。

所以这条路径**暂时走不通**，记录下来备用。

---

## 2. 先诊断：你家的网络到底能不能穿透

别猜，测。三个问题决定了所有方案的成败：

| 问题 | 影响 |
|---|---|
| 有没有**真公网 IPv4**（不是 CGNAT） | 决定端口转发能不能用 |
| NAT 是**锥型**还是**对称型** | 决定 UDP 打洞成功率 |
| 有没有**全局 IPv6** | 决定能否免穿透直连 |

### 2.1 用 STUN 一次性测完

本目录的 `stun_probe.py` 是纯标准库实现（不用装任何包），通过 STUN 协议问外网服务器「我出去之后长什么样」：

```bash
# 在树莓派上跑
python3 stun_probe.py

# 或者从你的电脑远程跑（自动上传 + 执行）
RPI_PASSWORD=你的密码 python run_stun.py
```

**本机实测输出**：

```
本地绑定端口: 37890

服务器           外部看到的地址              结果
------------------------------------------------------------
Google       101.204.67.155:27400      成功
Twilio       101.204.67.155:27400      成功

【结论】
  NAT 类型          : 锥型（Cone NAT）
  端点独立映射(EIM) : 是 —— 同一内网端口访问不同外部服务器，映射结果一致
  UDP 打洞成功率    : 高
```

### 2.2 怎么读懂这个结果

关键看两行：

- **外部地址不是 `100.64.x.x`** → 没有 CGNAT，有真公网 IP
- **两个不同服务器、同一本地端口 → 外部映射完全相同** → 这就是**端点独立映射（EIM）**

EIM 是什么意思？

```
情况一：EIM（端点独立映射，锥型 NAT）✅
   树莓派:37890 ──→ Google   →  101.204.67.155:27400
   树莓派:37890 ──→ Twilio   →  101.204.67.155:27400
                                        ↑ 同一个洞，谁都能往里塞包

情况二：非 EIM（对称型 NAT）❌
   树莓派:37890 ──→ Google   →  101.204.67.155:27400
   树莓派:37890 ──→ Twilio   →  101.204.67.155:31922
                                        ↑ 换目标就换洞，打洞基本失败
```

对称型 NAT 下，你告诉对端「我在这儿」，但那个洞只对特定目标有效，对端塞不进来。锥型 NAT 则没有这个限制。

**好消息**：Tailscale 自己也会做同样的判断，实测结果一致——

```bash
$ tailscale netcheck
Report:
	* UDP: true
	* IPv4: yes, 101.204.67.155:27011
	* IPv6: no, but OS has support
	* MappingVariesByDestIP: false        ← false = EIM = 锥型 NAT ✅
	* Nearest DERP: Nuremberg
	* DERP latency:
		- nue: 168.9ms (Nuremberg)
		- hkg: 182.5ms (Hong Kong)
		- lax: 184.5ms (Los Angeles)
```

`MappingVariesByDestIP: false` 和 STUN 探测的结论完全对上。

### 2.3 DERP 延迟说明什么

DERP 是 Tailscale 的**兜底中继**。打洞成功时流量走 P2P 直连，根本不经过 DERP；只有打洞失败才降级到中继。

本机最近的中继节点在纽伦堡（168.9ms）而不是香港（182.5ms）——这个分配不太合理，说明 DERP 选路没有考虑地理最优。但只要我们打洞成功，延迟就由真实链路决定（同城一般 <20ms），跟这 168ms 无关。

**测试方法**：等两个设备都加入后

```bash
tailscale ping rpi5
# 输出 via DERP(xxx) → 走了中继，慢
# 输出 via 101.204.67.155:27400 → 直连，快 ✅
```

---

## 3. 三种方案对比

| 方案 | 成本 | 延迟 | 穿透公司防火墙 | 安全性 | 需要做什么 |
|---|---|---|---|---|---|
| **Tailscale** | 免费 | 低（打洞后 P2P 直连） | 较强（UDP 打洞，失败自动降级 TCP 中继） | **高**（不暴露任何端口） | 浏览器授权一次 + 每台设备装客户端 |
| 端口转发 + DDNS | 免费 | 最低（真直连） | **弱**（公司常封 22/2222，只放行 80/443） | **低**（SSH 暴露公网，几天内必有爆破尝试） | 进小米路由器后台配；必须配密钥登录 + 禁密码 + fail2ban |
| FRP + 云服务器 | ~24 元/月 | 中（经 VPS 中转） | **最好**（可走 443，伪装成 HTTPS） | 中 | 买轻量云服务器，自建 frps/frpc |

### 选 Tailscale 的理由（针对本例）

1. 你家是**锥型 NAT + 真公网 IP**，打洞成功率高——Tailscale 最能发挥的场景
2. **不动路由器**——不用进小米后台折腾，不暴露任何端口
3. **免费**，个人用途 3 用户 / 100 设备以内绰绰有余
4. 打洞失败也能用 DERP 兜底，不会彻底连不上

### 端口转发的真实代价（别低估）

一旦把 22 端口暴露到公网，不出几天就会有来自全球的自动化爆破。这不是恐吓——你可以在任意一台公网服务器上 `grep 'Failed password' /var/log/auth.log` 看看数量级。**必须**同时做：

- 禁密码登录，只用 SSH 密钥
- 装 fail2ban
- 改端口（只能减少噪音，不能替代上面两条）

对新手来说这是额外一整套坑，所以不作为首选。

---

## 4. Tailscale 部署步骤

### 4.1 树莓派安装

Debian 13 (trixie) 有官方源，直接 apt 装：

```bash
# 1. 加官方 GPG key
curl -fsSL https://pkgs.tailscale.com/stable/debian/trixie.noarmor.gpg \
  | sudo tee /usr/share/keyrings/tailscale-archive-keyring.gpg >/dev/null

# 2. 加 apt 源
curl -fsSL https://pkgs.tailscale.com/stable/debian/trixie.tailscale-keyring.list \
  | sudo tee /etc/apt/sources.list.d/tailscale.list >/dev/null

# 3. 安装
sudo apt-get update
sudo apt-get install -y tailscale

# 4. 验证
tailscale version          # 本机实测：1.102.3
systemctl is-enabled tailscaled   # 应为 enabled
```

> 换成别的发行版时，把 URL 里的 `trixie` 换成你的 codename（如 `bookworm`、`jammy`、`noble`）。
> 可以先探一下源是否存在：
> `curl -o /dev/null -w "%{http_code}" https://pkgs.tailscale.com/stable/debian/<codename>.tailscale-keyring.list`

### 4.2 启动并授权

```bash
sudo systemctl start tailscaled
sudo tailscale up --hostname=rpi5 --accept-dns=false
```

会输出一个授权链接，形如：

```
To authenticate, visit:

	https://login.tailscale.com/a/xxxxxxxxxxxx
```

在浏览器打开，用账号登录（GitHub / Google / Microsoft 均可）。授权后：

```bash
$ tailscale status
100.x.y.z   rpi5    cshonor@    linux    -

$ tailscale ip -4
100.105.221.16
```

**参数说明**：

| 参数 | 作用 |
|---|---|
| `--hostname=rpi5` | 指定设备名，之后可以用 `ssh wzp@rpi5` 而不是记 IP |
| `--accept-dns=false` | **不接管 DNS**。默认开启会让 Tailscale 的 MagicDNS 接管系统 DNS 解析，装了 systemd-resolved 的机器上容易和本地解析打架，关掉更省心 |

### 4.3 笔记本安装

- Windows：下载 `https://pkgs.tailscale.com/stable/tailscale-setup-latest.exe`
  或 `winget install Tailscale.Tailscale`
- 装完后在系统托盘点图标 → Log in → **用和树莓派同一个账号**

两台设备登录同一账号后，会自动出现在同一个 tailnet 里。

### 4.4 验证打洞是否成功

```bash
# 从笔记本
tailscale ping rpi5

# 或直接 SSH（设备名自动解析，不用记 100.x 地址）
ssh wzp@rpi5
```

确认走的是直连还是中继：

```bash
tailscale status
# 或在笔记本上
tailscale ping -c 3 rpi5
# 显示 via 101.204.67.155:xxxxx  = 直连 ✅
# 显示 via DERP(nue)             = 中继，延迟 168ms+
```

---

## 5. 关键加固（不做迟早出事）

这两条是**无人值守场景**下最容易踩的雷，都发生在你人不在家的时候。

### 5.1 开机自愈：补 `network-online.target` 依赖

**问题**：官方 `tailscaled.service` 的启动依赖是这样写的——

```bash
$ systemctl cat tailscaled | grep -E '^(After|Wants)'
Wants=network-pre.target
After=network-pre.target NetworkManager.service systemd-resolved.service
```

注意它有 `network-pre.target` 但**没有 `network-online.target`**。这两个不是一回事：

| target | 何时算「达成」 | 网络真的能用吗 |
|---|---|---|
| `network-pre.target` | 网络栈**开始配置之前** | ❌ 还没有 IP |
| `network-online.target` | 网络配置**完成，IP 已拿到** | ✅ 能发包 |

树莓派走 WiFi，从 `network-pre` 到真正拿到 IP 可能差好几秒。结果是：

> `tailscaled` 启动了，但那一刻还没有可用网络，隧道建不起来。

更糟的是，`Restart=on-failure` **只在进程崩溃时重启**。「进程活着但没网」这种情况 systemd 认为服务是健康的，不会重启——然后你就会在公司发现自己连不回家，而树莓派上一切「看起来正常」。

**修复**：加一个 systemd drop-in。

```bash
sudo mkdir -p /etc/systemd/system/tailscaled.service.d
sudo install -m 0644 tailscaled-override.conf \
  /etc/systemd/system/tailscaled.service.d/override.conf
sudo systemctl daemon-reload
```

也可以一步到位：

```bash
RPI_PASSWORD=你的密码 python fix_boot_order.py
```

**验证生效**：

```bash
$ systemctl cat tailscaled | grep -E '^(After|Wants|Restart)'
Wants=network-pre.target
After=network-pre.target NetworkManager.service systemd-resolved.service
Restart=on-failure
After=network-online.target          ← 新增 ✅
Wants=network-online.target          ← 新增 ✅
Restart=on-failure
RestartSec=5                          ← 新增 ✅
```

`drop-in` 的内容见本目录 `tailscaled-override.conf`。

> ⚠️ 前提：`network-online.target` 要真的会等待。本机 `NetworkManager-wait-online.service` 是 `enabled` 的，所以有效。
> 检查：`systemctl list-unit-files | grep wait-online`
> 如果用的是 `systemd-networkd`，需要 `sudo systemctl enable systemd-networkd-wait-online`。

### 5.2 密钥过期：默认 180 天后掉线

**这是最容易忽略的一条。** Tailscale 的节点密钥默认 **180 天**后过期，过期后设备会掉出 tailnet，需要重新在浏览器授权——而那时你可能正在公司。

实测本机：

```bash
$ tailscale status --json | python3 -c "import sys,json; print(json.load(sys.stdin)['Self']['KeyExpiry'])"
2027-02-25T11:26:36Z
```

对应 2026-08-29 授权 + 180 天。

**解决**：登录 <https://login.tailscale.com/admin/machines>，找到 `rpi5` → 点右侧 `...` → **Disable key expiry**（禁用密钥过期）。

对于放在家里不常碰的设备，这一项应当无条件开启。

### 5.3 部署完成检查清单

```bash
# 在树莓派上逐条确认
systemctl is-active tailscaled          # active
systemctl is-enabled tailscaled         # enabled（开机自启）
systemctl show tailscaled -p After | grep network-online   # 已包含
tailscale status                        # 能看到自己，BackendState=Running
tailscale netcheck | grep -E 'UDP|MappingVariesByDestIP'   # UDP: true / false
sudo ss -tlnp | grep :22                # sshd 监听 0.0.0.0:22（tailnet 也能连进来）
```

---

## 6. 安全加固（可选但推荐）

Tailscale 已经避免了「端口暴露公网」这个最大的风险面。剩下的建议：

| 措施 | 命令 / 做法 | 优先级 |
|---|---|---|
| 开启 SSH 密钥登录 | 本地 `ssh-copy-id wzp@rpi5`，然后 `PasswordAuthentication no` | 高 |
| 装 fail2ban | `sudo apt install fail2ban` | 中 |
| 关闭密钥过期 | 管理后台 Disable key expiry | **高**（见 5.2） |
| 不要用 `--advertise-exit-node` | 除非你明确想让树莓派当网关 | — |
| 不要用 `--accept-routes` | 除非你明确想让树莓派访问你的内网 | — |

> 关于 `--accept-routes`：不加的话，树莓派只把它自己加进 tailnet；加了之后，你在公司还能通过树莓派访问家里**整个局域网**的其他设备（路由器后台、NAS 等）。功能强大，但等于把家里的网络暴露给你的 tailnet，按需开启。

---

## 7. 常见坑

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | `tailscale up` 报 `Error reading SSH protocol banner` | 之前反复输错密码触发了 SSH 认证限流 | 等几十秒重试，别连续猛试 |
| 2 | `tailscaled` 起来了但 `tailscale status` 显示未登录 | 没在浏览器完成授权 | 重新 `sudo tailscale up` 拿新链接 |
| 3 | 授权链接打不开 / 提示过期 | 链接一次性且有时效 | 重新 `sudo tailscale up` 生成新链接 |
| 4 | 装客户端时创建 TUN 接口导致 SSH 瞬时断连 | `tailscaled` 创建 `tailscale0` 网卡时的网络抖动 | 无害，等 2 秒重连即可 |
| 5 | 重启后 `tailscaled` 起不来 / 隧道不通 | 缺少 `network-online.target` 依赖 | 见 5.1 |
| 6 | 半年后突然连不上 | 节点密钥 180 天过期 | 见 5.2 |
| 7 | `tailscale ping` 显示 `via DERP` | UDP 打洞失败，走了中继 | 延迟会高（本机 168ms+）；检查公司网络是否禁 UDP |
| 8 | 装完后本地 DNS 解析出问题 | MagicDNS 接管了系统 DNS | 用 `--accept-dns=false` 重新 up |
| 9 | `apt-get update` 报 Tailscale 源 404 | 发行版 codename 不对（如把 trixie 写成 bookworm） | 核对 `/etc/os-release` 的 `VERSION_CODENAME` |
| 10 | 公司网络下完全连不上 | 公司防火墙禁 UDP，或禁止了 Tailscale 的控制面域名 | 上 FRP 走 443，或先确认公司是否允许 |
| 11 | `sudo -S -p ''` 从 stdin 喂密码静默失败 | 部分环境下 sudo 拿不到密码，且 stderr 被丢弃后极难排查 | 用 `echo 'pwd' \| sudo -S sh -c '...'`（见 `fix_boot_order.py`） |

---

## 8. 故障排查决策树

```
连不上 rpi5
│
├─ 树莓派本身在线吗？（用家里另一台设备 ping 192.168.31.109）
│   └─ 不在线 → 电源 / WiFi / SD 卡问题，Tailscale 无能为力
│
├─ tailscaled 在跑吗？（需要能先连进去）
│   └─ systemctl is-active tailscaled → 不是 active
│       └─ journalctl -u tailscaled -n 50 看报错
│
├─ 授权过期了吗？
│   └─ tailscale status --json | grep KeyExpiry
│       └─ 已过期 → 管理后台重新授权，并 Disable key expiry
│
├─ 打洞成功吗？
│   └─ tailscale ping rpi5
│       ├─ via 101.204.67.155:xxxxx → 直连，正常 ✅
│       └─ via DERP(xxx) → 中继
│           └─ tailscale netcheck 看 UDP 是否 true
│               └─ UDP: false → 公司网禁 UDP，考虑 FRP
│
└─ SSH 本身的问题？
    └─ sudo ss -tlnp | grep :22
        └─ 没监听 → sshd 挂了，systemctl restart sshd
```

**保底方案**：在公司连不上时，让家里人帮忙看一眼树莓派的电源灯；或者给树莓派配一个智能插座，能远程断电重启。

---

## 9. HFT / 嵌入式关联

| 主题 | 关联 |
|---|---|
| **NAT 与连接状态** | NAT 表条目有超时（TCP 通常几十分钟到几小时）。长连接要保活，否则空闲后条目被清， tunnel 就断了——这是 HFT 里长连接心跳机制的现实基础 |
| **P2P 打洞** | 本质是「双方同时向外发包，在 NAT 上撞出一个洞」。需要第三方（STUN / 信令服务器）交换地址信息，属于经典的 NAT traversal 问题 |
| **UDP vs TCP 打洞** | UDP 无连接，打洞容易得多；TCP 打洞需要同时 SYN（TCP simultaneous open），成功率低。Tailscale 优先 UDP，降级才走 TCP 中继——这就是为什么它是 WireGuard 而不是 OpenVPN |
| **中继延迟的代价** | 本机 DERP 延迟 168ms（纽伦堡）vs 直连同城 <20ms，差 8 倍以上。对低延迟场景，**中继和直连是两个世界** |
| **嵌入式现场调试** | 工业现场的设备通常在 NAT 后面、无公网 IP、无显示器。Tailscale 这类 overlay 网络是远程维护的标准做法；但注意设备端要有可靠的看门狗和断线重连（对应本文 5.1 的开机自愈思路） |
| **CGNAT 地址空间** | `100.64.0.0/10`（RFC 6598）是给运营商 NAT 保留的。看到这个段就要警觉：没有公网可达性 |

---

## 10. 自测题

<details>
<summary>Q1. 树莓派能正常 `curl` 百度，为什么我从公司 SSH 不回去？</summary>

出网和入网是两回事。出网时 NAT 表会创建条目，回程包能找到路；入网时 NAT 表里没有对应条目，路由器直接丢弃。

NAT 表条目只能由**内网主机主动发包**创建，这是 NAT 的核心不对称性。

</details>

<details>
<summary>Q2. 买个 4G 随身 WiFi 能解决吗？为什么反而更糟？</summary>

不能，而且更糟。4G 网络走 CGNAT，分配的是 `100.64.0.0/10` 共享地址，互联网上不可路由，连「被寻址的资格」都没有。

原本只挡 1 层（你家路由器），换成 4G 后挡 2 层（运营商 NAT + 设备 NAT），且你无法配置运营商那一层。

</details>

<details>
<summary>Q3. STUN 探测里「两个不同服务器返回相同外部端口」说明什么？</summary>

说明是**端点独立映射（EIM）**，即锥型 NAT。

同一内网端口访问任何外部目标，映射结果都一样，意味着洞是「通用」的——对端只要知道这个地址就能塞包进来，UDP 打洞成功率高。

对称型 NAT 则换目标就换洞，打洞基本失败。

</details>

<details>
<summary>Q4. `tailscaled` 服务是 active，但隧道没通，可能是什么原因？</summary>

最可能是启动时机问题：官方 unit 只依赖 `network-pre.target`（网络开始配置前就算完成），树莓派走 WiFi 时此刻可能还没有 IP。

`Restart=on-failure` 只在进程崩溃时重启，「进程活着但没网」它不管。

修复：加 drop-in 补 `After=network-online.target` + `Wants=network-online.target`。

</details>

<details>
<summary>Q5. 半年后突然连不上，最可能是什么原因？</summary>

Tailscale 节点密钥默认 180 天过期。过期后设备掉出 tailnet，需要重新在浏览器授权——而那时你可能正在公司，无法物理接触设备。

预防：管理后台把该设备设为 Disable key expiry。

</details>

<details>
<summary>Q6. `tailscale ping` 显示 `via DERP(nue)` 而不是 `via 101.204.67.155:xxxx`，意味着什么？</summary>

UDP 打洞失败，流量走了 DERP 中继服务器（本机最近的是纽伦堡，168.9ms）。

功能上还能用，但延迟会显著高于直连。常见原因是公司防火墙禁 UDP。此时可考虑 FRP 走 443 端口。

</details>

<details>
<summary>Q7. 为什么推荐 `--accept-dns=false`？</summary>

默认开启时 Tailscale 的 MagicDNS 会接管系统 DNS 解析（通过 systemd-resolved）。在装了 systemd-resolved 的机器上容易和本地解析规则冲突，导致域名解析出问题。

对于「只是想 SSH 回去」的场景，不需要 MagicDNS，关掉更省心。需要用它时再开。

</details>

<details>
<summary>Q8. 端口转发方案下，为什么必须禁密码登录？</summary>

SSH 一旦暴露到公网，几天内就会有来自全球的自动化爆破（扫 IP 段 + 常见用户名/密码字典）。密码再复杂也只是拖延时间——而且会消耗 CPU 和日志空间。

禁密码 + 只用密钥登录是质变：攻击者面对的是不可暴力破解的非对称密钥。配合 fail2ban 封禁反复失败的 IP。

</details>

---

## 文件清单

| 文件 | 说明 |
|---|---|
| `README.md` | 本文 |
| `stun_probe.py` | 纯标准库 STUN 探测，判断公网 IP / NAT 类型 / EIM |
| `run_stun.py` | 从本地上传到树莓派并远程执行 `stun_probe.py` |
| `tailscaled-override.conf` | 开机自愈 drop-in（补 `network-online.target` 依赖） |
| `fix_boot_order.py` | 自动安装上述 drop-in 并校验生效 |

所有脚本的凭据均从环境变量读取，不硬编码：

```bash
export RPI_HOST=192.168.31.109      # 默认值
export RPI_USER=wzp                 # 默认值
export RPI_PASSWORD=你的密码         # 必填，缺失时脚本直接退出
```

> ⚠️ 本仓库是**公开**仓库，提交任何脚本前务必确认没有硬编码密码、密钥、token。
