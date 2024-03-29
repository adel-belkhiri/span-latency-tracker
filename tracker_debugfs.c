/*
 * Copyright (C) 2014 Julien Desfossez <jdesfossez@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/sched.h>
#include <linux/fs.h>

#include "tracker_debugfs.h"
#include "latency_tracker.h"
#include "tracker_private.h"
#include "wrapper/trace-clock.h"

#define DEBUGFSNAME "latency"

static struct dentry *debugfs_root;
static struct dentry *debugfs_create_tracking_on(umode_t mode,
		struct dentry *parent, struct latency_tracker *tracker);
static struct dentry *debugfs_create_alloc_size(umode_t mode,
		struct dentry *parent, struct latency_tracker *tracker);
static struct dentry *debugfs_create_allocate(umode_t mode,
		struct dentry *parent, struct latency_tracker *tracker);
static int latency_tracker_debugfs_setup_stats(struct latency_tracker *tracker);

int latency_tracker_debugfs_setup(void)
{
	debugfs_root = debugfs_create_dir(DEBUGFSNAME, NULL);
	if (!debugfs_root)
		goto error;

	return 0;

error:
	return -1;
}

void latency_tracker_debugfs_cleanup(void)
{
	debugfs_remove_recursive(debugfs_root);
}

int setup_default_entries(struct latency_tracker *tracker)
{

	debugfs_create_u64("threshold", S_IRUSR|S_IWUSR,
			tracker->debugfs_instance_dir, &tracker->threshold);

	debugfs_create_u64("timeout", S_IRUSR|S_IWUSR,
			tracker->debugfs_instance_dir, &tracker->timeout);

	debugfs_create_u32("worst_threshold", S_IRUSR|S_IWUSR,
			tracker->debugfs_instance_dir,
			&tracker->worst_threshold);

	debugfs_create_tracking_on(S_IRUSR|S_IWUSR,
			tracker->debugfs_instance_dir, tracker);

	debugfs_create_alloc_size(S_IRUSR|S_IWUSR,
			tracker->debugfs_instance_dir, tracker);

	debugfs_create_allocate(S_IRUSR|S_IWUSR,
			tracker->debugfs_instance_dir, tracker);

	latency_tracker_debugfs_setup_stats(tracker);

	return 0;
}

static
ssize_t read_wakeup_pipe(struct file *filp, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct latency_tracker *tracker = filp->private_data;

	wait_event_interruptible(tracker->read_wait,
			tracker->got_alert);
	tracker->got_alert = false;

	return 0;
}

void latency_tracker_debugfs_wakeup_pipe(struct latency_tracker *tracker)
{
	uint64_t ts = trace_clock_read64();

	/* Rate limiter */
	if ((ts - tracker->last_wakeup_ts) < tracker->wakeup_rate_limit_ns)
		return;

	if (atomic_read(&tracker->wakeup_readers))
		irq_work_queue(&tracker->wake_irq);
	tracker->last_wakeup_ts = ts;
}

static
int open_wakeup_pipe(struct inode *inode, struct file *filp)
{
	int ret;
	struct latency_tracker *tracker = inode->i_private;

	filp->private_data = tracker;
	atomic_inc(&tracker->wakeup_readers);
	ret = try_module_get(THIS_MODULE);
	if (!ret)
		return -1;

	return 0;
}

static
int release_wakeup_pipe(struct inode *inode, struct file *filp)
{
	struct latency_tracker *tracker = filp->private_data;
	atomic_dec(&tracker->wakeup_readers);
	module_put(THIS_MODULE);

	return 0;
}

static
const struct file_operations wakeup_pipe_fops = {
	.open           = open_wakeup_pipe,
	.release		= release_wakeup_pipe,
	.read           = read_wakeup_pipe,
	.llseek         = default_llseek,
	/* TODO: poll */
};

static
void irq_wake(struct irq_work *entry)
{
	struct latency_tracker *tracker = container_of(entry,
			struct latency_tracker, wake_irq);

	tracker->got_alert = true;
	wake_up_interruptible(&tracker->read_wait);
}

int latency_tracker_debugfs_setup_wakeup_pipe(struct latency_tracker *tracker)
{
	init_irq_work(&tracker->wake_irq, irq_wake);
	init_waitqueue_head(&tracker->read_wait);
	tracker->got_alert = false;
	/* FIXME: param */
	tracker->wakeup_rate_limit_ns = 1000000000;
	tracker->wakeup_pipe = debugfs_create_file("wakeup_pipe", S_IRUSR,
			tracker->debugfs_instance_dir, tracker, &wakeup_pipe_fops);
	if (!tracker->wakeup_pipe)
		return -1;

	return 0;
}

static
void destroy_wakeup_pipe(struct latency_tracker *tracker)
{
	if (!tracker->wakeup_pipe)
		return;

	irq_work_sync(&tracker->wake_irq);
	debugfs_remove(tracker->wakeup_pipe);
	tracker->wakeup_pipe = NULL;
}

static
int read_stats(struct seq_file *m, void *v)
{
	struct latency_tracker *tracker = (struct latency_tracker *) m->private;

	seq_printf(m, "count: %llu\n", tracker->count_delay);
	seq_printf(m, "min (ns): %llu\n", tracker->min_delay);
	seq_printf(m, "max (ns): %llu\n", tracker->max_delay);
	if (tracker->count_delay > 0)
		seq_printf(m, "average (ns): %llu\n",
				tracker->total_delay / tracker->count_delay);
	else
		seq_printf(m, "average (ns): 0\n");
	seq_printf(m, "total (ns): %llu\n", tracker->total_delay);

	return 0;
}

static
int open_stats(struct inode *inode, struct file *filp)
{
	struct latency_tracker *tracker = inode->i_private;

	return single_open(filp, read_stats, tracker);
}

static
const struct file_operations stats_fops = {
	.open           = open_stats,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static
void destroy_stats(struct latency_tracker *tracker)
{
	if (!tracker->stats_file)
		return;

	debugfs_remove(tracker->stats_file);
	tracker->stats_file = NULL;
}

static
int latency_tracker_debugfs_setup_stats(struct latency_tracker *tracker)
{
	tracker->stats_file = debugfs_create_file("stats", S_IRUSR,
			tracker->debugfs_instance_dir, tracker, &stats_fops);
	if (!tracker->stats_file)
		return -1;

	return 0;
}

int latency_tracker_debugfs_add_tracker(
		struct latency_tracker *tracker)
{
	struct dentry *dir;
	int ret;

	/*
	 * FIXME: when dealing with multiple instances, we will have to
	 * test to see if this directory already exists.
	 */
	dir = debugfs_create_dir(tracker->tracker_name, debugfs_root);
	if (!dir)
		goto error;
	tracker->debugfs_tracker_dir = dir;

	dir = debugfs_create_dir(tracker->instance_name, dir);
	if (!dir)
		goto error;
	tracker->debugfs_instance_dir = dir;

	ret = setup_default_entries(tracker);
	if (ret != 0)
		goto error_cleanup;

	return 0;

error_cleanup:
	latency_tracker_debugfs_remove_tracker(tracker);

error:
	return -1;
}

void latency_tracker_debugfs_remove_tracker(struct latency_tracker *tracker)
{
	if (!tracker->debugfs_instance_dir)
		return;
	destroy_wakeup_pipe(tracker);
	destroy_stats(tracker);
	debugfs_remove_recursive(tracker->debugfs_tracker_dir);
}

struct dentry *latency_tracker_debugfs_add_subfolder(
		struct latency_tracker *tracker, const char *name)
{
	struct dentry *dir;

	if (!tracker->debugfs_instance_dir)
		goto error;

	dir = debugfs_create_dir(name, tracker->debugfs_instance_dir);
	if (!dir)
		goto error;

	return dir;

error:
	return NULL;
}

int latency_open_generic(struct inode *inode, struct file *filp)
{
	filp->private_data = inode->i_private;
	return 0;
}

static
ssize_t read_int(struct file *filp, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int r, ret;
	char buf[64];
	int *val = filp->private_data;

	r = snprintf(buf, min_t(size_t, count, 64), "%d\n", *val);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, r);
	return ret;
}

static
ssize_t write_int(struct file *filp, const char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int val, ret;
	int *out_val;

	ret = kstrtoint_from_user(ubuf, count, 10, &val);
	if (ret)
		return ret;

	/* must have at least 1 entry */
	if (!val)
		return -EINVAL;
	out_val = filp->private_data;
	*out_val = val;

	return count;
}

static
int open_int(struct inode *inode, struct file *filp)
{
	filp->private_data = inode->i_private;
	return 0;
}

static
const struct file_operations fops_int = {
	.open           = open_int,
	.read           = read_int,
	.write		= write_int,
	.llseek         = default_llseek,
};

struct dentry *debugfs_create_int(const char *name, umode_t mode,
		struct dentry *parent, int *value)
{
	return debugfs_create_file(name, mode, parent, value, &fops_int);
}

/* tracking_on debugfs file */
static
ssize_t tracking_on_read(struct file *filp, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int r, ret;
	char buf[64];
	struct latency_tracker *tracker = filp->private_data;

	r = snprintf(buf, min_t(size_t, count, 64), "%d\n",
			tracker->tracking_on);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, r);
	return ret;
}

static
ssize_t tracking_on_write(struct file *filp, const char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int val, ret;
	struct latency_tracker *tracker = filp->private_data;

	ret = kstrtoint_from_user(ubuf, count, 10, &val);
	if (ret)
		return ret;
	if (tracker->tracking_on == val)
		goto end;

	latency_tracker_set_tracking_on(tracker, val, 1);

end:
	return count;
}

static
const struct file_operations fops_tracking_on = {
	.open           = open_int,
	.read           = tracking_on_read,
	.write		= tracking_on_write,
	.llseek         = default_llseek,
};

static
struct dentry *debugfs_create_tracking_on(umode_t mode, struct dentry *parent,
		struct latency_tracker *tracker)
{
	return debugfs_create_file("tracking_on", mode, parent,
			tracker, &fops_tracking_on);
}

/* Tracker allocated size before startup */
static
ssize_t alloc_size_read(struct file *filp, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int r, ret;
	char buf[64];
	struct latency_tracker *tracker = filp->private_data;

	r = snprintf(buf, min_t(size_t, count, 64), "%d\n",
			tracker->free_list_nelems);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, r);
	return ret;
}

static
ssize_t alloc_size_write(struct file *filp, const char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int val, ret;
	struct latency_tracker *tracker = filp->private_data;

	ret = kstrtoint_from_user(ubuf, count, 10, &val);
	if (ret)
		return ret;

	/* Once the memory has been allocated, we cannot change this size */
	if (tracker->allocated)
		return -EPERM;

	latency_tracker_set_startup_events(tracker, val);

	return count;
}

static
const struct file_operations fops_alloc_size = {
	.open           = open_int,
	.read           = alloc_size_read,
	.write			= alloc_size_write,
	.llseek         = default_llseek,
};

static
struct dentry *debugfs_create_alloc_size(umode_t mode, struct dentry *parent,
		struct latency_tracker *tracker)
{
	return  debugfs_create_file("alloc_size", mode, parent,
			tracker, &fops_alloc_size);
}

/* Allocate the tracker memory */
static
ssize_t allocate_read(struct file *filp, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int r, ret;
	char buf[64];
	struct latency_tracker *tracker = filp->private_data;

	r = snprintf(buf, min_t(size_t, count, 64), "%d\n",
			tracker->allocated);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, r);
	return ret;
}

static
ssize_t allocate_write(struct file *filp, const char __user *ubuf,
		size_t count, loff_t *ppos)
{
	int val, ret;
	struct latency_tracker *tracker = filp->private_data;

	ret = kstrtoint_from_user(ubuf, count, 10, &val);
	if (ret)
		return ret;
	if (tracker->allocated)
		return -EPERM;

	ret = latency_tracker_allocate(tracker);
	if (ret != 0)
		return ret;

	return count;
}

static
const struct file_operations fops_allocate = {
	.open           = open_int,
	.read           = allocate_read,
	.write			= allocate_write,
	.llseek         = default_llseek,
};

static
struct dentry *debugfs_create_allocate(umode_t mode, struct dentry *parent,
		struct latency_tracker *tracker)
{
	return  debugfs_create_file("allocate", mode, parent,
			tracker, &fops_allocate);
}
