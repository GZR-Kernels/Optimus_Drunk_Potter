/*
 * FPC1020 Fingerprint sensor device driver
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/wakelock.h>
#include <linux/notifier.h>

struct vreg_config {
	char *name;
	unsigned long vmin;
	unsigned long vmax;
	int ua_load;
};

static const struct vreg_config const vreg_conf[] = {
	{ "vdd_ana", 1800000UL, 1800000UL, 6000, },
	{ "vcc_spi", 1800000UL, 1800000UL, 10, },
	{ "vdd_io", 1800000UL, 1800000UL, 6000, },
};

struct fpc1020_data {
	struct clk *iface_clk;
	struct clk *core_clk;
	struct regulator *vreg[ARRAY_SIZE(vreg_conf)];

	struct device *dev;
	struct spi_device *spi;
	struct input_dev *input;
	struct notifier_block fb_notif;
	struct work_struct pm_work;
	spinlock_t irq_lock;

	bool irq_disabled;
	int clocks_enabled;
	int clocks_suspended;

	bool screen_off;

	int irq_gpio;
	int rst_gpio;
};

static int vreg_setup(struct fpc1020_data *fpc1020, const char *name,
	bool enable)
{
	size_t i;
	int rc;
	struct regulator *vreg;
	struct device *dev = fpc1020->dev;

	for (i = 0; i < ARRAY_SIZE(fpc1020->vreg); i++) {
		const char *n = vreg_conf[i].name;

		if (!strncmp(n, name, strlen(n)))
			goto found;
	}
	dev_err(dev, "Regulator %s not found\n", name);
	return -EINVAL;
found:
	vreg = fpc1020->vreg[i];
	if (enable) {
		if (!vreg) {
			vreg = regulator_get(dev, name);
			if (!vreg) {
				dev_err(dev, "Unable to get  %s\n", name);
				return -ENODEV;
			}
		}
		if (regulator_count_voltages(vreg) > 0) {
			rc = regulator_set_voltage(vreg, vreg_conf[i].vmin,
					vreg_conf[i].vmax);
			if (rc)
				dev_err(dev,
					"Unable to set voltage on %s, %d\n",
					name, rc);
		}
		rc = regulator_set_optimum_mode(vreg, vreg_conf[i].ua_load);
		if (rc < 0)
			dev_err(dev, "Unable to set current on %s, %d\n",
					name, rc);
		rc = regulator_enable(vreg);
		if (rc) {
			dev_err(dev, "error enabling %s: %d\n", name, rc);
			regulator_put(vreg);
			vreg = NULL;
		}
		fpc1020->vreg[i] = vreg;
	} else {
		if (vreg) {
			if (regulator_is_enabled(vreg)) {
				regulator_disable(vreg);
				dev_dbg(dev, "disabled %s\n", name);
			}
			regulator_put(vreg);
			fpc1020->vreg[i] = NULL;
		}
		rc = 0;
	}
	return rc;
}

static int __set_clks(struct fpc1020_data *fpc1020, bool enable)
{
	int rc = 0;

	if (enable) {
		dev_dbg(fpc1020->dev, "setting clk rates\n");
		rc = clk_set_rate(fpc1020->core_clk,
				fpc1020->spi->max_speed_hz);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error setting clk_rate: %u, %d\n",
					__func__, fpc1020->spi->max_speed_hz,
					rc);
			return rc;
		}
		dev_dbg(fpc1020->dev, "enabling core_clk\n");
		rc = clk_prepare_enable(fpc1020->core_clk);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error enabling core clk: %d\n",
					__func__, rc);
			goto out;
		}

		dev_dbg(fpc1020->dev, "enabling iface_clk\n");
		rc = clk_prepare_enable(fpc1020->iface_clk);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error enabling iface clk: %d\n",
					__func__, rc);
			clk_disable_unprepare(fpc1020->core_clk);
			goto out;
		}
		dev_dbg(fpc1020->dev, "%s ok. clk rate %u hz\n", __func__,
				fpc1020->spi->max_speed_hz);

	} else {
		dev_dbg(fpc1020->dev, "disabling clks\n");
		clk_disable_unprepare(fpc1020->iface_clk);
		clk_disable_unprepare(fpc1020->core_clk);
	}

out:
	return rc;
}

static int set_clks(struct fpc1020_data *fpc1020, bool enable)
{
	if (!enable) {
		if (!fpc1020->clocks_enabled) {
			dev_err(fpc1020->dev, "%s clock already disabled\n",
				__func__);
			return 0;
		}
		fpc1020->clocks_enabled--;
		if (!fpc1020->clocks_enabled)
			return __set_clks(fpc1020, enable);
	} else {
		if (fpc1020->clocks_enabled) {
			dev_err(fpc1020->dev, "%s: clock already enabled\n",
				__func__);
			fpc1020->clocks_enabled++;
			return 0;
		}
		fpc1020->clocks_enabled++;
		return __set_clks(fpc1020, enable);
	}
	return 0;
}

static void set_fpc_irq(struct fpc1020_data *f, bool enable)
{
	bool irq_disabled;

	spin_lock(&f->irq_lock);
	irq_disabled = f->irq_disabled;
	f->irq_disabled = !enable;
	spin_unlock(&f->irq_lock);

	if (enable == !irq_disabled)
		return;

	if (enable)
		enable_irq(gpio_to_irq(f->irq_gpio));
	else
		disable_irq(gpio_to_irq(f->irq_gpio));
}

/*
 * sysfs node to check the interrupt status of the sensor, the interrupt
 * handler should perform sysf_notify to allow userland to poll the node.
 */
static ssize_t dev_enable_set(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	int state = (*buf == '1') ? 1 : 0;

	dev_dbg(fpc1020->dev, "%s state = %d\n", __func__, state);
	return 1;
}

static ssize_t clk_enable_set(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	return set_clks(fpc1020, (*buf == '1')) ? : count;
}

static ssize_t irq_get(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct fpc1020_data *f = dev_get_drvdata(dev);
	bool irq_disabled;
	int irq;
	ssize_t count;

	spin_lock(&f->irq_lock);
	irq_disabled = f->irq_disabled;
	spin_unlock(&f->irq_lock);

	irq = !irq_disabled && gpio_get_value(f->irq_gpio);
	count = scnprintf(buf, PAGE_SIZE, "%d\n", irq);

	return count;
}

static ssize_t screen_state_get(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct fpc1020_data *f = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", !f->screen_off);
}

static DEVICE_ATTR(dev_enable, S_IWUSR | S_IWGRP, NULL, dev_enable_set);
static DEVICE_ATTR(clk_enable, S_IWUSR | S_IWGRP, NULL, clk_enable_set);
static DEVICE_ATTR(irq, S_IRUSR | S_IRGRP, irq_get, NULL);
static DEVICE_ATTR(screen_state, S_IRUSR, screen_state_get, NULL);

static struct attribute *attributes[] = {
	&dev_attr_dev_enable.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	&dev_attr_screen_state.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static void set_fingerprint_hal_nice(int nice)
{
	struct task_struct *p;

	read_lock(&tasklist_lock);
	for_each_process(p) {
		if(!memcmp(p->comm, "fps_work", 9)) {
			set_user_nice(p, nice);
			break;
		}
	}
	read_unlock(&tasklist_lock);
}

static void fpc1020_suspend_resume(struct work_struct *work)
{
	struct fpc1020_data *f = container_of(work, typeof(*f), pm_work);

	/* Escalate fingerprintd priority when screen is off */
	if (f->screen_off) {
		set_fingerprint_hal_nice(MIN_NICE);
	} else {
		set_fpc_irq(f, true);
		set_fingerprint_hal_nice(0);
	}

	sysfs_notify(&f->dev->kobj, NULL, dev_attr_screen_state.attr.name);
}

static int fb_notifier_callback(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct fpc1020_data *f = container_of(nb, typeof(*f), fb_notif);
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	if (action != FB_EARLY_EVENT_BLANK)
		return 0;

	if (*blank == FB_BLANK_UNBLANK) {
		cancel_work_sync(&f->pm_work);
		f->screen_off = false;
		queue_work(system_highpri_wq, &f->pm_work);
	} else if (*blank == FB_BLANK_POWERDOWN) {
		cancel_work_sync(&f->pm_work);
		f->screen_off = true;
		queue_work(system_highpri_wq, &f->pm_work);
	}

	return 0;
}

static irqreturn_t fpc1020_irq_handler(int irq, void *dev_id)
{
	struct fpc1020_data *f = dev_id;

	if (f->screen_off){
		pm_wakeup_event(f->dev, 1000);
	}

	sysfs_notify(&f->dev->kobj, NULL, dev_attr_irq.attr.name);

	return IRQ_HANDLED;
}

static int fpc1020_request_named_gpio(struct fpc1020_data *f,
	const char *label, int *gpio)
{
	struct device *dev = f->dev;
	struct device_node *np = dev->of_node;
	int ret;

	ret = of_get_named_gpio(np, label, 0);
	if (ret < 0) {
		dev_err(dev, "failed to get '%s'\n", label);
		return ret;
	}

	*gpio = ret;

	ret = devm_gpio_request(dev, *gpio, label);
	if (ret) {
		dev_err(dev, "failed to request gpio %d\n", *gpio);
		return ret;
	}

	return 0;
}

static int fpc1020_probe(struct spi_device *spi)
{
   	struct device *dev = &spi->dev;
	struct device_node *np = dev->of_node;
	struct fpc1020_data *f;
	int ret;

	if (!np) {
		dev_err(dev, "no of node found\n");
		return -EINVAL;
	}

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f) {
		dev_err(dev, "devm_kzalloc failed for struct fpc1020_data\n");
		return -ENOMEM;
	}

	f->dev = dev;
	dev_set_drvdata(dev, f);
	f->spi = spi;

	ret = fpc1020_request_named_gpio(f, "fpc,gpio_irq", &f->irq_gpio);
	if (ret)
		goto err1;

	ret = fpc1020_request_named_gpio(f, "fpc,gpio_rst", &f->rst_gpio);
	if (ret)
		goto err1;

	spin_lock_init(&f->irq_lock);
	INIT_WORK(&f->pm_work, fpc1020_suspend_resume);
	f->clocks_enabled = 0;
	f->clocks_suspended = 0;

	ret = sysfs_create_group(&dev->kobj, &attribute_group);
	if (ret) {
		dev_err(dev, "Could not create sysfs, ret: %d\n", ret);
		goto err2;
	}

	ret = devm_request_threaded_irq(dev, gpio_to_irq(f->irq_gpio),
			NULL, fpc1020_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			dev_name(dev), f);
	if (ret) {
		dev_err(dev, "Could not request irq, ret: %d\n", ret);
		goto err3;
	}

	f->fb_notif.notifier_call = fb_notifier_callback;
	ret = fb_register_client(&f->fb_notif);
	if (ret) {
		dev_err(dev, "Unable to register fb_notifier, ret: %d\n", ret);
		goto err4;
	}

	if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot")) {
		set_clks(f, true);
	}

	gpio_direction_input(f->irq_gpio);
	gpio_direction_output(f->rst_gpio, 1);
	device_init_wakeup(dev, true);

	return 0;
err4:
        devm_free_irq(dev, gpio_to_irq(f->irq_gpio), f);
err3:
	sysfs_remove_group(&dev->kobj, &attribute_group);
err2:
	input_unregister_device(f->input);
	input_free_device(f->input);
err1:
	devm_kfree(dev, f);
	return ret;
}

static int fpc1020_remove(struct spi_device *spi)
{
	struct fpc1020_data *f = dev_get_drvdata(&spi->dev);

	if (f->input != NULL)
		input_free_device(f->input);

	sysfs_remove_group(&spi->dev.kobj, &attribute_group);
	(void)vreg_setup(f, "vdd_io", false);
	(void)vreg_setup(f, "vcc_spi", false);
	(void)vreg_setup(f, "vdd_ana", false);
	return 0;
}

static int fpc1020_suspend(struct spi_device *spi, pm_message_t mesg)
{
	struct fpc1020_data *f = dev_get_drvdata(&spi->dev);

	f->clocks_suspended = f->clocks_enabled;
	if (f->clocks_suspended)
		__set_clks(f, false);
	enable_irq_wake(gpio_to_irq(f->irq_gpio));
	return 0;
}

static int fpc1020_resume(struct spi_device *spi)
{
	struct fpc1020_data *f = dev_get_drvdata(&spi->dev);

	if (f->clocks_suspended)
		__set_clks(f, true);
	disable_irq_wake(gpio_to_irq(f->irq_gpio));
	return 0;
}

static struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{ }
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct spi_driver fpc1020_driver = {
	.probe		= fpc1020_probe,
	.driver = {
		.name	= "fpc1020",
		.owner	= THIS_MODULE,
		.of_match_table = fpc1020_of_match,
	},
	.remove = fpc1020_remove,
	.suspend = fpc1020_suspend,
	.resume = fpc1020_resume,
};

static int __init fpc1020_init(void)
{
	return spi_register_driver(&fpc1020_driver);
}

static void __exit fpc1020_exit(void)
{
	return spi_unregister_driver(&fpc1020_driver);
}

module_init(fpc1020_init);
module_exit(fpc1020_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
