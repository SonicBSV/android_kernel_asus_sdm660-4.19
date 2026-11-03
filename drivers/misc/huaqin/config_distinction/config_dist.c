/*
 * config_dist.c - Configuration distinction driver
 *
 * Huaqin add for ZQL1820-136 by zhaojunhai1 at 2018/08/22
 * Huaqin add for ZQL1820-145 by zhaojunhai1 at 2018/08/19
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/workqueue.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>          /* was <asm/uaccess.h> */
#include <linux/semaphore.h>
#include <linux/poll.h>
#include <linux/fcntl.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/signal.h>
#include <linux/gpio.h>
#include <linux/mm.h>
#include <linux/of_gpio.h>
#include <linux/miscdevice.h>
#include <linux/regulator/consumer.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/pinctrl/consumer.h> /* missing in original */

/*
 * In upstream 4.19 the wakelock API is replaced by pm_wake_lock.
 * In Qualcomm MSM 4.19 kernels <linux/wakelock.h> may still exist.
 */
#include <linux/pm_wakeup.h>

#define CONFIG_DIST_IOCTL_MAGIC_NO	0xFA
#define CONFIG_DIST_INIT_GPIO		_IO(CONFIG_DIST_IOCTL_MAGIC_NO, 20)
#define CONFIG_DIST_RELEASE_DEVICE	_IO(CONFIG_DIST_IOCTL_MAGIC_NO, 25)

#define DEVICE_NAME "configdist_dev"

static u8 config_dist_debug = 0x01;

#define CONFIG_DIST_DBG(fmt, args...)					\
	do {								\
		if (config_dist_debug & 0x01)				\
			pr_debug("[config_dist]:%5d: <%s> " fmt,	\
				 __LINE__, __func__, ##args);		\
	} while (0)

#define CONFIG_DIST_ERR(fmt, args...)					\
	pr_err("[config_dist]:%5d: <%s> " fmt,				\
	       __LINE__, __func__, ##args)

struct configdist_data {
	struct platform_device	*config_dist_dev;
	struct miscdevice	*miscdev;
	u32			nfc_num;
	u32			mem_num;
	u32			mem1_num;
	u32			cam_num;
	int			nfc_val;
	int			mem_val;
	int			mem1_val;
	int			cam_val;
	struct fasync_struct	*async_queue;
	struct wakeup_source	*config_dist_lock;
	struct mutex		 buf_lock;
	struct pinctrl		*dist_pinctrl;
	struct pinctrl_state	*dist_pinctrl_mem_state;
	struct pinctrl_state	*dist_pinctrl_mem1_state;
	struct pinctrl_state	*dist_pinctrl_cam_state;
};

static struct configdist_data *g_configdist_data;

/* ---------- GPIO init / free ---------- */

static int config_dist_init_gpio(struct configdist_data *cd)
{
	int err;

	/* MEM */
	cd->dist_pinctrl_mem_state =
		pinctrl_lookup_state(cd->dist_pinctrl, "dist_mem_active");
	if (IS_ERR(cd->dist_pinctrl_mem_state)) {
		CONFIG_DIST_ERR("lookup dist_mem_active state failed\n");
		return PTR_ERR(cd->dist_pinctrl_mem_state);
	}
	pinctrl_select_state(cd->dist_pinctrl, cd->dist_pinctrl_mem_state);

	if (!gpio_is_valid(cd->mem_num)) {
		CONFIG_DIST_DBG("not valid mem gpio\n");
		return -EIO;
	}
	err = gpio_request(cd->mem_num, "config_dist-mem");
	if (err) {
		CONFIG_DIST_DBG("could not request mem gpio\n");
		return err;
	}
	gpio_direction_input(cd->mem_num);
	cd->mem_val = gpio_get_value(cd->mem_num);

	/* MEM1 */
	cd->dist_pinctrl_mem1_state =
		pinctrl_lookup_state(cd->dist_pinctrl, "dist_mem1_active");
	if (IS_ERR(cd->dist_pinctrl_mem1_state)) {
		CONFIG_DIST_ERR("lookup dist_mem1_active state failed\n");
		return PTR_ERR(cd->dist_pinctrl_mem1_state);
	}
	pinctrl_select_state(cd->dist_pinctrl, cd->dist_pinctrl_mem1_state);

	if (!gpio_is_valid(cd->mem1_num)) {
		CONFIG_DIST_DBG("not valid mem1 gpio\n");
		return -EIO;
	}
	err = gpio_request(cd->mem1_num, "config_dist-mem1");
	if (err) {
		CONFIG_DIST_DBG("could not request mem1 gpio\n");
		return err;
	}
	gpio_direction_input(cd->mem1_num);
	cd->mem1_val = gpio_get_value(cd->mem1_num);

	/* NFC */
	if (!gpio_is_valid(cd->nfc_num)) {
		CONFIG_DIST_DBG("not valid nfc gpio\n");
		return -EIO;
	}
	err = gpio_request(cd->nfc_num, "config_dist-nfc");
	if (err) {
		CONFIG_DIST_DBG("could not request nfc gpio\n");
		return err;
	}
	gpio_direction_input(cd->nfc_num);
	cd->nfc_val = gpio_get_value(cd->nfc_num);

	/* CAM */
	cd->dist_pinctrl_cam_state =
		pinctrl_lookup_state(cd->dist_pinctrl, "dist_cam_active");
	if (IS_ERR(cd->dist_pinctrl_cam_state)) {
		CONFIG_DIST_ERR("lookup dist_cam_active state failed\n");
		return PTR_ERR(cd->dist_pinctrl_cam_state);
	}
	pinctrl_select_state(cd->dist_pinctrl, cd->dist_pinctrl_cam_state);

	if (!gpio_is_valid(cd->cam_num)) {
		CONFIG_DIST_DBG("not valid cam gpio\n");
		return -EIO;
	}
	err = gpio_request(cd->cam_num, "config_dist-cam");
	if (err) {
		CONFIG_DIST_DBG("could not request cam gpio\n");
		return err;
	}
	gpio_direction_input(cd->cam_num);
	cd->cam_val = gpio_get_value(cd->cam_num);

	CONFIG_DIST_DBG("val mem[%d] mem1[%d] nfc[%d] cam[%d]\n",
			cd->mem_val, cd->mem1_val,
			cd->nfc_val, cd->cam_val);
	CONFIG_DIST_DBG("%s() ok\n", __func__);
	return 0;
}

static void config_dist_free_gpio(struct configdist_data *cd)
{
	CONFIG_DIST_DBG("%s() enter\n", __func__);

	if (gpio_is_valid(cd->mem_num))
		gpio_free(cd->mem_num);
	if (gpio_is_valid(cd->mem1_num))       /* was missing */
		gpio_free(cd->mem1_num);
	if (gpio_is_valid(cd->nfc_num))
		gpio_free(cd->nfc_num);
	if (gpio_is_valid(cd->cam_num))
		gpio_free(cd->cam_num);

	CONFIG_DIST_DBG("%s() ok\n", __func__);
}

/* ---------- DTS parsing ---------- */

static int config_dist_parse_dts(struct device *dev,
				 struct configdist_data *cd)
{
	cd->mem_num  = of_get_named_gpio(dev->of_node, "dist,mem_gpio",  0);
	cd->mem1_num = of_get_named_gpio(dev->of_node, "dist,mem1_gpio", 0);
	cd->nfc_num  = of_get_named_gpio(dev->of_node, "dist,nfc_gpio",  0);
	cd->cam_num  = of_get_named_gpio(dev->of_node, "dist,cam_gpio",  0);

	cd->dist_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(cd->dist_pinctrl)) {
		CONFIG_DIST_ERR("pinctrl get failed\n");
		return PTR_ERR(cd->dist_pinctrl);
	}

	CONFIG_DIST_DBG("node mem[%d] mem1[%d] nfc[%d] cam[%d]\n",
			cd->mem_num, cd->mem1_num,
			cd->nfc_num, cd->cam_num);
	return 0;
}

/* ---------- file_operations ---------- */

static int config_dist_open(struct inode *inode, struct file *file)
{
	file->private_data = g_configdist_data;
	return 0;
}

static int config_dist_release(struct inode *inode, struct file *file)
{
	if (!file->private_data)
		return -EIO;
	file->private_data = NULL;
	return 0;
}

static long config_dist_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	int err = 0;
	struct configdist_data *cd = filp->private_data;

	mutex_lock(&cd->buf_lock);
	switch (cmd) {
	case CONFIG_DIST_INIT_GPIO:
		err = config_dist_init_gpio(cd);
		break;
	case CONFIG_DIST_RELEASE_DEVICE:
		config_dist_free_gpio(cd);
		misc_deregister(cd->miscdev);
		break;
	default:
		break;
	}
	mutex_unlock(&cd->buf_lock);
	return err;
}

static ssize_t config_dist_read(struct file *filp, char __user *buf,
				size_t num, loff_t *offset)
{
	u8 val = 0x03;
	u8 ubuf = 0;
	struct configdist_data *cd = filp->private_data;

	mutex_lock(&cd->buf_lock);

	CONFIG_DIST_DBG("num: %zu\n", num);

	switch (num) {
	case 1:
		ubuf = (u8)(cd->nfc_val & 0xFF);
		copy_to_user(buf, &ubuf, 1);
		break;
	case 2:
		ubuf = (u8)((cd->mem_val << 1) | cd->mem1_val) & 0xFF;
		copy_to_user(buf + 1, &ubuf, 1);
		break;
	case 3:
		ubuf = (u8)(cd->cam_val & 0xFF);
		copy_to_user(buf + 2, &ubuf, 1);
		break;
	case 4:
		copy_to_user(buf + 3, &val, 1);
		break;
	default:
		break;
	}

	CONFIG_DIST_DBG("num: %zu ubuf: 0x%x\n", num, ubuf);
	mutex_unlock(&cd->buf_lock);
	return (ssize_t)num;
}

static const struct file_operations config_dist_fops = {
	.owner          = THIS_MODULE,
	.open           = config_dist_open,
	.unlocked_ioctl = config_dist_ioctl,
	.read           = config_dist_read,
	.release        = config_dist_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = config_dist_ioctl,
#endif
};

static struct miscdevice st_config_dist_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DEVICE_NAME,
	.fops  = &config_dist_fops,
};

/* ---------- proc: nfc_status ---------- */

static int nfc_status_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", g_configdist_data->nfc_val & 0xFF);
	return 0;
}

static int nfc_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, nfc_status_show, inode->i_private);
}

static const struct file_operations nfc_status_fops = {
	.open    = nfc_status_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---------- probe ---------- */

static int config_dist_probe(struct platform_device *pdev)
{
	struct configdist_data *cd;
	int status;

	CONFIG_DIST_DBG("probe enter\n");

	status = misc_register(&st_config_dist_dev);
	if (status) {
		CONFIG_DIST_ERR("misc register failed: %d\n", status);
		return status;
	}

	cd = kzalloc(sizeof(*cd), GFP_KERNEL);
	if (!cd) {
		status = -ENOMEM;
		goto err_deregister;
	}

	cd->miscdev         = &st_config_dist_dev;
	cd->config_dist_dev = pdev;
	mutex_init(&cd->buf_lock);
	cd->config_dist_lock = wakeup_source_register(&pdev->dev,
						      "config_dist");

	status = config_dist_parse_dts(&pdev->dev, cd);
	if (status) {
		CONFIG_DIST_ERR("parse dts failed: %d\n", status);
		goto err_free;
	}

	status = config_dist_init_gpio(cd);
	if (status) {
		CONFIG_DIST_ERR("init gpio failed: %d\n", status);
		goto err_free;
	}

	g_configdist_data = cd;

	proc_create("nfc_status", 0666, NULL, &nfc_status_fops);

	return 0;

err_free:
	kfree(cd);
err_deregister:
	misc_deregister(&st_config_dist_dev);
	return status;
}

/* ---------- driver registration ---------- */

static const struct of_device_id config_dist_of_match[] = {
	{ .compatible = "config,dist" },
	{ }
};
MODULE_DEVICE_TABLE(of, config_dist_of_match);

static const struct platform_device_id config_dist_id[] = {
	{ "config_dist", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, config_dist_id);

static struct platform_driver config_dist_driver = {
	.driver = {
		.name           = "config_dist",
		.of_match_table = config_dist_of_match,
	},
	.id_table = config_dist_id,
	.probe    = config_dist_probe,
};

static int __init config_dist_init(void)
{
	CONFIG_DIST_DBG("init\n");
	return platform_driver_register(&config_dist_driver);
}

static void __exit config_dist_exit(void)
{
	CONFIG_DIST_DBG("exit\n");
	platform_driver_unregister(&config_dist_driver);
}

module_init(config_dist_init);
module_exit(config_dist_exit);

MODULE_DESCRIPTION("Config Distinction Driver");
MODULE_LICENSE("GPL");
