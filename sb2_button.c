/*
 * Supports for Surface Book 2 and Surface Pro (2017) power and volume
 * buttons.
 *
 * Based on soc_button_array.c:
 *
 * (C) Copyright 2014 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio_keys.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>

struct soc_button_info {
	const char *name;
	int acpi_index;
	unsigned int event_type;
	unsigned int event_code;
	bool autorepeat;
	bool wakeup;
};

struct soc_device_data {
	int (*check)(struct device *);
	struct soc_button_info *button_info;
};

/*
 * Some of the buttons like volume up/down are auto repeat, while others
 * are not. To support both, we register two platform devices, and put
 * buttons into them based on whether the key should be auto repeat.
 */
#define BUTTON_TYPES			2
#define SURFACE_METHOD_DSM		"_DSM"

struct soc_button_data {
	struct platform_device *children[BUTTON_TYPES];
};

/*
 * Get the Nth GPIO number from the ACPI object.
 */
static int soc_button_lookup_gpio(struct device *dev, int acpi_index)
{
	struct gpio_desc *desc;
	int gpio;

	desc = gpiod_get_index(dev, NULL, acpi_index, GPIOD_ASIS);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	gpio = desc_to_gpio(desc);

	gpiod_put(desc);

	return gpio;
}

static struct platform_device *
soc_button_device_create(struct platform_device *pdev,
			 const struct soc_button_info *button_info,
			 bool autorepeat)
{
	const struct soc_button_info *info;
	struct platform_device *pd;
	struct gpio_keys_button *gpio_keys;
	struct gpio_keys_platform_data *gpio_keys_pdata;
	int n_buttons = 0;
	int gpio;
	int error;

	for (info = button_info; info->name; info++)
		if (info->autorepeat == autorepeat)
			n_buttons++;

	gpio_keys_pdata = devm_kzalloc(&pdev->dev,
				       sizeof(*gpio_keys_pdata) +
					sizeof(*gpio_keys) * n_buttons,
				       GFP_KERNEL);
	if (!gpio_keys_pdata)
		return ERR_PTR(-ENOMEM);

	gpio_keys = (void *)(gpio_keys_pdata + 1);
	n_buttons = 0;

	for (info = button_info; info->name; info++) {
		if (info->autorepeat != autorepeat)
			continue;

		gpio = soc_button_lookup_gpio(&pdev->dev, info->acpi_index);
		if (!gpio_is_valid(gpio))
			continue;

		gpio_keys[n_buttons].type = info->event_type;
		gpio_keys[n_buttons].code = info->event_code;
		gpio_keys[n_buttons].gpio = gpio;
		gpio_keys[n_buttons].active_low = 1;
		gpio_keys[n_buttons].desc = info->name;
		gpio_keys[n_buttons].wakeup = info->wakeup;
		/* These devices often use cheap buttons, use 50 ms debounce */
		gpio_keys[n_buttons].debounce_interval = 50;
		n_buttons++;
	}

	if (n_buttons == 0) {
		error = -ENODEV;
		goto err_free_mem;
	}

	gpio_keys_pdata->buttons = gpio_keys;
	gpio_keys_pdata->nbuttons = n_buttons;
	gpio_keys_pdata->rep = autorepeat;

	pd = platform_device_alloc("gpio-keys", PLATFORM_DEVID_AUTO);
	if (!pd) {
		error = -ENOMEM;
		goto err_free_mem;
	}

	error = platform_device_add_data(pd, gpio_keys_pdata,
					 sizeof(*gpio_keys_pdata));
	if (error)
		goto err_free_pdev;

	error = platform_device_add(pd);
	if (error)
		goto err_free_pdev;

	return pd;

err_free_pdev:
	platform_device_put(pd);
err_free_mem:
	devm_kfree(&pdev->dev, gpio_keys_pdata);
	return ERR_PTR(error);
}

static int soc_button_remove(struct platform_device *pdev)
{
	struct soc_button_data *priv = platform_get_drvdata(pdev);

	int i;

	for (i = 0; i < BUTTON_TYPES; i++)
		if (priv->children[i])
			platform_device_unregister(priv->children[i]);

	return 0;
}

static int soc_button_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct acpi_device_id *id;
	struct soc_device_data *device_data;
	struct soc_button_info *button_info = NULL;
	struct soc_button_data *priv;
	struct platform_device *pd;
	int i;
	int error;

	id = acpi_match_device(dev->driver->acpi_match_table, dev);
	if (!id)
		return -ENODEV;

	device_data = (struct soc_device_data *)id->driver_data;
	if (device_data) {
		// device dependent check, required for MSHW0040
		error = device_data->check(dev);
		if (error < 0)
			return error;

		button_info = device_data->button_info;
	}

	error = gpiod_count(dev, NULL);
	if (error < 0) {
		dev_dbg(dev, "no GPIO attached, ignoring...\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	for (i = 0; i < BUTTON_TYPES; i++) {
		pd = soc_button_device_create(pdev, button_info, i == 0);
		if (IS_ERR(pd)) {
			error = PTR_ERR(pd);
			if (error != -ENODEV) {
				soc_button_remove(pdev);
				return error;
			}
			continue;
		}

		priv->children[i] = pd;
	}

	if (!priv->children[0] && !priv->children[1])
		return -ENODEV;

	if (!id->driver_data)
		devm_kfree(dev, button_info);

	return 0;
}

static int soc_device_check_MSHW0040(struct device *dev)
{
	if (!acpi_has_method(ACPI_HANDLE(dev), SURFACE_METHOD_DSM))
		return -ENODEV;
	
	return 0;
}

static struct soc_button_info soc_button_MSHW0040[] = {
	{ "power",       0, EV_KEY, KEY_POWER,      false, true  },
	{ "volume_up",   2, EV_KEY, KEY_VOLUMEUP,   true,  false },
	{ "volume_down", 4, EV_KEY, KEY_VOLUMEDOWN, true,  false },
	{ }
};

static struct soc_device_data soc_device_MSHW0040 = {
	.check = soc_device_check_MSHW0040,
	.button_info = soc_button_MSHW0040,
};

static const struct acpi_device_id soc_button_acpi_match[] = {
	{ "MSHW0040", (unsigned long)&soc_device_MSHW0040 },
	{ }
};

MODULE_DEVICE_TABLE(acpi, soc_button_acpi_match);

static struct platform_driver soc_button_driver_sb2 = {
	.probe          = soc_button_probe,
	.remove		= soc_button_remove,
	.driver		= {
		.name = "SurfaceBook2 Button",
		.acpi_match_table = ACPI_PTR(soc_button_acpi_match),
	},
};
module_platform_driver(soc_button_driver_sb2);

MODULE_AUTHOR("Maximilian Luz");
MODULE_DESCRIPTION("Surface Book 2/Surface Pro (2017) Button Driver");
MODULE_LICENSE("GPL v2");
