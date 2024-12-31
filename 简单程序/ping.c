#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>
#include <time.h>
#include <errno.h>

// 定义 ICMP 报文的最大长度
#define PACKET_SIZE 64

// 计算校验和（checksum）
unsigned short checksum(void *b, int len) {
    unsigned short *buf = b;
    unsigned int sum = 0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2) {
        sum += *buf++;
    }

    if (len == 1) {
        sum += *(unsigned char *)buf;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;

    return result;
}

// 创建 ICMP 报文
void create_icmp_packet(struct icmphdr *icmp_hdr, int sequence) {
    icmp_hdr->type = ICMP_ECHO;    // ICMP 回显请求
    icmp_hdr->code = 0;
    icmp_hdr->un.echo.id = getpid();
    icmp_hdr->un.echo.sequence = htons(sequence);  // 序列号
    icmp_hdr->checksum = 0;                        // 先置为 0 再计算校验和
    icmp_hdr->checksum = checksum(icmp_hdr, sizeof(struct icmphdr));
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <hostname>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *hostname = argv[1];
    struct sockaddr_in addr;
    struct hostent *host_entity;

    // 解析主机名或 IP 地址
    if ((host_entity = gethostbyname(hostname)) == NULL) {
        perror("gethostbyname");
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr = *((struct in_addr *)host_entity->h_addr);

    // 创建原始套接字
    int sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int sequence = 0;
    while (1) {
        struct icmphdr icmp_hdr;
        memset(&icmp_hdr, 0, sizeof(icmp_hdr));

        // 创建 ICMP 报文
        create_icmp_packet(&icmp_hdr, sequence);

        // 发送 ICMP 报文
        if (sendto(sockfd, &icmp_hdr, sizeof(icmp_hdr), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("sendto");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // 接收 ICMP 响应
        char buffer[PACKET_SIZE];
        struct sockaddr_in r_addr;
        socklen_t addr_len = sizeof(r_addr);

        ssize_t bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&r_addr, &addr_len);
        if (bytes_received < 0) {
            perror("recvfrom");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // 解析 ICMP 响应
        struct iphdr *ip_hdr = (struct iphdr *)buffer;
        struct icmphdr *recv_icmp_hdr = (struct icmphdr *)(buffer + (ip_hdr->ihl * 4));

        if (recv_icmp_hdr->type == ICMP_ECHOREPLY && recv_icmp_hdr->un.echo.id == getpid()) {
            printf("Received ICMP Echo Reply from %s: Sequence=%d\n", inet_ntoa(r_addr.sin_addr), ntohs(recv_icmp_hdr->un.echo.sequence));
        } else {
            printf("Received ICMP packet of type %d\n", recv_icmp_hdr->type);
        }

        sequence++;
        sleep(1);  // 等待1秒后再发送下一个请求
    }

    close(sockfd);
    return 0;
}