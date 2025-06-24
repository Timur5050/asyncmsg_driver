#include "asyncmsg.h"


static struct asyncmsg_dev asyncmsg_dev;
static dev_t asyncmsg_devno;
static struct class *asyncmsg_class;
static int asyncmsg_major = 0;

static void asyncmsg_timer_fn(struct timer_list *t);

static int culc_free_space(struct asyncmsg_dev *dev)
{
    return (dev->tail < MAX_QUEUE_SIZE) ? (MAX_QUEUE_SIZE - dev->tail) : 0;
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

    struct async_msg *curr_msg = &dev->queue[dev->head];
    curr_msg->processed = true;

    len = snprintf(tmp, sizeof(tmp),
                "message: %s\n"
                "len: %ld\n"
                "timestamp_ns: %lld\n"
                "processed: %d\n",
                curr_msg->msg,
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

    if (dev->tail >= MAX_QUEUE_SIZE)
    {
        up(&dev->sem);
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

    count = min((size_t)MAX_MSG_LEN, count);
    if(copy_from_user(tmp, buf, count))
    {
        up(&dev->sem);
        return -EFAULT;
    }   

    tmp[count] = '\0';
    struct async_msg new_mess;
    memcpy(new_mess.msg, tmp, count);
    new_mess.timestamp_ns = ktime_get_ns();
    new_mess.len = count;
    new_mess.processed = false;
    

    dev->queue[dev->tail] = new_mess;
    dev->tail++;
    dev->free_messages++;

    printk(KERN_INFO "asyncmsg: write message with len %d\n", count);
    wake_up(&dev->read_q);
    up(&dev->sem);

    return count;
}

static void asyncmsg_timer_fn(struct timer_list *t)
{
    struct asyncmsg_dev *dev = from_timer(dev, t, stat_timer);
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    printk(KERN_INFO  "asyncmsg: "
                        "head position: %d\n"
                        "tail position: %d\n"
                        "free messages: %d\n"
                        "open count: %d\n"
                        "write delay ms: %d\n",
                        dev->head, 
                        dev->tail,
                        dev->free_messages,
                        dev->open_count,
                        dev->write_delay_ms);
    spin_unlock_irqrestore(&dev->lock, flags);

    mod_timer(&dev->stat_timer, jiffies + msecs_to_jiffies(10000));
}


static struct file_operations asyncmsg_fops = {
    .owner = THIS_MODULE,
    .open = asyncmsg_open,
    .release = asyncmsg_release,
    .read = asyncmsg_read,
    .write = asyncmsg_write,
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

    // initializing device struct
    asyncmsg_dev.head = 0;
    asyncmsg_dev.tail = 0;
    asyncmsg_dev.free_messages = 0;
    asyncmsg_dev.open_count = 0;

    sema_init(&asyncmsg_dev.sem, 1);
    spin_lock_init(&asyncmsg_dev.lock);
    init_waitqueue_head (&asyncmsg_dev.read_q);
    init_waitqueue_head(&asyncmsg_dev.write_q);

    timer_setup(&asyncmsg_dev.stat_timer, asyncmsg_timer_fn, 0);
    mod_timer(&asyncmsg_dev.stat_timer, jiffies + msecs_to_jiffies(10000));

    // mod_timer(&asyncmsg_dev.stat_timer, jiffies + msecs_to_jiffies(10000));
    // tasklet_init(&asyncmsg_dev.tasklet, asyncmsg_tasklet_fn, (unsigned long)&asyncmsg_dev);
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
    device_destroy(asyncmsg_class, asyncmsg_devno);
    class_destroy(asyncmsg_class);
    cdev_del(&asyncmsg_dev.cdev);
    // flush_workqueue(asyncmsg_dev.wq);
    // destroy_workqueue(asyncmsg_dev.wq);
    // del_timer_sync(&asyncmsg_dev.timer);
    // tasklet_kill(&asyncmsg_dev.tasklet);
    unregister_chrdev_region(   asyncmsg_devno, 1);
    printk(KERN_INFO "asyncmsg: module unloaded\n");
}


module_init(asyncmsg_init);
module_exit(asyncmsg_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Timur5050");
MODULE_DESCRIPTION("driver that covers almost all material from 0 to 7 chapters from book");
MODULE_VERSION("1.0.0");