#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <locale.h>

#define PORT        3490
#define MAX_CLIENTS 100
#define MAXBUF      4096
#define MAXNAME     32
#define MAXROOM     32

/* 클라이언트 상태 관리 구조체 */
typedef struct {
    int fd;                     // 소켓 파일 디스크립터 (-1이면 빈 슬롯)
    char nickname[MAXNAME];     // 닉네임
    char room[MAXROOM];         // 현재 방 이름
    int registered;             // 0: 접속직후, 1: /join 완료

    /* TCP 스트림 처리를 위한 버퍼 */
    char cmd_buf[MAXBUF];       // 명령어를 쌓아두는 버퍼
    int cmd_len;                // 현재 버퍼에 쌓인 데이터 길이

    /* 파일 전송 상태 */
    long file_remain;           // 남은 파일 전송량 (>0 이면 파일 모드)
} ClientContext;

/* 서버 상태 관리 구조체 */
typedef struct {
    int listenfd;
    ClientContext clients[MAX_CLIENTS]; // 클라이언트 배열
    fd_set all_fds;                     // 전체 관찰 대상 fd 셋
    int max_fd;                         // 현재 가장 큰 fd 번호
} ServerContext;

/* 전역 서버 컨텍스트 (main과 signal 핸들러 등에서 접근 가능하도록 할 수 있으나, 여기선 main 루프 내에서 처리) */

/* 클라이언트 슬롯 초기화 */
void init_client(ClientContext *c) {
    c->fd = -1;
    memset(c->nickname, 0, MAXNAME);
    memset(c->room, 0, MAXROOM);
    c->registered = 0;
    memset(c->cmd_buf, 0, MAXBUF);
    c->cmd_len = 0;
    c->file_remain = 0;
}

/* 연결 종료 및 정리 */
void disconnect_client(ServerContext *server, int idx) {
    int fd = server->clients[idx].fd;
    if (fd >= 0) {
        close(fd);
        FD_CLR(fd, &server->all_fds);
        printf("SERVER: Client fd=%d disconnected\n", fd);
    }
    init_client(&server->clients[idx]);
}

/* 같은 방의 다른 클라이언트에게 메시지 전송 (브로드캐스트) */
void broadcast_to_room(ServerContext *server, int sender_idx, const char *data, int len) {
    ClientContext *sender = &server->clients[sender_idx];
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ClientContext *target = &server->clients[i];
        
        if (target->fd == -1) continue; // 빈 슬롯
        if (!target->registered) continue; // 입장 전
        if (strcmp(target->room, sender->room) != 0) continue; // 다른 방
        if (i == sender_idx) continue;
        // 자기 자신 및 다른 사람에게 전송 (send 실패 시 로그만)
        if (send(target->fd, data, len, 0) == -1) {
            perror("send broadcast");
        }
    }
}

/* 명령어 처리 로직 (/join, /msg, /file) */
void process_command(ServerContext *server, int idx, char *line) {
    ClientContext *cli = &server->clients[idx];
    int fd = cli->fd;
    char response[MAXBUF];

    // 1. /join <name> <room>
    if (strncmp(line, "/join", 5) == 0) {
        char name[MAXNAME], room[MAXROOM];
        if (sscanf(line, "/join %31s %31s", name, room) != 2) {
            snprintf(response, sizeof(response), "ERR Usage: /join <name> <room>\n");
            send(fd, response, strlen(response), 0);
            return;
        }
        strncpy(cli->nickname, name, MAXNAME - 1);
        strncpy(cli->room, room, MAXROOM - 1);
        cli->registered = 1;

        printf("SERVER: fd=%d joined. Nick=%s, Room=%s\n", fd, cli->nickname, cli->room);
        snprintf(response, sizeof(response), "OK Joined as %s in room %s\n", cli->nickname, cli->room);
        send(fd, response, strlen(response), 0);
    }
    // 2. /msg <message>
    else if (strncmp(line, "/msg", 4) == 0) {
        if (!cli->registered) {
            send(fd, "ERR Please /join first.\n", 24, 0);
            return;
        }
        char *msg = line + 4;
        while (*msg == ' ') msg++; // 공백 제거

        char packet[MAXBUF];
        snprintf(packet, sizeof(packet), "[%s] %s\n", cli->nickname, msg);
        broadcast_to_room(server, idx, packet, strlen(packet));
    }
    // 3. /file <filename> <size>
    else if (strncmp(line, "/file", 5) == 0) {
        if (!cli->registered) {
            send(fd, "ERR Please /join first.\n", 24, 0);
            return;
        }
        char fname[256];
        long fsize = 0;
        if (sscanf(line, "/file %255s %ld", fname, &fsize) != 2 || fsize <= 0) {
            send(fd, "ERR Usage: /file <name> <size>\n", 31, 0);
            return;
        }

        // 상태 전환: 파일 데이터 수신 모드
        cli->file_remain = fsize;

        // 같은 방 사람들에게 파일 수신 알림 (헤더 전송)
        char header[MAXBUF];
        snprintf(header, sizeof(header), "FILE %s %s %ld\n", cli->nickname, fname, fsize);
        broadcast_to_room(server, idx, header, strlen(header));

        printf("SERVER: fd=%d started file transfer '%s' (%ld bytes)\n", fd, fname, fsize);
    }
    else {
        send(fd, "ERR Unknown command\n", 20, 0);
    }
}

/* 수신된 데이터 처리 (버퍼링 및 파싱) */
void handle_client_data(ServerContext *server, int idx) {
    ClientContext *cli = &server->clients[idx];
    char buf[MAXBUF];
    
    // 1. 데이터 수신
    ssize_t nbytes = recv(cli->fd, buf, sizeof(buf), 0);
    if (nbytes <= 0) {
        disconnect_client(server, idx);
        return;
    }

    // 2. 파일 데이터 모드인 경우: 버퍼링 없이 즉시 브로드캐스트
    if (cli->file_remain > 0) {
        long to_send = (nbytes > cli->file_remain) ? cli->file_remain : nbytes;
        
        // 같은 방 인원에게 바이너리 데이터 전송
        broadcast_to_room(server, idx, buf, to_send);
        
        cli->file_remain -= to_send;
        if (cli->file_remain <= 0) {
            printf("SERVER: fd=%d file transfer complete\n", cli->fd);
        }
        
        // 파일 데이터 뒤에 붙어온 텍스트 명령어가 있다면? (복잡한 경우)
        // 이 예제에서는 파일 데이터와 명령어가 정확히 분리된다고 가정하거나
        // 남은 데이터를 버립니다. 실무에선 링버퍼가 필요합니다.
        return; 
    }

    // 3. 일반 텍스트 모드: 버퍼에 쌓고 개행 문자 단위로 처리
    // 버퍼 오버플로우 방지
    if (cli->cmd_len + nbytes >= MAXBUF) {
        // 버퍼가 꽉 찼다면 비우거나 에러 처리 (여기선 초기화)
        cli->cmd_len = 0; 
    }
    
    memcpy(cli->cmd_buf + cli->cmd_len, buf, nbytes);
    cli->cmd_len += nbytes;
    cli->cmd_buf[cli->cmd_len] = '\0'; // 안전장치

    // 개행 문자(\n)가 있는지 확인하며 처리
    char *p;
    while ((p = strchr(cli->cmd_buf, '\n')) != NULL) {
        *p = '\0'; // 개행을 null로 변경하여 문자열 분리
        
        // 캐리지 리턴(\r) 처리
        if (p > cli->cmd_buf && *(p-1) == '\r') *(p-1) = '\0';

        if (strlen(cli->cmd_buf) > 0) {
            process_command(server, idx, cli->cmd_buf);
        }

        // 처리한 부분 제거하고 버퍼 앞으로 당기기 (memmove)
        int processed_len = (p - cli->cmd_buf) + 1;
        int remaining = cli->cmd_len - processed_len;
        
        memmove(cli->cmd_buf, cli->cmd_buf + processed_len, remaining);
        cli->cmd_len = remaining;
        cli->cmd_buf[cli->cmd_len] = '\0';
    }
}

/* 새 연결 수락 */
void handle_new_connection(ServerContext *server) {
    struct sockaddr_in cli_addr;
    socklen_t addrlen = sizeof(cli_addr);
    int newfd = accept(server->listenfd, (struct sockaddr *)&cli_addr, &addrlen);
    
    if (newfd < 0) {
        perror("accept");
        return;
    }

    // 빈 슬롯 찾기
    int idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i].fd == -1) {
            idx = i;
            break;
        }
    }

    if (idx == -1) {
        printf("SERVER: Too many clients. Rejected.\n");
        close(newfd);
        return;
    }

    // 등록
    init_client(&server->clients[idx]);
    server->clients[idx].fd = newfd;
    
    FD_SET(newfd, &server->all_fds);
    if (newfd > server->max_fd) server->max_fd = newfd;

    printf("SERVER: New connection from %s, assigned fd=%d\n", 
           inet_ntoa(cli_addr.sin_addr), newfd);
}

int main(void) {
    setlocale(LC_ALL, "");
    ServerContext server;
    
    // 초기화
    memset(&server, 0, sizeof(server));
    for (int i = 0; i < MAX_CLIENTS; i++) init_client(&server.clients[i]);
    
    // 소켓 생성
    server.listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (server.listenfd < 0) { perror("socket"); exit(1); }

    int yes = 1;
    setsockopt(server.listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server.listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind"); exit(1);
    }

    if (listen(server.listenfd, 10) < 0) {
        perror("listen"); exit(1);
    }

    FD_ZERO(&server.all_fds);
    FD_SET(server.listenfd, &server.all_fds);
    server.max_fd = server.listenfd;

    printf("SERVER: Running on port %d...\n", PORT);

    while (1) {
        fd_set read_fds = server.all_fds;
        
        if (select(server.max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // 1. 새 연결 확인
        if (FD_ISSET(server.listenfd, &read_fds)) {
            handle_new_connection(&server);
        }

        // 2. 기존 클라이언트 데이터 확인
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = server.clients[i].fd;
            if (fd != -1 && fd != server.listenfd && FD_ISSET(fd, &read_fds)) {
                handle_client_data(&server, i);
            }
        }
    }

    close(server.listenfd);
    return 0;
}