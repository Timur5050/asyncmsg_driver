#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#define DEVICE_PATH "/dev/asyncmsg"

void sigio_handler(int sig, siginfo_t *info, void *context) {
    printf("[SIGIO] Received signal from fd=%d, si_code=%d\n", info->si_fd, info->si_code);

    if (info->si_code == POLL_IN)
        printf("[SIGIO] Data is available to read\n");
    else
        printf("[SIGIO] Other signal code: %d\n", info->si_code);
}

int main() {
    int fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }

    // Установлюємо обробник SIGIO
    struct sigaction sa;
    sa.sa_sigaction = sigio_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGIO, &sa, NULL) == -1) {
        perror("sigaction");
        close(fd);
        return 1;
    }

    // Налаштування SIGIO
    if (fcntl(fd, F_SETOWN, getpid()) == -1) {
        perror("F_SETOWN");
        close(fd);
        return 1;
    }

    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_ASYNC) == -1) {
        perror("F_SETFL O_ASYNC");
        close(fd);
        return 1;
    }

    printf("Listening for events on %s (poll + SIGIO)...\n", DEVICE_PATH);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;

    char buf[1024];
    while (1) {
        int ret = poll(&pfd, 1, 5000); // 10 сек

        if (ret == -1) {
            perror("poll");
            break;
        } else if (ret == 0) {
            printf("[POLL] Timeout — no data\n");
        } else {
            if (pfd.revents & POLLIN & POLLOUT)
            {
                printf("[POLL]: ready to read\n");
            }
            else if (pfd.revents & POLLIN) {
                printf("[POLL]: ready to read\n");
            }
            else if (pfd.revents & POLLOUT) {
            }
                printf("[POLL]: ready to write\n");
            }
        sleep(5);
    }

    close(fd);
    return 0;
}
