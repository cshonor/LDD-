/* sniff.c —— 网络设备例子（用户态 AF_PACKET 抓包）
 *
 * 网络设备（eth0/wlan0）与字符/块设备的根本区别：
 *   1. /dev 下没有它的节点 —— ls /dev 里永远找不到 eth0
 *   2. 接口不走 read/write 系统调用，而是 socket
 *   3. 传输单位不是字节流也不是块，而是报文（内核里是 skb）
 *
 * 本程序用 AF_PACKET 原始套接字在链路层收包，
 * 能看到以太网帧头：目标 MAC / 源 MAC / 以太类型。
 * 每收一包就是"一个报文"，没有"读到半个包"的概念 —— 报文是原子的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

static void print_mac(const unsigned char *m)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

int main(int argc, char **argv)
{
    const char *ifname = argc > 1 ? argv[1] : "eth0";
    int want = argc > 2 ? atoi(argv[2]) : 5;
    unsigned char buf[2048];

    /* PF_PACKET：绕过协议栈直接到链路层（相当于用户态挂到驱动出口上） */
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket(AF_PACKET)");   /* 没 root 会 EPERM */
        return 1;
    }

    struct sockaddr_ll sll = {0};
    sll.sll_family  = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = (int)if_nametoindex(ifname);
    if (!sll.sll_ifindex) {
        fprintf(stderr, "no such interface: %s\n", ifname);
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
        perror("bind");
        return 1;
    }

    printf("sniffing on %s (idx=%d), waiting for %d packets...\n",
           ifname, sll.sll_ifindex, want);

    for (int i = 0; i < want; i++) {
        ssize_t len = recv(fd, buf, sizeof buf, 0);
        if (len < (ssize_t)sizeof(struct ethhdr))
            continue;

        struct ethhdr *eh = (struct ethhdr *)buf;
        printf("#%d %4zd B  ", i + 1, len);
        print_mac(eh->h_source);
        printf(" -> ");
        print_mac(eh->h_dest);
        printf("  type=0x%04x", ntohs(eh->h_proto));
        if (ntohs(eh->h_proto) == ETH_P_IP)
            printf(" (IPv4)");
        else if (ntohs(eh->h_proto) == ETH_P_ARP)
            printf(" (ARP)");
        else if (ntohs(eh->h_proto) == ETH_P_IPV6)
            printf(" (IPv6)");
        printf("\n");
    }
    close(fd);
    return 0;
}
