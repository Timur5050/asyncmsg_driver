#include "asyncmsg.h"


static struct asyncmsg_dev asyncmsg_dev;
static dev_t asyncmsg_devno;
static struct class *asyncmsg_class;
static int asyncmsg_major = 0;

static void asyncmsg_timer_fn(struct timer_list *t);
static void asyncmsg_tasklet_fn(unsigned long arg);

static void asyncmsg_contructor(void *ptr)
{
    memset(ptr, 0, sizeof(struct async_msg));
    printk(KERN_INFO "asyncmsg: initializing slab object\n");
}

static int culc_free_space(struct asyncmsg_dev *dev)
{
    return (dev->tail < asyncmsg_dev.max_queue_size) ? (asyncmsg_dev.max_queue_size - dev->tail) : 0;
}

static int asyncmsg_fasync(int fd, struct file *file, int on)
{
    struct asyncmsg_dev *dev = file->private_data;
    return fasync_helper(fd, file, on, &dev->fasync_queue);
}

static int asyncmsg_open(struct inode *inode, struct file *filp)
{
    filp->private_data = &asyncmsg_dev;
    unsigned long flags;

    spin_lock_irqsave(&asyncmsg_dev.lock, flags);
    asyncmsg_dev.open_count++;
    printk(KERN_INFO "asyncmsg: opened device, major=%d, minor=%d\n",
        MAJOR(inode->i_rdev), MINOR(inode->i_rdev));
    spin_unlock_irqrestore(&asyncmsg_dev.lock, flags);
    return 0;
}

static int asyncmsg_release(struct inode *inode, struct file *file)
{
    unsigned long flags;

    spin_lock_irqsave(&asyncmsg_dev.lock, flags);
    asyncmsg_dev.open_count--;
    printk(KERN_INFO "asyncmsg: released device, major=%d, minor=%d\n",
            MAJOR(inode->i_rdev), MINOR(inode->i_rdev));
    spin_unlock_irqrestore(&asyncmsg_dev.lock, flags);  
    return 0;
}

static ssize_t asyncmsg_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    char tmp[RETURN_MESSAGE];
    struct asyncmsg_dev *dev = file->private_data;
    int len;

    if (*ppos > 0)
    {
        return 0;
    }

    int ret = wait_event_interruptible_timeout(dev->read_q, dev->free_messages > 0, msecs_to_jiffies(15000));
    if(ret == 0)
    {
        return 0;
    }
    else if(ret < 0)
    {
        return -EFAULT;
    }

    if(down_interruptible(&dev->sem))
    {
        return -ERESTARTSYS;
    }

    if (dev->head >= dev->tail)
    {
        up(&dev->sem);
        return 0;
    }   

    struct async_msg *curr_msg = dev->queue[dev->head];
    curr_msg->processed = true;

    len = snprintf(tmp, sizeof(tmp),
        "message: %.*s\nlen: %ld\ntimestamp_ns: %lld\nprocessed: %d\n",
        (int)curr_msg->len, curr_msg->msg,
        curr_msg->len,
        curr_msg->timestamp_ns,
        curr_msg->processed);

    if (copy_to_user(buf, tmp, len))
    {
        up(&dev->sem);
        return -EFAULT;
    }

    dev->head++;
    dev->free_messages--;
    *ppos += len;
    up(&dev->sem);
    wake_up(&dev->write_q);
    
    return len;
}

static ssize_t asyncmsg_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char tmp[MAX_MSG_LEN];
    struct asyncmsg_dev *dev = file->private_data;
    unsigned long curr_jiffies = jiffies;
    unsigned long new_interval;

    if (dev->tail >= asyncmsg_dev.max_queue_size)
    {
        return -ENOSPC;
    }

    int ret = wait_event_interruptible_timeout(dev->write_q, culc_free_space(dev) > 0, msecs_to_jiffies(15000));
    if(ret == 0)
    {
        return 0;
    }
    else if(ret < 0)
    {
        return -EFAULT;
    }

    if(down_interruptible(&dev->sem))
    {
        return -ERESTARTSYS;
    }

    if(dev->last_jiffies && time_before(curr_jiffies, dev->last_jiffies + msecs_to_jiffies(dev->write_delay_ms)))
    {
        printk(KERN_INFO "asyncmsg: not so fast. we have delay : %d ms, between writes\n", dev->write_delay_ms);
        up(&dev->sem);
        return -EAGAIN;
    }

    count = min((size_t)MAX_MSG_LEN, count);
    if(copy_from_user(tmp, buf, count))
    {
        up(&dev->sem);
        return -EFAULT;
    }   

    tmp[count] = '\0';

    if(sscanf(tmp, "interval=%lu", &new_interval) == 1)
    {
        dev->write_delay_ms = new_interval;
        dev->last_jiffies = curr_jiffies;
        printk(KERN_INFO "asyncmsg: set interval to %lu ms \n", new_interval);
        up(&dev->sem);
        return count;
    }

    struct async_msg *new_mess = mempool_alloc(dev->asyncmsg_mempool, GFP_KERNEL);

    memcpy(new_mess->msg, tmp, count);
    new_mess->timestamp_ns = ktime_get_ns();
    new_mess->len = count;
    new_mess->processed = false;

    dev->queue[dev->tail] = new_mess;
    dev->tail++;
    dev->free_messages++;
    dev->last_jiffies = curr_jiffies;

    if(dev->fasync_queue)
    {
        kill_fasync(&dev->fasync_queue, SIGIO, POLL_IN);
    }

    wake_up(&dev->read_q);
    up(&dev->sem);
    tasklet_schedule(&dev->msg_tasklet);

    return count;
}

static long asyncmsg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int err = 0;
    int tmp;
    struct asyncmsg_dev *dev = file->private_data;

    if(_IOC_TYPE(cmd) != ASYNC_MSG_IOC_MAGIC)
    {
        return -EINVAL;
    }
    if(_IOC_NR(cmd) > ASYNC_MSG_IOC_MXMR)
    {
        return -EINVAL;
    }

    if(_IOC_DIR(cmd) & _IOC_READ)
    {
        err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
    }
    else if(_IOC_DIR(cmd) & _IOC_WRITE)
    {
        err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
    }
    if(err)
    {
        return -EFAULT;
    }


    switch(cmd)
    {
    case ASYNC_MSG_CLEAR_IO:
        dev->head = 0;
        dev->free_messages = 0;

        for(int i = 0; i < dev->tail; i++)
        {
            mempool_free(dev->queue[i], dev->asyncmsg_mempool);
            dev->queue[i] = NULL;
        }
        dev->tail = 0;
        break;
    case ASYNC_MSG_SET_SIZE:
        if(copy_from_user(&tmp, (int __user *)arg, sizeof(int)))
        {
            return -EFAULT;
        }
        if(tmp <= 0 || tmp >= 4096)
        {
            return -EINVAL;
        }
        dev->max_queue_size = tmp;
        printk(KERN_INFO "asyncmsg: changed max size of queue for : %d\n", dev->max_queue_size);
        break;
    case ASYNC_MSG_GET_SIZE:
        tmp = dev->max_queue_size;
        if (copy_to_user((int __user *)arg, &tmp, sizeof(int)))
            return -EFAULT;
        printk(KERN_INFO "asyncmsg: returned max buffer size : %d\n", dev->max_queue_size);
        break;
    case ASYNC_MSG_GET_STAT:
        char tmp[RETURN_MESSAGE];   
        int len;
        unsigned long flags;

        spin_lock_irqsave(&dev->lock, flags);
        len = snprintf(tmp, sizeof(tmp),
                    "asyncmsg: "
                    "head=%d tail=%d free=%d open=%d delay_ms=%d max size=%d\n",
                    dev->head, 
                    dev->tail,
                    dev->free_messages,
                    dev->open_count,
                    dev->write_delay_ms,
                    dev->max_queue_size);
        spin_unlock_irqrestore(&dev->lock, flags);

        if(copy_to_user((char __user*)arg, tmp, len))
        {
            return -EFAULT;
        }
        break;
    }
    return 0;

}

static __poll_t asyncmsg_poll(struct file *file, struct poll_table_struct *wait)
{
    struct asyncmsg_dev *dev = file->private_data;
    __poll_t mask = 0;

    poll_wait(file, &dev->read_q, wait);
    poll_wait(file, &dev->write_q, wait);

    if(dev->free_messages > 0)
    {
        mask |= POLLIN | POLLRDNORM;
    }
    if(culc_free_space(dev) > 0)
    {
        mask |= POLLOUT | POLLWRNORM;
    }

    return mask;
}

static void asyncmsg_timer_fn(struct timer_list *t)
{
    struct asyncmsg_dev *dev = from_timer(dev, t, stat_timer);
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    pr_info_ratelimited("asyncmsg: "
                    "head=%d tail=%d free=%d open=%d delay_ms=%d max size=%d\n",
                    dev->head, 
                    dev->tail,
                    dev->free_messages,
                    dev->open_count,
                    dev->write_delay_ms,
                    dev->max_queue_size);
    spin_unlock_irqrestore(&dev->lock, flags);

    mod_timer(&dev->stat_timer, jiffies + msecs_to_jiffies(600000));
}

static void asyncmsg_tasklet_fn(unsigned long arg)
{
    struct asyncmsg_dev *dev = (struct asyncmsg_dev *)arg;
    int letter_counter = 0;

    if (dev->tail > 0 && dev->queue[dev->tail - 1]) {
        char *tmp = dev->queue[dev->tail - 1]->msg;
        while (*tmp != '\0') {
            if ((*tmp >= 'a' && *tmp <= 'z') || (*tmp >= 'A' && *tmp <= 'Z')) {
                letter_counter++;
            }
            tmp++;
        }
        printk(KERN_INFO "asyncmsg: got message with %d letters\n", letter_counter);
    }
}

static struct file_operations asyncmsg_fops = {
    .owner = THIS_MODULE,
    .open = asyncmsg_open,
    .release = asyncmsg_release,
    .read = asyncmsg_read,
    .write = asyncmsg_write,
    .unlocked_ioctl = asyncmsg_ioctl,
    .fasync = asyncmsg_fasync,
    .poll = asyncmsg_poll,
};

static int __init asyncmsg_init(void)
{
    int err;
    err = alloc_chrdev_region(&asyncmsg_devno, 0, 1, "asyncmsg");
    asyncmsg_major = MAJOR(asyncmsg_devno);
    if(err < 0)
    {
        printk(KERN_ERR "asyncmsg: failed to allocate device number\n");
        return err;
    } 
    asyncmsg_dev.max_queue_size = MAX_QUEUE_SIZE;
    asyncmsg_dev.queue = kmalloc_array(asyncmsg_dev.max_queue_size, sizeof(struct async_msg *), GFP_KERNEL);
    if (!asyncmsg_dev.queue) {
        printk(KERN_ERR "asyncmsg: failed to allocate queue\n");
        return -ENOMEM;
    }
    memset(asyncmsg_dev.queue, 0, asyncmsg_dev.max_queue_size * sizeof(struct async_msg *));

    asyncmsg_dev.asyncmsg_cache = kmem_cache_create("asyncmsg_cache", sizeof(struct async_msg), 0,
                                    SLAB_HWCACHE_ALIGN, asyncmsg_contructor);
    
    if(!asyncmsg_dev.asyncmsg_cache)
    {
        printk(KERN_ERR "asyncmsg: failed to create slab cache\n");
        return -ENOMEM;
    }

    asyncmsg_dev.asyncmsg_mempool = mempool_create(MIN_POOL_OBJECTS, mempool_alloc_slab, 
                                            mempool_free_slab, asyncmsg_dev.asyncmsg_cache);
    if(!asyncmsg_dev.asyncmsg_mempool)
    {
        printk(KERN_ERR "asyncmsg: failed tp create mempool\n");
        return -ENOMEM;
    }

    // initializing device struct
    asyncmsg_dev.head = 0;
    asyncmsg_dev.tail = 0;
    asyncmsg_dev.free_messages = 0;
    asyncmsg_dev.open_count = 0;
    asyncmsg_dev.last_jiffies = 0;

    sema_init(&asyncmsg_dev.sem, 1);
    spin_lock_init(&asyncmsg_dev.lock);
    init_waitqueue_head (&asyncmsg_dev.read_q);
    init_waitqueue_head(&asyncmsg_dev.write_q);

    timer_setup(&asyncmsg_dev.stat_timer, asyncmsg_timer_fn, 0);
    mod_timer(&asyncmsg_dev.stat_timer, jiffies + msecs_to_jiffies(600000));

    tasklet_init(&asyncmsg_dev.msg_tasklet, asyncmsg_tasklet_fn, (unsigned long)&asyncmsg_dev);
    asyncmsg_dev.wq = create_singlethread_workqueue("asyncmsg_wq");
    if(!asyncmsg_dev.wq)
    {
        err = -ENOMEM;
        goto fail_wq;
    }
    // INIT_DELAYED_WORK(&asyncmsg_dev.heavy_job, asyncmsg_work_fn);

    cdev_init(&asyncmsg_dev.cdev, &asyncmsg_fops);
    asyncmsg_dev.cdev.owner = THIS_MODULE;
    err = cdev_add(&asyncmsg_dev.cdev, asyncmsg_devno, 1);
    if(err)
    {
        printk(KERN_ERR "asyncmsg: failed to add cdev\n");
        goto fail_cdev;
    }

    asyncmsg_class = class_create("asyncmsg");
    if(IS_ERR(asyncmsg_class))
    {
        err = PTR_ERR(asyncmsg_class);
        printk(KERN_ERR "asyncmsg: failed to create class\n");
        goto fail_class;
    }

    device_create(asyncmsg_class, NULL, asyncmsg_devno, NULL, "asyncmsg");

    printk(KERN_INFO "asyncmsg: module loaded with major\n");

    return 0;

fail_class:
    cdev_del(&asyncmsg_dev.cdev);
fail_cdev:
    destroy_workqueue(asyncmsg_dev.wq);
fail_wq:
    unregister_chrdev_region(asyncmsg_devno, 1);
    return err;
}   


static void __exit asyncmsg_exit(void)
{
    for (int i = 0; i < asyncmsg_dev.tail; i++) {
        if (asyncmsg_dev.queue[i]) {
            mempool_free(asyncmsg_dev.queue[i], asyncmsg_dev.asyncmsg_mempool);
            asyncmsg_dev.queue[i] = NULL;
        }
    }
    kfree(asyncmsg_dev.queue);
    device_destroy(asyncmsg_class, asyncmsg_devno);
    class_destroy(asyncmsg_class);
    cdev_del(&asyncmsg_dev.cdev);
    del_timer_sync(&asyncmsg_dev.stat_timer);
    mempool_destroy(asyncmsg_dev.asyncmsg_mempool);
    kmem_cache_destroy(asyncmsg_dev.asyncmsg_cache);
    unregister_chrdev_region(asyncmsg_devno, 1);
    printk(KERN_INFO "asyncmsg: module unloaded\n");
}

module_init(asyncmsg_init);
module_exit(asyncmsg_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Timur5050");
MODULE_DESCRIPTION("driver that covers almost all material from 0 to 7, 9 chapters from book");
MODULE_VERSION("1.0.0");