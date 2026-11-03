/*
 * hardwareInfo.c - Show hardware info via procfs
 *
 * Created by liunianliang, 2018/02/09
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>          /* was <asm/uaccess.h> */

#define HARDWARE_ROOT_DIR	"hardwareinfo"
#define FOUR_IN_ONE_DIR		"wcnss"
#define FOUR_IN_ONE_MODEL	"WCN3980"
#define GPS_BASEBAND_VERSION	"MPSS.AT.3.1-00484-SDM660_GEN_PACK-1"
#define BTFM_VERSION		"BTFM.CHE.2.1.1.c2-00099-QCACHROMZ-1"
#define WLAN_VERSION		"WLAN.HL.1.0.1.c2-00308-QCAHLSWMTPLZ-1"

static const char * const wcnsses[] = { "gps", "bt", "wifi", "fm" };
static const char * const wcnss_type[] = { "model", "version" };

/* ---- model (common) ---- */

static int wcnss_model_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", FOUR_IN_ONE_MODEL);
	return 0;
}

static int wcnss_model_open(struct inode *inode, struct file *file)
{
	return single_open(file, wcnss_model_show, inode->i_private);
}

static const struct file_operations wcnss_model_fops = {
	.open    = wcnss_model_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---- GPS version ---- */

static int wcnss_gps_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", GPS_BASEBAND_VERSION);
	return 0;
}

static int wcnss_gps_open(struct inode *inode, struct file *file)
{
	return single_open(file, wcnss_gps_show, inode->i_private);
}

static const struct file_operations wcnss_gps_fops = {
	.open    = wcnss_gps_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---- BT version ---- */

static int wcnss_bt_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", BTFM_VERSION);
	return 0;
}

static int wcnss_bt_open(struct inode *inode, struct file *file)
{
	return single_open(file, wcnss_bt_show, inode->i_private);
}

static const struct file_operations wcnss_bt_fops = {
	.open    = wcnss_bt_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---- FM version (reuses BT open) ---- */

static const struct file_operations wcnss_fm_fops = {
	.open    = wcnss_bt_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---- WLAN version ---- */

static int wcnss_wifi_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", WLAN_VERSION);
	return 0;
}

static int wcnss_wifi_open(struct inode *inode, struct file *file)
{
	return single_open(file, wcnss_wifi_show, inode->i_private);
}

static const struct file_operations wcnss_wifi_fops = {
	.open    = wcnss_wifi_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---- version fops table ---- */

static const struct file_operations * const version_fops[] = {
	&wcnss_gps_fops,   /* 0 = gps  */
	&wcnss_bt_fops,    /* 1 = bt   */
	&wcnss_wifi_fops,  /* 2 = wifi */
	&wcnss_fm_fops,    /* 3 = fm   */
};

/* ---- init ---- */

static int __init hardware_info_init(void)
{
	struct proc_dir_entry *root, *wcnss, *sub;
	int i;

	root = proc_mkdir(HARDWARE_ROOT_DIR, NULL);
	if (!root) {
		pr_err("create /proc/%s failed\n", HARDWARE_ROOT_DIR);
		return -ENOMEM;
	}

	wcnss = proc_mkdir(FOUR_IN_ONE_DIR, root);
	if (!wcnss) {
		pr_err("create /proc/%s/%s failed\n",
		       HARDWARE_ROOT_DIR, FOUR_IN_ONE_DIR);
		return -ENOMEM;
	}

	for (i = 0; i < ARRAY_SIZE(wcnsses); i++) {
		sub = proc_mkdir(wcnsses[i], wcnss);
		if (!sub) {
			pr_err("create %s subdir failed\n", wcnsses[i]);
			continue;
		}
		proc_create(wcnss_type[0], 0444, sub, &wcnss_model_fops);
		proc_create(wcnss_type[1], 0444, sub, version_fops[i]);
	}

	return 0;
}
device_initcall(hardware_info_init);

MODULE_DESCRIPTION("Hardware Info procfs driver");
MODULE_LICENSE("GPL");
