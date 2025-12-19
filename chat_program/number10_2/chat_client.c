// chat_client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>

#define PORT 3490
#define MAXBUF 512
#define MAXMSG 1024

int main(int argc, char *argv[])
{
    int sock;
    struct sockaddr_in serv_addr;
    fd_set readfds;
    char sendbuf[MAXBUF];
    char recvbuf[MAXBUF];

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <SERVER IP> <NICKNAME> <ROOM>\n", argv[0]);
        exit(1);
    }

    const char *server_ip = argv[1];
    const char *nickname  = argv[2];
    const char *room      = argv[3];

    /* 1. 소켓 생성 */
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    /* 2. 서버 주소 설정 */
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(PORT);
    serv_addr.sin_addr.s_addr = inet_addr(server_ip);

    /* 3. 서버 접속 */
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        exit(1);
    }

    printf("Connected to %s:%d  nick=%s room=%s\n",
           server_ip, PORT, nickname, room);
    printf("Type messages and press Enter. '/quit' to exit.\n");

    /* 접속 직후 /join 전송 */
    char joinmsg[MAXMSG];
    snprintf(joinmsg, sizeof(joinmsg), "/join %s %s\n", nickname, room);
    send(sock, joinmsg, strlen(joinmsg), 0);

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock, &readfds);

        int maxfd = (sock > STDIN_FILENO ? sock : STDIN_FILENO);

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select");
            exit(1);
        }

        /* 키보드 입력 */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(sendbuf, sizeof(sendbuf), stdin) == NULL)
                break;

            /* /quit 명령 서버로 전송 */
            if (strncmp(sendbuf, "/quit", 5) == 0) {
                send(sock, "/quit\n", 6, 0);
                break;
            }

            char msg[MAXMSG];
            snprintf(msg, sizeof(msg), "/msg %s", sendbuf);
            send(sock, msg, strlen(msg), 0);
        }

        /* 서버 메시지 수신 */
        if (FD_ISSET(sock, &readfds)) {
            int nbytes = recv(sock, recvbuf, sizeof(recvbuf) - 1, 0);
            if (nbytes <= 0)
                break;

            recvbuf[nbytes] = '\0';
            printf("%s", recvbuf);
        }
    }

    close(sock);
    return 0;
}
