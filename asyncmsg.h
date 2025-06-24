#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/types.h> 
#include <linux/wait.h>


#define MAX_MSG_LEN 128
#define MAX_QUEUE_SIZE 64
#define RETURN_MESSAGE 512


struct async_msg
{
    char msg[MAX_MSG_LEN];
    size_t len;
    u64 timestamp_ns;
    bool processed;
};

struct asyncmsg_dev {
    struct async_msg queue[MAX_QUEUE_SIZE];
    int head;
    int tail;
    int free_messages;
    int open_count;
    struct cdev cdev;

    struct semaphore sem;
    spinlock_t lock;
    wait_queue_head_t read_q;
    wait_queue_head_t write_q;

    struct timer_list stat_timer;
    struct tasklet_struct msg_tasklet;
    struct workqueue_struct *wq;
    struct work_struct heavy_job;

    struct fasync_struct *async_queue;

    unsigned int write_delay_ms;
};




