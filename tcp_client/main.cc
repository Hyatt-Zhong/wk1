#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 16663
struct message
{
    int cmd;
    char param[256];
};

    // pthread_t atestid;
	// pthread_create(&atestid, NULL, audio_test_thread_func, (void *)handler);
	// pthread_detach(atestid);

void handle(char* buf,int len, void* args){
	// ZRT_AudioHandler *audio = (ZRT_AudioHandler *)args;
	// struct message *pmsg=buf;

	// switch (pmsg->cmd)
	// {
	// case 1:
	// 	pthread_mutex_lock(&audio->mutex);
    // 	snprintf(audio->sToneFileName, sizeof(audio->sToneFileName), "%s", pmsg->param);
    // 	pthread_mutex_unlock(&audio->mutex);
	// 	break;
	
	// default:
	// 	break;
	// }

}

static void* audio_test_thread_func(void* args){

    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    fd_set readfds;
    int activity, i, addrlen, errno;
    int BUFFER_SIZE=512;
    char *success="success";
    char buffer[BUFFER_SIZE];

    // 创建套接字
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 初始化服务器地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    // 将套接字绑定到服务器地址
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // 监听连接
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Server started. Waiting for connections...\n");

    while (1) {
        // 清空文件描述符集合
        FD_ZERO(&readfds);

        // 将服务器套接字添加到文件描述符集合
        FD_SET(server_fd, &readfds);

        // 使用 select 函数监听多个套接字
        activity = select(10, &readfds, NULL, NULL, NULL);

        if ((activity < 0)) {
            printf("select error");
        }

        // 如果有新的连接请求
        if (FD_ISSET(server_fd, &readfds)) {
            int addrlen = sizeof(client_addr);
            if ((client_fd = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t*)&addrlen)) < 0) {
                
            }
            else
            {
                FD_SET(client_fd, &readfds);
                printf("New connection, socket fd is %d, ip is : %s, port : %d\n", client_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            }
        }

        if (FD_ISSET(client_fd, &readfds)) {
                int valread=0;
                if ((valread = read(client_fd, buffer, BUFFER_SIZE)) == 0) {
                    getpeername(client_fd, (struct sockaddr*)&client_addr, (socklen_t*)&addrlen);
                    printf("Host disconnected, ip %s, port %d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    close(client_fd);
                } else {
                    handle(buffer,valread,args);
                    send(client_fd, success, strlen(success), 0);
                }
            }

    }
}


int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <port> <cmd> <param>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    int cmd = atoi(argv[2]);
    char *param = argv[3];

    int sockfd;
    struct sockaddr_in server_addr;

    // 创建socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    // 连接服务器
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // 发送数据
    message msg;
    msg.cmd=cmd;
    strcpy(msg.param,param);
    if (send(sockfd, &msg, sizeof(msg), 0) == -1) {
        perror("send");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // 接收数据
    char buffer[1024];
    int bytes_received = recv(sockfd, buffer, sizeof(buffer), 0);
    if (bytes_received == -1) {
        perror("recv");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    buffer[bytes_received] = '\0';
    printf("Received from server: %s\n", buffer);

    // 关闭socket
    close(sockfd);

    return 0;
}