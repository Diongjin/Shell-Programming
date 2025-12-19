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

#define PORT 3490
#define QLEN 10
#define MAXBUF 512
#define MAXNAME 32
#define MAXROOM 32

static int  registered[FD_SETSIZE];
static char nicknames[FD_SETSIZE][MAXNAME];
static char rooms[FD_SETSIZE][MAXROOM];

void init_clients() {
    for (int i = 0; i < FD_SETSIZE; i++) {
        registered[i] = 0;
        nicknames[i][0] = '\0';
        rooms[i][0] = '\0';
    }
}

void handle_line(int fd, fd_set *activefds, char *line) {
    char buf[MAXBUF];

    /* 줄 끝 개행 제거 */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
        line[--len] = '\0';
    if (len == 0) return;

    /* /join <name> <room> */
    if (strncmp(line, "/join", 5) == 0) {
        char name[MAXNAME], room[MAXROOM];

        if (sscanf(line, "/join %31s %31s", name, room) != 2) {
            const char *err = "ERR Usage: /join <name> <room>\n";
            send(fd, err, strlen(err), 0);
            return;
        }

        strncpy(nicknames[fd], name, MAXNAME-1);
        strncpy(rooms[fd], room, MAXROOM-1);
        registered[fd] = 1;

        snprintf(buf, sizeof(buf),
                 "OK Joined as %s in room %s\n", name, room);
        send(fd, buf, strlen(buf), 0);

        printf("SERVER: fd=%d joined name=%s room=%s\n", fd, name, room);
    }

    /* /msg <message> */
    else if (strncmp(line, "/msg", 4) == 0) {
        if (!registered[fd]) {
            const char *err = "ERR Please /join first.\n";
            send(fd, err, strlen(err), 0);
            return;
        }

        char *msg = line + 4;
        while (*msg == ' ') msg++;
        if (*msg == '\0') return;

        char out[MAXBUF];
        snprintf(out, sizeof(out),
                 "[room %s][%s] %s\n",
                 rooms[fd], nicknames[fd], msg);

        for (int j = 0; j < FD_SETSIZE; j++) {
            if (FD_ISSET(j, activefds) && registered[j] &&
                strcmp(rooms[j], rooms[fd]) == 0) {
                send(j, out, strlen(out), 0);
            }
        }
    }

    /* /quit 처리 */
    else if (strncmp(line, "/quit", 5) == 0) {
        if (registered[fd]) {
            char out[MAXBUF];
            snprintf(out, sizeof(out),
                     "NOTICE %s left room %s\n",
                     nicknames[fd], rooms[fd]);

            for (int j = 0; j < FD_SETSIZE; j++) {
                if (FD_ISSET(j, activefds) && registered[j] &&
                    strcmp(rooms[j], rooms[fd]) == 0) {
                    send(j, out, strlen(out), 0);
                }
            }
        }
        close(fd);
        FD_CLR(fd, activefds);
        registered[fd] = 0;
        nicknames[fd][0] = '\0';
        rooms[fd][0] = '\0';
    }

    else {
        const char *err = "ERR Unknown command.\n";
        send(fd, err, strlen(err), 0);
    }
}

int main(void) {
    setlocale(LC_ALL, "");

    int listenfd, newfd;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t addrlen;
    fd_set activefds, readfds;
    int maxfd, i, nbytes;
    char buf[MAXBUF];

    init_clients();

    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    int yes = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    listen(listenfd, QLEN);

    printf("SERVER: listening on port %d ...\n", PORT);

    FD_ZERO(&activefds);
    FD_SET(listenfd, &activefds);
    maxfd = listenfd;

    while (1) {
        readfds = activefds;

        select(maxfd + 1, &readfds, NULL, NULL, NULL);

        for (i = 0; i <= maxfd; i++) {
            if (!FD_ISSET(i, &readfds)) continue;

            if (i == listenfd) {
                addrlen = sizeof(cli_addr);
                newfd = accept(listenfd,
                               (struct sockaddr *)&cli_addr, &addrlen);

                FD_SET(newfd, &activefds);
                if (newfd > maxfd) maxfd = newfd;

                printf("SERVER: new client %s, fd=%d\n",
                       inet_ntoa(cli_addr.sin_addr), newfd);
            }
            else {
                nbytes = recv(i, buf, sizeof(buf) - 1, 0);
                if (nbytes <= 0) {
                    close(i);
                    FD_CLR(i, &activefds);
                    registered[i] = 0;
                } else {
                    buf[nbytes] = '\0';
                    handle_line(i, &activefds, buf);
                }
            }
        }
    }

    close(listenfd);
    return 0;
}
