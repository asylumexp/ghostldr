/*
 * echo: a TCP echo server payload for the ELF loader (docs/elfldr.md).
 *
 * Inside SWRR on a PS5, socket() for the internet fails (ENOSYS); the ELF
 * loader gives payloads sockets made by the emulator's JIT compiler
 * process instead, and bind, listen, accept, recv and send then work as
 * usual.  This one listens on TCP 9025, serves one client at a time and
 * sends every line back with "echo: " in front.  A client that sends
 * "quit" ends it; so do 60 seconds without a client.  What it does goes to
 * its stdout, which comes back on the connection it was sent on:
 *
 *     nc PS5 9021 < build/elfldr/echo.elf      # in one terminal
 *     nc PS5 9025                              # in another: type lines, then quit
 *
 * MIT license, like the rest of ghostldr.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 9025
#define IDLE_MS 60000

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0)
            return -1;
        p += w, n -= (size_t)w;
    }
    return 0;
}

/* one client: 1 when it asked to quit */
static int serve(int c)
{
    char buf[512], line[600];
    size_t have = 0;
    send_all(c, "echo: hello, send lines; \"quit\" stops the server\n", 49);
    for (;;) {
        ssize_t r = recv(c, buf + have, sizeof(buf) - 1 - have, 0);
        if (r <= 0)
            return 0;
        have += (size_t)r;
        char *nl;
        while ((nl = memchr(buf, '\n', have))) {
            size_t len = (size_t)(nl - buf);
            if (len && buf[len - 1] == '\r')
                len--;
            buf[len] = 0;
            if (strcmp(buf, "quit") == 0) {
                send_all(c, "echo: bye\n", 10);
                return 1;
            }
            int n = snprintf(line, sizeof(line), "echo: %s\n", buf);
            printf("echo: %zu bytes from the client: %s\n", len, buf);
            if (send_all(c, line, (size_t)n) != 0)
                return 0;
            have -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, have);
        }
        if (have == sizeof(buf) - 1)
            have = 0; /* a line longer than the buffer: dropped */
    }
}

int main(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("echo: socket() failed: errno %d\n", errno);
        return 1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        printf("echo: bind/listen on TCP %d failed: errno %d\n", PORT, errno);
        close(s);
        return 2;
    }
    printf("echo: socket %d listening on TCP %d\n", s, PORT);
    int clients = 0, quit = 0;
    while (!quit) {
        struct pollfd p = {s, POLLIN, 0};
        int n = poll(&p, 1, IDLE_MS);
        if (n == 0) {
            printf("echo: no client for %d s, stopping\n", IDLE_MS / 1000);
            break;
        }
        if (n < 0)
            continue;
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int c = accept(s, (struct sockaddr *)&from, &len);
        if (c < 0) {
            printf("echo: accept failed: errno %d\n", errno);
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        printf("echo: client %d from %s:%d\n", ++clients, ip, ntohs(from.sin_port));
        quit = serve(c);
        close(c);
        printf("echo: client %d gone\n", clients);
    }
    close(s);
    printf("echo: served %d client%s\n", clients, clients == 1 ? "" : "s");
    return 0;
}
