#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DEVICE_PATH "/dev/asyncmsg"

#define ASYNC_MSG_IOC_MAGIC 't'
#define ASYNC_MSG_CLEAR_IO _IO(ASYNC_MSG_IOC_MAGIC, 0)
#define ASYNC_MSG_SET_SIZE _IOW(ASYNC_MSG_IOC_MAGIC, 1, int)
#define ASYNC_MSG_GET_SIZE _IOR(ASYNC_MSG_IOC_MAGIC, 2, int)
#define ASYNC_MSG_GET_STAT _IOR(ASYNC_MSG_IOC_MAGIC, 3, int)
#define ASYNC_MSG_IOC_MXMR 3

int main() {
    char buf[100];
    int ret;
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }

    printf("Testing ioctl on %s\n", DEVICE_PATH);

    char stats_buf[512] = {0};
    if (ioctl(fd, ASYNC_MSG_GET_STAT, (void *)stats_buf) == -1) {
        perror("ASYNC_MSG_GET_STAT failed");
    } else {
        printf("ASYNC_MSG_GET_STAT:\n%s\n", stats_buf);
    }


    strcpy(buf, "Привіт, це тест!");
    ret = write(fd, buf, strlen(buf));
    if (ret < 0)
        perror("write");
    else
        printf("Записано %d байтів: %s\n", ret, buf);

    char stats_buf1[512] = {0};
    if (ioctl(fd, ASYNC_MSG_GET_STAT, (void *)stats_buf1) == -1) {
        perror("ASYNC_MSG_GET_STAT failed");
    } else {
        printf("ASYNC_MSG_GET_STAT:\n%s\n", stats_buf1);
    }


    //CLEAR
    if (ioctl(fd, ASYNC_MSG_CLEAR_IO) == -1) {
        perror("ASYNC_MSG_CLEAR_IO failed");
    } else {
        printf("ASYNC_MSG_CLEAR_IO: success\n");
    }

    // SET SIZE
    int new_size = 32;
    if (ioctl(fd, ASYNC_MSG_SET_SIZE, &new_size) == -1) {
        perror("ASYNC_MSG_SET_SIZE failed");
    } else {
        printf("ASYNC_MSG_SET_SIZE: new size set to %d\n", new_size);
    }

    // // GET SIZE
    int current_size = 0;
    if (ioctl(fd, ASYNC_MSG_GET_SIZE, &current_size) == -1) {
        perror("ASYNC_MSG_GET_SIZE failed");
    } else {
        printf("ASYNC_MSG_GET_SIZE: current size is %d\n", current_size);
    }

    // GET STAT
    char stats_buf2[512] = {0};
    if (ioctl(fd, ASYNC_MSG_GET_STAT, (void *)stats_buf2) == -1) {
        perror("ASYNC_MSG_GET_STAT failed");
    } else {
        printf("ASYNC_MSG_GET_STAT:\n%s\n", stats_buf2);
    }

    close(fd);
    return 0;
}
