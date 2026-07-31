#include "log.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

static int s_sock = -1;
static struct sockaddr_in s_dst;
static FILE *s_file;
static int s_udp_ok;

void log_init_file(const char *path) {
    if (!path || !path[0])
        return;
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    s_file = fopen(path, "a");
    if (s_file) {
        setvbuf(s_file, NULL, _IONBF, 0);
        fprintf(s_file, "\n----- log start -----\n");
    }
}

void log_close_file(void) {
    if (!s_file)
        return;
    fprintf(s_file, "----- log end -----\n");
    fclose(s_file);
    s_file = NULL;
}

void log_set_file_enabled(int enable, const char *path) {
    if (enable) {
        if (!s_file)
            log_init_file(path && path[0] ? path : "/data/moonlight/debug.log");
    } else {
        log_close_file();
    }
}

void log_init(const char *udp_host, unsigned short udp_port) {
    s_udp_ok = 0;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }

    if (!udp_host || !udp_host[0]) {
        LOGW("log: no debug_host; no UDP");
        return;
    }

    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port = htons(udp_port);
    if (inet_pton(AF_INET, udp_host, &s_dst.sin_addr) != 1) {
        LOGE("log: invalid debug_host: %s", udp_host);
        return;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        LOGE("log: socket UDP errno=%d", errno);
        return;
    }
    s_udp_ok = 1;
    LOGI("log: UDP %s:%u ok; file=%s", udp_host, udp_port,
         s_file ? "si" : "no");
}

void log_shutdown(void) {
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    log_close_file();
    s_udp_ok = 0;
}

void log_vprintf(const char *fmt, va_list ap) {
    char buf[1536];
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap2);
    if (len < 0)
        return;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;

    if (s_file) {
        fwrite(buf, 1, (size_t)len, s_file);
        if (len == 0 || buf[len - 1] != '\n')
            fputc('\n', s_file);
    }

    if (s_sock >= 0)
        sendto(s_sock, buf, (size_t)len, 0, (struct sockaddr *)&s_dst, sizeof(s_dst));
}

void log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
}

void log_notify(const char *fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

#ifdef __ORBIS__
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    strncpy(req.message, text, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
    log_printf("[N] %s\n", text);
}
