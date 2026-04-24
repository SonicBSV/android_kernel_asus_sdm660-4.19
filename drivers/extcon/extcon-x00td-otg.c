// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS X00TD OTG extcon driver
 *
 * Detects OTG cable insertion via board OTG ID GPIO and exports
 * EXTCON_USB_HOST independently from charger driver.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/extcon-provider.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/power_supply.h>

#define X00TD_OTG_DEFAULT_POLL_MS	1000

static const unsigned int x00td_otg_cable[] = {
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

struct x00td_otg_extcon {
	struct device		*dev;
	struct extcon_dev	*edev;
	struct delayed_work	work;
	int			id_gpio;
	int			poll_ms;
	bool			last_state;
	struct power_supply	*usb_psy;
	int			low_stable_count;
	int			high_stable_count;
};

static bool x00td_usb_present(struct x00td_otg_extcon *otg)
{
	union power_supply_propval pval = { 0, };
	int rc;

	if (!otg->usb_psy)
		otg->usb_psy = power_supply_get_by_name("usb");

	if (!otg->usb_psy)
		return false;

	rc = power_supply_get_property(otg->usb_psy,
				       POWER_SUPPLY_PROP_PRESENT,
				       &pval);
	if (rc < 0)
		return false;

	return !!pval.intval;
}

static void x00td_otg_extcon_work(struct work_struct *work)
{
	struct x00td_otg_extcon *otg =
		container_of(work, struct x00td_otg_extcon, work.work);
	bool gpio_low;
	bool usb_present;
	bool new_state;

	gpio_low = (gpio_get_value(otg->id_gpio) == 0);
	usb_present = x00td_usb_present(otg);
	new_state = otg->last_state;

	/*
	 * If host is not active:
	 *  - do not enter OTG if charger is present
	 *  - require several consecutive low reads to enter OTG
	 */
	if (!otg->last_state) {
		if (usb_present) {
			otg->low_stable_count = 0;
			otg->high_stable_count = 0;
			new_state = false;
		} else if (gpio_low) {
			otg->low_stable_count++;
			otg->high_stable_count = 0;
			if (otg->low_stable_count >= 3)
				new_state = true;
		} else {
			otg->low_stable_count = 0;
			otg->high_stable_count = 0;
			new_state = false;
		}
	} else {
		/*
		 * If host is active:
		 *  - ignore usb_present (self VBUS from OTG boost)
		 *  - require several consecutive high reads to exit OTG
		 */
		if (!gpio_low) {
			otg->high_stable_count++;
			otg->low_stable_count = 0;
			if (otg->high_stable_count >= 5)
				new_state = false;
		} else {
			otg->high_stable_count = 0;
			otg->low_stable_count = 0;
			new_state = true;
		}
	}

	if (new_state != otg->last_state) {
		extcon_set_state_sync(otg->edev, EXTCON_USB_HOST, new_state);
		otg->last_state = new_state;
		dev_info(otg->dev,
			 "OTG host state -> %d (gpio_low=%d usb_present=%d low_cnt=%d high_cnt=%d)\n",
			 new_state, gpio_low, usb_present,
			 otg->low_stable_count, otg->high_stable_count);
	}

	schedule_delayed_work(&otg->work,
			      msecs_to_jiffies(otg->poll_ms));
}

static int x00td_otg_extcon_probe(struct platform_device *pdev)
{
	struct x00td_otg_extcon *otg;
	int rc;

	otg = devm_kzalloc(&pdev->dev, sizeof(*otg), GFP_KERNEL);
	if (!otg)
		return -ENOMEM;

	otg->dev = &pdev->dev;
	otg->poll_ms = X00TD_OTG_DEFAULT_POLL_MS;

	rc = of_property_read_u32(pdev->dev.of_node,
				  "qcom,poll-interval-ms",
				  &otg->poll_ms);
	if (rc < 0)
		otg->poll_ms = X00TD_OTG_DEFAULT_POLL_MS;

	otg->id_gpio = of_get_named_gpio(pdev->dev.of_node,
					 "qcom,otg-id-gpio", 0);
	if (!gpio_is_valid(otg->id_gpio)) {
		dev_err(&pdev->dev, "invalid OTG ID GPIO\n");
		return -EINVAL;
	}

	rc = devm_gpio_request_one(&pdev->dev, otg->id_gpio,
				   GPIOF_IN, "x00td_otg_id");
	if (rc < 0) {
		dev_err(&pdev->dev,
			"failed to request OTG ID GPIO rc=%d\n", rc);
		return rc;
	}

	otg->edev = devm_extcon_dev_allocate(&pdev->dev, x00td_otg_cable);
	if (IS_ERR(otg->edev))
		return PTR_ERR(otg->edev);

	rc = devm_extcon_dev_register(&pdev->dev, otg->edev);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to register extcon rc=%d\n", rc);
		return rc;
	}

	platform_set_drvdata(pdev, otg);

	INIT_DELAYED_WORK(&otg->work, x00td_otg_extcon_work);

	/* Initial state */
	otg->last_state = false;
	extcon_set_state_sync(otg->edev, EXTCON_USB_HOST, false);

	schedule_delayed_work(&otg->work, 0);

	dev_info(&pdev->dev, "X00TD OTG extcon probed gpio=%d poll=%dms\n",
		 otg->id_gpio, otg->poll_ms);

	return 0;
}

static int x00td_otg_extcon_remove(struct platform_device *pdev)
{
	struct x00td_otg_extcon *otg = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&otg->work);
	return 0;
}

static const struct of_device_id x00td_otg_extcon_match[] = {
	{ .compatible = "asus,x00td-otg-extcon", },
	{ },
};
MODULE_DEVICE_TABLE(of, x00td_otg_extcon_match);

static struct platform_driver x00td_otg_extcon_driver = {
	.probe	= x00td_otg_extcon_probe,
	.remove	= x00td_otg_extcon_remove,
	.driver	= {
		.name		= "x00td-otg-extcon",
		.of_match_table	= x00td_otg_extcon_match,
	},
};
module_platform_driver(x00td_otg_extcon_driver);

MODULE_DESCRIPTION("ASUS X00TD OTG extcon driver");
MODULE_LICENSE("GPL v2");
