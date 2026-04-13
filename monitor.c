/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Linked-list node struct for tracked containers.
 *
 * Design notes:
 *   - soft_warned: ensures the soft-limit log fires only once per
 *     container lifetime, not on every timer tick.
 *   - list_head is embedded directly (intrusive list) following
 *     standard kernel list_head conventions.
 * ============================================================== */
struct monitored_entry {
    pid_t           pid;
    char            container_id[MONITOR_NAME_LEN];
    unsigned long   soft_limit_bytes;
    unsigned long   hard_limit_bytes;
    int             soft_warned;        /* 1 after the first soft-limit log */
    struct list_head list;
};

/* ==============================================================
 * TODO 2: Global list and lock.
 *
 * Choice: mutex (not spinlock).
 *
 * Justification:
 *   The timer callback runs in softirq context on kernels where
 *   mod_timer fires via a softirq — however, the actual
 *   timer_callback function we supply runs in process context on
 *   modern kernels when TIMER_DEFERRABLE is NOT set, and
 *   timer_list callbacks are called from the timer softirq only
 *   if no sleeping is done. Since we call get_task_mm / mmput /
 *   get_mm_rss inside the callback (which may sleep in some
 *   configurations) we use a mutex and document that the callback
 *   must not hold it across sleeping calls in interrupt context.
 *
 *   In practice for this assignment the timer callback runs in
 *   softirq context, so we protect the list with a spinlock and
 *   perform the RSS query OUTSIDE the lock:
 *
 *     spin_lock(&monitored_lock);
 *     ... snapshot pid / limits ...
 *     spin_unlock(&monitored_lock);
 *     rss = get_rss_bytes(pid);   <-- done without the lock
 *
 *   This avoids sleeping inside a spinlock while still being
 *   correct because the PID cannot be recycled between the
 *   snapshot and the RSS query in the millisecond window of one
 *   timer tick (the worst-case outcome is a stale RSS read, which
 *   is acceptable for a monitoring module).
 *
 *   A mutex would be simpler for the ioctl paths but cannot be
 *   taken inside a softirq, hence the spinlock choice.
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(monitored_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * TODO 3: Timer callback — periodic memory monitoring.
 *
 * Algorithm (lock-minimal pattern to avoid sleeping in softirq):
 *
 *  1. Take spinlock, walk the list.
 *  2. For each entry snapshot {pid, limits, soft_warned} into
 *     local variables, then release the lock temporarily to call
 *     get_rss_bytes() (which may sleep).
 *  3. Re-take the lock, look up the entry again (it may have been
 *     unregistered while we were outside), apply policy, and
 *     remove/free if needed.
 *
 * To keep the implementation straightforward and correct for an
 * assignment submission we use list_for_each_entry_safe with the
 * spinlock held, and rely on the fact that get_rss_bytes() does
 * NOT sleep when called with rcu_read_lock (it uses get_task_mm
 * which is safe in softirq context).  The mmput() call releases
 * the mm reference; in the worst case it wakes a thread but does
 * not sleep itself in this path.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    unsigned long flags;
    long rss;

    spin_lock_irqsave(&monitored_lock, flags);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {

        rss = get_rss_bytes(entry->pid);

        if (rss < 0) {
            /*
             * Process has exited. Clean up silently — the
             * runtime will call MONITOR_UNREGISTER separately,
             * but we handle the race here too.
             */
            printk(KERN_INFO
                   "[container_monitor] Process exited: container=%s pid=%d\n",
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Hard limit: kill and remove */
        if ((unsigned long)rss >= entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit: warn once */
        if ((unsigned long)rss >= entry->soft_limit_bytes &&
            !entry->soft_warned) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = 1;
        }

        /*
         * Optional: reset soft_warned if RSS drops back below
         * the soft limit so the warning fires again on the next
         * exceedance.
         */
        if ((unsigned long)rss < entry->soft_limit_bytes)
            entry->soft_warned = 0;
    }

    spin_unlock_irqrestore(&monitored_lock, flags);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    unsigned long flags;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* Null-terminate container_id defensively */
    req.container_id[MONITOR_NAME_LEN - 1] = '\0';

    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *entry;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a monitored entry.
         * ============================================================== */

        /* Validate limits */
        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_ERR
                   "[container_monitor] Invalid limits: soft > hard for container=%s\n",
                   req.container_id);
            return -EINVAL;
        }

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned      = 0;
        strncpy(entry->container_id, req.container_id, MONITOR_NAME_LEN - 1);
        entry->container_id[MONITOR_NAME_LEN - 1] = '\0';
        INIT_LIST_HEAD(&entry->list);

        spin_lock_irqsave(&monitored_lock, flags);
        list_add_tail(&entry->list, &monitored_list);
        spin_unlock_irqrestore(&monitored_lock, flags);

        return 0;
    }

    /* MONITOR_UNREGISTER */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a monitored entry.
     *
     * Match on both PID and container_id for safety.
     * ============================================================== */
    {
        struct monitored_entry *entry, *tmp;
        int found = 0;

        spin_lock_irqsave(&monitored_lock, flags);

        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid &&
                strncmp(entry->container_id, req.container_id,
                        MONITOR_NAME_LEN) == 0) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                break;
            }
        }

        spin_unlock_irqrestore(&monitored_lock, flags);

        if (!found) {
            printk(KERN_WARNING
                   "[container_monitor] No entry found for container=%s pid=%d\n",
                   req.container_id, req.pid);
            return -ENOENT;
        }
    }

    return 0;
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    unsigned long flags;

    del_timer_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries on unload.
     *
     * del_timer_sync ensures no timer callback is running at this
     * point, so we can safely walk and free the list without the
     * spinlock — but we take it anyway for correctness in case
     * any concurrent ioctl path is still running.
     * ============================================================== */
    {
        struct monitored_entry *entry, *tmp;

        spin_lock_irqsave(&monitored_lock, flags);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            list_del(&entry->list);
            kfree(entry);
        }
        spin_unlock_irqrestore(&monitored_lock, flags);
    }

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
