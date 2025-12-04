= 0;
            // sscanf 안전하게 사용 (buffer size 제한)
            if (sscanf(p, "FILE %63s %255s %ld", sender, fname, &size) == 3 && size > 0) {
                char msg[512];
                snprintf(msg, sizeof(msg), "** 파일 수신 시작: %s (%ld bytes) from %s **", fname, size, sender);
                append_chat_text(app, msg);

                snprintf(app->recv_filename, sizeof(app->recv_filename), "recv_%s", fname);
                app->recv_fp = fopen(app->recv_filename, "wb");
                
                if (app->recv_fp) {
                    app->receiving_file = TRUE;
                    app->recv_file_remaining = size;
                } else {
                    append_chat_text(app, "** 파일 생성 실패 **");
                }
            } else {
                append_chat_text(app, p); // 파싱 실패 시 그냥 텍스트로 출력
            }
        } else {
            if (strlen(p) > 0) append_chat_text(app, p);
        }

        if (!nl) break;
        p = nl + 1;
    }

    return TRUE;
}

/* 메시지 전송 처리 */
static void on_send_clicked(GtkWidget *widget, gpointer data)
{
    ChatApp *app = (ChatApp *)data;
    if (app->sockfd < 0) return;

    const char *text = gtk_entry_get_text(GTK_ENTRY(app->entry_msg));
    if (!text || strlen(text) == 0) return;

    char buf[MAXBUF];
    snprintf(buf, sizeof(buf), "/msg %s\n", text);

    if (send(app->sockfd, buf, strlen(buf), 0) < 0) {
        append_chat_text(app, "** 전송 실패 **");
        disconnect_from_server(app);
        return;
    }

    gtk_entry_set_text(GTK_ENTRY(app->entry_msg), "");
}

/* 파일 전송 처리 */
static void on_file_clicked(GtkWidget *widget, gpointer data)
{
    ChatApp *app = (ChatApp *)data;
    if (app->sockfd < 0) return;

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "파일 선택", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_취소", GTK_RESPONSE_CANCEL,
        "_열기", GTK_RESPONSE_ACCEPT, NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filepath) {
            FILE *fp = fopen(filepath, "rb");
            if (!fp) {
                append_chat_text(app, "** 파일을 열 수 없습니다 **");
                g_free(filepath);
                gtk_widget_destroy(dialog);
                return;
            }

            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            char *basename = g_path_get_basename(filepath);
            char header[MAXBUF];
            snprintf(header, sizeof(header), "/file %s %ld\n", basename, size);

            if (send(app->sockfd, header, strlen(header), 0) < 0) {
                append_chat_text(app, "** 헤더 전송 실패 **");
                fclose(fp);
                g_free(basename);
                g_free(filepath);
                gtk_widget_destroy(dialog);
                return;
            }

            /* 주의: 큰 파일 전송 시 UI가 멈출 수 있음. 
               실제 상용 코드에서는 별도 스레드나 g_idle_add를 사용해야 함. */
            char buf[MAXBUF];
            size_t nread;
            while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
                if (send(app->sockfd, buf, nread, 0) < 0) {
                    append_chat_text(app, "** 데이터 전송 중 오류 **");
                    break;
                }
            }

            char msg[512];
            snprintf(msg, sizeof(msg), "** 파일 전송 완료: %s **", basename);
            append_chat_text(app, msg);

            fclose(fp);
            g_free(basename);
            g_free(filepath);
        }
    }
    gtk_widget_destroy(dialog);
}

/* 서버 연결 */
static void on_connect_clicked(GtkWidget *widget, gpointer data)
{
    ChatApp *app = (ChatApp *)data;

    const char *ip   = gtk_entry_get_text(GTK_ENTRY(app->entry_ip));
    const char *nick = gtk_entry_get_text(GTK_ENTRY(app->entry_nick));
    const char *room = gtk_entry_get_text(GTK_ENTRY(app->entry_room));

    if (strlen(ip) == 0 || strlen(nick) == 0 || strlen(room) == 0) {
        append_chat_text(app, "** 모든 정보를 입력하세요 **");
        return;
    }

    app->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (app->sockfd < 0) {
        append_chat_text(app, "** 소켓 생성 실패 **");
        return;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        append_chat_text(app, "** 유효하지 않은 IP 주소 **");
        close(app->sockfd);
        app->sockfd = -1;
        return;
    }

    if (connect(app->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        append_chat_text(app, "** 접속 실패 **");
        close(app->sockfd);
        app->sockfd = -1;
        return;
    }

    // Join 메시지
    char joinmsg[MAXBUF];
    snprintf(joinmsg, sizeof(joinmsg), "/join %s %s\n", nick, room);
    send(app->sockfd, joinmsg, strlen(joinmsg), 0);

    append_chat_text(app, "** 서버 접속 완료 **");

    // GIOChannel 설정 (비동기 수신)
    app->sock_channel = g_io_channel_unix_new(app->sockfd);
    g_io_channel_set_encoding(app->sock_channel, NULL, NULL); // Binary safe
    g_io_channel_set_flags(app->sock_channel, G_IO_FLAG_NONBLOCK, NULL);

    app->io_watch_id = g_io_add_watch(app->sock_channel, 
                                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                      socket_io_cb, app);

    gtk_widget_set_sensitive(app->btn_connect, FALSE);
    gtk_widget_set_sensitive(app->btn_send, TRUE);
    gtk_widget_set_sensitive(app->btn_file, TRUE);
}

/* 윈도우 닫기 이벤트 */
static void on_destroy(GtkWidget *widget, gpointer data)
{
    ChatApp *app = (ChatApp *)data;
    disconnect_from_server(app);
    free(app); // 구조체 메모리 해제
    gtk_main_quit();
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    gtk_init(&argc, &argv);

    // 앱 컨텍스트 초기화
    ChatApp *app = (ChatApp *)malloc(sizeof(ChatApp));
    memset(app, 0, sizeof(ChatApp));
    app->sockfd = -1;

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "GTK Chat Client Refactored");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 500, 400);
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
    
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_ip), "IP Address");
    gtk_entry_set_text(GTK_ENTRY(app->entry_ip), "127.0.0.1"); // 기본값 편의
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_nick), "Nickname");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_room), "Room");

    gtk_box_pack_start(GTK_BOX(hbox_top), app->entry_ip, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_top), app->entry_nick, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_top), app->entry_room, TRUE, TRUE, 0);

    app->btn_connect = gtk_button_new_with_label("Connect");
    gtk_box_pack_start(GTK_BOX(hbox_top), app->btn_connect, FALSE, FALSE, 0);
    g_signal_connect(app->btn_connect, "clicked", G_CALLBACK(on_connect_clicked), app);

    // 채팅창 (스크롤 포함)
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 5);
    
    app->textview_chat = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->textview_chat), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->textview_chat), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->textview_chat), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled), app->textview_chat);

    // 하단 메시지 전송 영역
    GtkWidget *hbox_bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_bottom, FALSE, FALSE, 0);

    app->entry_msg = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox_bottom), app->entry_msg, TRUE, TRUE, 0);
    g_signal_connect(app->entry_msg, "activate", G_CALLBACK(on_send_clicked), app);

    app->btn_send = gtk_button_new_with_label("Send");
    gtk_widget_set_sensitive(app->btn_send, FALSE);
    gtk_box_pack_start(GTK_BOX(hbox_bottom), app->btn_send, FALSE, FALSE, 0);
    g_signal_connect(app->btn_send, "clicked", G_CALLBACK(on_send_clicked), app);

    app->btn_file = gtk_button_new_with_label("File");
    gtk_widget_set_sensitive(app->btn_file, FALSE);
    gtk_box_pack_start(GTK_BOX(hbox_bottom), app->btn_file, FALSE, FALSE, 0);
    g_signal_connect(app->btn_file, "clicked", G_CALLBACK(on_file_clicked), app);

    gtk_widget_show_all(app->window);
    gtk_main();

    return 0;
}[test1] here
[test2] thx
