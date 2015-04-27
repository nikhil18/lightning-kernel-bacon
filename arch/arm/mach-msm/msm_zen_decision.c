/* linux/arch/arm/mach-msm/msm_zen_decision.c
 *
 * In-kernel replacement for MSM/MPDecision userspace service.
 *
 * Copyright (c) 2015 Brandon Berhent
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define ZEN_DECISION "zen_decision"

/* Enable/Disable driver */
unsigned int enabled = 1;

/* How long to wait to disable cores on suspend (in ms) */
unsigned int suspend_wait_time = 5000;

static struct notifier_block fb_notifier;

static struct workqueue_struct *zen_suspend_wq;
static struct delayed_work suspend_work;

struct kobject *zendecision_kobj;

/*
 * __msm_zen_dec_suspend
 *
 * Core suspend work function.
 * Brings all CPUs except CPU0 offline
 */
static void __msm_zen_dec_suspend(struct work_struct *work)
{
	int cpu;

	for_each_online_cpu(cpu) {
		/* Don't call cpu_down if cpu0 */
		if (cpu == 0) continue;
		cpu_down(cpu);
	}
}

/*
 * msm_zen_dec_suspend
 *
 * Call __msm_zen_dec_suspend as delayed work by suspend_wait_time
 * This is configurably delayed to avoid excessive CPU downing
 */
static int msm_zen_dec_suspend(void)
{
	int ret;

	INIT_DELAYED_WORK(&suspend_work, __msm_zen_dec_suspend);
	ret = queue_delayed_work_on(0, zen_suspend_wq, &suspend_work,
			msecs_to_jiffies(suspend_wait_time));

	return ret;
}

/*
 * msm_zen_dec_resume
 *
 * Core resume function.
 * Cancels suspend work and brings all CPUs online.
 */
static void __ref msm_zen_dec_resume(void)
{
	int cpu;

	/* Clear suspend workqueue */
	flush_workqueue(zen_suspend_wq);
	cancel_delayed_work_sync(&suspend_work);

	for_each_cpu_not(cpu, cpu_online_mask) {
		/* Don't call cpu_up if cpu0 */
		if (cpu == 0) continue;
		cpu_up(cpu);
	}
}

/** Use FB notifiers to detect screen off/on and do the work **/
static int fb_notifier_callback(struct notifier_block *nb,
	unsigned long event, void *data)
{
	int *blank;
	struct fb_event *evdata = data;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK)
			msm_zen_dec_resume();
		else if (*blank == FB_BLANK_POWERDOWN)
			msm_zen_dec_suspend();
	}

	return 0;
}

/* Sysfs Start */
static ssize_t enable_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", enabled);
}

static ssize_t enable_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t size)
{
	int ret;
	unsigned long new_val;

	ret = kstrtoul(buf, 0, &new_val);
	if (ret < 0)
		return ret;

	if (new_val > 0)
		enabled = 1;
	else
		enabled = 0;

	return size;
}

static ssize_t suspend_delay_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", suspend_wait_time);
}

static ssize_t suspend_delay_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t size)
{
	int ret;
	unsigned long new_val;
	ret = kstrtoul(buf, 0, &new_val);
	if (ret < 0)
		return ret;

	if (new_val < 0)
		suspend_wait_time = 0;

	return size;
}

static struct kobj_attribute kobj_enabled =
	__ATTR(enabled, 0644, enable_show,
		enable_store);

static struct kobj_attribute kobj_suspend_wait =
	__ATTR(suspend_wait_time, 0644, suspend_delay_show,
		suspend_delay_store);

static struct attribute *zen_decision_attrs[] = {
	&kobj_enabled.attr, &kobj_suspend_wait.attr, NULL,
};

static struct attribute_group zen_decision_option_group = {
	.attrs = zen_decision_attrs,
};

/* Sysfs End */

static int zen_decision_probe(struct platform_device *pdev)
{
	int ret;

	/* Set default settings */
	enabled = 1;

	/* Setup sysfs */
	zendecision_kobj = kobject_create_and_add("zen_decision", kernel_kobj);
	if (zendecision_kobj == NULL) {
		pr_err("[%s]: subsystem register failed. \n", ZEN_DECISION);
		return -ENOMEM;
	}

	ret = sysfs_create_group(zendecision_kobj, &zen_decision_option_group);
	if (ret) {
		pr_info("[%s]: sysfs interface failed to initialize\n", ZEN_DECISION);
		return -EINVAL;
	} else
		pr_info("[%s]: sysfs interface initialized.\n", ZEN_DECISION);

	/* Setup Workqueues */
	zen_suspend_wq = alloc_workqueue("zen_suspend_wq", WQ_FREEZABLE, 0);
	if (!zen_suspend_wq) {
		pr_err("[%s]: Failed to allocate suspend workqueue\n", ZEN_DECISION);
		return -ENOMEM;
	}

	/* Setup FB Notifier */
	fb_notifier.notifier_call = fb_notifier_callback;
	if (fb_register_client(&fb_notifier)) {
		pr_err("[%s]: failed to register FB notifier\n", ZEN_DECISION);
		return -ENOMEM;
	}

	return ret;
}

static int zen_decision_remove(struct platform_device *pdev)
{
	kobject_put(zendecision_kobj);

	flush_workqueue(zen_suspend_wq);
	cancel_delayed_work_sync(&suspend_work);
	destroy_workqueue(zen_suspend_wq);

	fb_unregister_client(&fb_notifier);
	fb_notifier.notifier_call = NULL;

	return 0;
}

static struct platform_driver zen_decision_driver = {
	.probe = zen_decision_probe,
	.remove = zen_decision_remove,
	.driver = {
		.name = ZEN_DECISION,
		.owner = THIS_MODULE,
	}
};

static struct platform_device zen_decision_device = {
	.name = ZEN_DECISION,
	.id = -1
};

static int __init zen_decision_init(void)
{
	int ret = platform_driver_register(&zen_decision_driver);
	if (ret)
		pr_err("[%s]: platform_driver_register failed: %d\n", ZEN_DECISION, ret);
	else
		pr_info("[%s]: platform_driver_register succeeded\n", ZEN_DECISION);


	ret = platform_device_register(&zen_decision_device);
	if (ret)
		pr_err("[%s]: platform_device_register failed: %d\n", ZEN_DECISION, ret);
	else
		pr_info("[%s]: platform_device_register succeeded\n", ZEN_DECISION);

	return ret;
}

static void __exit zen_decision_exit(void)
{
	platform_driver_unregister(&zen_decision_driver);
	platform_device_unregister(&zen_decision_device);
}

late_initcall(zen_decision_init);
module_exit(zen_decision_exit);

MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Zen Decision MPDecision Replacement");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Brandon Berhent <bbedward@gmail.com>");
