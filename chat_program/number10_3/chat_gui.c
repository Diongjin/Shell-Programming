#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <locale.h>

#define PORT 3490
#define MAXBUF 1024

/* 프로그램의 상태와 위젯을 관리하는 구조체 (Context) */
typedef struct {
    // UI Widgets
    GtkWidget *window;
    GtkWidget *entry_ip;
    GtkWidget *entry_nick;
    GtkWidget *entry_room;
    GtkWidget *textview_chat;
    GtkWidget *entry_msg;
    GtkWidget *btn_connect;
    GtkWidget *btn_send;

    // Network State
    int sockfd;
    GIOChannel *sock_channel;
    guint io_watch_id; // 소켓 감시 ID
} ChatApp;

/* 채팅창에 텍스트 추가 (Helper 함수) */
static void append_chat_text(ChatApp *app, const char *msg)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->textview_chat));
    GtkTextIter end_iter;
    
    gtk_text_buffer_get_end_iter(buffer, &end_iter);
    gtk_text_buffer_insert(buffer, &end_iter, msg, -1);
    gtk_text_buffer_insert(buffer, &end_iter, "\n", -1);

    // 자동 스크롤
    GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &end_iter, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(app->textview_chat), mark, 0.0, TRUE, 0.0, 1.0);
    gtk_text_buffer_delete_mark(buffer, mark);
}

/* 연결 종료 및 리소스 정리 */
static void disconnect_server(ChatApp *app)
{
    if (app->io_watch_id > 0) {
        g_source_remove(app->io_watch_id);
        app->io_watch_id = 0;
    }
    
    if (app->sock_channel) {
        g_io_channel_shutdown(app->sock_channel, FALSE, NULL);
        g_io_channel_unref(app->sock_channel);
        app->sock_channel = NULL;
    }

    if (app->sockfd >= 0) {
        close(app->sockfd);
        app->sockfd = -1;
    }

    // UI 상태 변경
    gtk_widget_set_sensitive(app->btn_connect, TRUE);
    gtk_widget_set_sensitive(app->btn_send, FALSE);
    append_chat_text(app, "** Disconnected **");
}

/* 소켓 데이터 수신 콜백 */
static gboolean socket_io_cb(GIOChannel *source, GIOCondition condition, gpointer user_data)
{
    ChatApp *app = (ChatApp *)user_data;

    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        disconnect_server(app);
        return FALSE; // 이벤트 소스 제거
    }

    char buf[MAXBUF + 1];
    ssize_t n = recv(app->sockfd, buf, MAXBUF, 0);

    if (n <= 0) {
        append_chat_text(app, "** Server closed connection **");
        disconnect_server(app);
        return FALSE;
    }

    buf[n] = '\0';

    // 받은 데이터 처리 (단순 출력)
    // 실제 구현시에는 TCP 패킷 뭉침 현상 처리를 위해 별도 버퍼링이 필요할 수 있음
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        append_chat_text(app, p);
        if (!nl) break;
        p = nl + 1;
    }

    return TRUE; // 계속 감시
}

/* 전송 버튼 핸들러 */
static void on_send_clicked(GtkWidget *widget, gpointer user_data)
{
    ChatApp *app = (ChatApp *)user_data;
    
    if (app->sockfd < 0) return;

    const char *text = gtk_entry_get_text(GTK_ENTRY(app->entry_msg));
    if (!text || *text == '\0') return;

    char buf[MAXBUF + 64];
    // 메시지 프로토콜 포맷팅
    snprintf(buf, sizeof(buf), "/msg %s\n", text);

    if (send(app->sockfd, buf, strlen(buf), 0) == -1) {
        append_chat_text(app, "** Send error **");
        disconnect_server(app); // 에러 발생 시 연결 종료 처리
        return;
    }

    gtk_entry_set_text(GTK_ENTRY(app->entry_msg), "");
}

/* 접속 버튼 핸들러 */
static void on_connect_clicked(GtkWidget *widget, gpointer user_data)
{
    ChatApp *app = (ChatApp *)user_data;

    const char *ip   = gtk_entry_get_text(GTK_ENTRY(app->entry_ip));
    const char *nick = gtk_entry_get_text(GTK_ENTRY(app->entry_nick));
    const char *room = gtk_entry_get_text(GTK_ENTRY(app->entry_room));

    if (strlen(ip) == 0 || strlen(nick) == 0 || strlen(room) == 0) {
        append_chat_text(app, "** IP, Nickname, Room을 모두 입력하세요 **");
        return;
    }

    if (app->sockfd >= 0) {
        append_chat_text(app, "** 이미 연결되어 있습니다 **");
        return;
    }

    app->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (app->sockfd < 0) {
        append_chat_text(app, "** Socket creation failed **");
        return;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(PORT);
    
    // inet_pton이 inet_addr보다 안전하고 최신 표준임
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        append_chat_text(app, "** Invalid IP Address **");
        close(app->sockfd);
        app->sockfd = -1;
        return;
    }

    // Connect (Note: GUI 스레드에서 블로킹 함수 사용은 권장되지 않으나 간단한 예제를 위해 유지)
    if (connect(app->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        append_chat_text(app, "** Connect failed **");
        close(app->sockfd);
        app->sockfd = -1;
        return;
    }

    // Join 메시지 전송
    char joinmsg[MAXBUF];
    snprintf(joinmsg, sizeof(joinmsg), "/join %s %s\n", nick, room);
    if (send(app->sockfd, joinmsg, strlen(joinmsg), 0) == -1) {
        append_chat_text(app, "** Join request failed **");
        close(app->sockfd);
        app->sockfd = -1;
        return;
    }

    append_chat_text(app, "** Connected **");

    // GIOChannel 설정
    app->sock_channel = g_io_channel_unix_new(app->sockfd);
    g_io_channel_set_encoding(app->sock_channel, NULL, NULL);
    g_io_channel_set_flags(app->sock_channel, G_IO_FLAG_NONBLOCK, NULL);

    // Watch 등록 및 ID 저장
    app->io_watch_id = g_io_add_watch(app->sock_channel,
                                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                      socket_io_cb, app); // user_data로 app 전달

    gtk_widget_set_sensitive(app->btn_connect, FALSE);
    gtk_widget_set_sensitive(app->btn_send, TRUE);
}

/* 윈도우 종료 시 메모리 해제 */
static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    ChatApp *app = (ChatApp *)user_data;
    disconnect_server(app);
    free(app); // 할당된 구조체 메모리 해제
    gtk_main_quit();
}

/* UI 초기화 함수 */
static void init_ui(ChatApp *app)
{
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "GTK Chat Client (Refactored)");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 450, 350);
    
    // user_data로 app 포인터 전달
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_destroy), app);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 10);

    // 상단 입력 영역
    GtkWidget *hbox_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_top, FALSE, FALSE, 0);

    app->entry_ip = gtk_entry_new();
    app->entry_nick = gtk_entry_new();
    app->entry_room = gtk_entry_new();

    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_ip), "IP (127.0.0.1)");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_nick), "Nickname");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_room), "Room");
    
    // 기본값 설정 (테스트 편의성)
    gtk_entry_set_text(GTK_ENTRY(app->entry_ip), "127.0.0.1");

    gtk_box_pack_start(GTK_BOX(hbox_top), app->entry_ip, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_top), app->entry_nick, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_top), app->entry_room, TRUE, TRUE, 0);

    app->btn_connect = gtk_button_new_with_label("Connect");
    gtk_box_pack_start(GTK_BOX(hbox_top), app->btn_connect, FALSE, FALSE, 0);
    g_signal_connect(app->btn_connect, "clicked", G_CALLBACK(on_connect_clicked), app);

    // 채팅창 영역
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scrolled, TRUE); // 세로 확장
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 5);

    app->textview_chat = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->textview_chat), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->textview_chat), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->textview_chat), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled), app->textview_chat);

    // 하단 전송 영역
    GtkWidget *hbox_bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_bottom, FALSE, FALSE, 0);

    app->entry_msg = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox_bottom), app->entry_msg, TRUE, TRUE, 0);
    g_signal_connect(app->entry_msg, "activate", G_CALLBACK(on_send_clicked), app);

    app->btn_send = gtk_button_new_with_label("Send");
    gtk_widget_set_sensitive(app->btn_send, FALSE);
    gtk_box_pack_start(GTK_BOX(hbox_bottom), app->btn_send, FALSE, FALSE, 0);
    g_signal_connect(app->btn_send, "clicked", G_CALLBACK(on_send_clicked), app);
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    gtk_init(&argc, &argv);

    // 구조체 메모리 할당 및 초기화
    ChatApp *app = (ChatApp *)malloc(sizeof(ChatApp));
    if (!app) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }
    memset(app, 0, sizeof(ChatApp));
    app->sockfd = -1; // 초기 소켓 값 설정

    init_ui(app);
    gtk_widget_show_all(app->window);

    gtk_main();

    // (on_destroy에서 free(app)을 호출하므로 여기서는 별도 해제 불필요)
    return 0;
}