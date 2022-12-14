/*
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/lm3528.h>
#include <linux/regmap.h>
#include <linux/edp.h>

#define LM3528_BL_MAX_CURR	0x7F

/* I2C registers */
#define LM3528_GP		0x10
#define LM3528_GP_ENM		(1 << 0)
#define LM3528_GP_ENS		(1 << 1)
#define LM3528_GP_UNI		(1 << 2)
#define LM3528_GP_RMP0		(1 << 3)
#define LM3528_GP_RMP1		(1 << 4)
#define LM3528_GP_OLED		(1 << 5)

#define	LM3528_BMAIN		0xA0
#define LM3528_BSUB		0xB0
#define LM3528_HPG		0x80
#define LM3528_GPIO		0x81
#define	LM3528_PGEN0		0x90
#define	LM3528_PGEN1		0x91
#define	LM3528_PGEN2		0x92
#define	LM3528_PGEN3		0x93

struct lm3528_backlight_data {
	struct device		*lm3528_dev;
	struct regmap		*regmap;
	struct i2c_client	*client;
	struct regulator	*regulator;
	struct backlight_device *bl;
	bool (*is_powered)(void);
	int (*notify)(struct device *dev, int brightness);
	int			current_brightness;
	struct edp_client *lm3528_edp_client;
	int *edp_brightness_states;
};

enum chips { lm3528 };

static const struct i2c_device_id lm3528_id[] = {
	{ "lm3528_display_bl", lm3528 },
	{},
};
MODULE_DEVICE_TABLE(i2c, lm3528_id);

static const struct regmap_config lm3528_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int lm3528_backlight_set(struct backlight_device *bl, int brightness)
{
	struct lm3528_backlight_data *data = bl_get_data(bl);

	if (data->is_powered)
		if (!data->is_powered()) {
			pr_err("%s skipped as panel is not powered\n",
				__func__);
			/* do not report error else suspend fails */
			return 0;
		}

	if (data->notify)
		brightness = data->notify(data->lm3528_dev, brightness);

	data->current_brightness = brightness;

	/* normalize the brightness value [0-255] to lm3528 range */
	brightness = brightness * LM3528_BL_MAX_CURR / 255;

	regmap_update_bits(data->regmap, LM3528_GP,
				LM3528_GP_ENM | LM3528_GP_UNI,
				LM3528_GP_ENM | LM3528_GP_UNI);

	return regmap_write(data->regmap, LM3528_BMAIN, brightness);
}

static int lm3528_backlight_set_with_edp(struct backlight_device *bl,
	int brightness)
{
	struct lm3528_backlight_data *data = bl_get_data(bl);
	struct device *dev = data->lm3528_dev;
	unsigned int approved;
	int ret;
	unsigned int edp_state;
	unsigned int i;
	if (data->lm3528_edp_client) {
		for (i = 0; i < LM3528_EDP_NUM_STATES; i++) {
			if (brightness >= data->edp_brightness_states[i])
				break;
		}
		edp_state = i;
		ret = edp_update_client_request(data->lm3528_edp_client,
							edp_state, &approved);
		if (ret) {
			dev_err(dev, "E state transition failed\n");
			return ret;
		}
	}

	lm3528_backlight_set(bl, data->edp_brightness_states[approved]);
	return 0;
}

static int lm3528_backlight_update_status(struct backlight_device *bl)
{
	int brightness = bl->props.brightness;
	return lm3528_backlight_set_with_edp(bl, brightness);
}

static void lm3528_backlight_edpcb(unsigned int new_state, void *priv_data)
{
	struct backlight_device *bl_device =
		(struct backlight_device *) priv_data;
	struct lm3528_backlight_data *data = bl_get_data(bl_device);
	lm3528_backlight_set(bl_device, data->edp_brightness_states[new_state]);
}

static int lm3528_backlight_get_brightness(struct backlight_device *bl)
{
	struct lm3528_backlight_data *data = bl_get_data(bl);
	return data->current_brightness;
}

static const struct backlight_ops lm3528_backlight_ops = {
	.update_status	= lm3528_backlight_update_status,
	.get_brightness	= lm3528_backlight_get_brightness,
};

static int lm3528_bl_probe(struct i2c_client *i2c,
	const struct i2c_device_id *id)
{
	struct lm3528_backlight_data *data;
	struct backlight_device *bl;
	struct backlight_properties props;
	struct lm3528_platform_data *pData = i2c->dev.platform_data;
	struct edp_manager *battery_manager = NULL;
	int ret;

	data = devm_kzalloc(&i2c->dev, sizeof(*data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	data->client = i2c;
	data->lm3528_dev = &i2c->dev;
	data->current_brightness = 0;
	data->notify = pData->notify;
	data->is_powered = pData->is_powered;
	data->edp_brightness_states = pData->edp_brightness;
	data->regulator = regulator_get(data->lm3528_dev, "vin");
	if (IS_ERR(data->regulator)) {
		dev_err(&i2c->dev, "%s: failed to get regulator\n", __func__);
		data->regulator = NULL;
	} else {
		regulator_enable(data->regulator);
	}

	data->regmap = devm_regmap_init_i2c(i2c, &lm3528_regmap_config);
	if (IS_ERR(data->regmap)) {
		ret = PTR_ERR(data->regmap);
		dev_err(&i2c->dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 255;
	bl = backlight_device_register(i2c->name, data->lm3528_dev, data,
				       &lm3528_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&i2c->dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

	data->lm3528_edp_client = devm_kzalloc(&i2c->dev,
					sizeof(struct edp_client), GFP_KERNEL);
	if (IS_ERR_OR_NULL(data->lm3528_edp_client)) {
		dev_err(&i2c->dev, "could not allocate edp client\n");
		return PTR_ERR(data->lm3528_edp_client);
	}

	strncpy(data->lm3528_edp_client->name, "backlight", EDP_NAME_LEN - 1);
	data->lm3528_edp_client->name[EDP_NAME_LEN - 1] = '\0';
	data->lm3528_edp_client->states = pData->edp_states;
	data->lm3528_edp_client->num_states = LM3528_EDP_NUM_STATES;
	data->lm3528_edp_client->e0_index = LM3528_EDP_ZERO;
	data->lm3528_edp_client->private_data = bl;
	data->lm3528_edp_client->priority = EDP_MAX_PRIO + 2;
	data->lm3528_edp_client->throttle = lm3528_backlight_edpcb;
	data->lm3528_edp_client->notify_promotion = lm3528_backlight_edpcb;

	battery_manager = edp_get_manager("battery");
	if (!battery_manager) {
		dev_err(&i2c->dev, "unable to get edp manager\n");
	} else {
		ret = edp_register_client(battery_manager,
					data->lm3528_edp_client);
		if (ret) {
			dev_err(&i2c->dev, "unable to register edp client\n");
		} else {
			ret = edp_update_client_request(
					data->lm3528_edp_client,
						LM3528_EDP_ZERO, NULL);
			if (ret) {
				dev_err(&i2c->dev,
					"unable to set E0 EDP state\n");
				edp_unregister_client(data->lm3528_edp_client);
			} else {
				goto edp_success;
			}
		}
	}

	devm_kfree(&i2c->dev, data->lm3528_edp_client);
	data->lm3528_edp_client = NULL;

edp_success:
	data->bl = bl;
	bl->props.brightness = pData->dft_brightness;

	i2c_set_clientdata(i2c, data);
	backlight_update_status(bl);
	return 0;
}

static int lm3528_bl_remove(struct i2c_client *cl)
{
	struct lm3528_backlight_data *data = i2c_get_clientdata(cl);
	data->bl->props.brightness = 0;
	backlight_update_status(data->bl);
	backlight_device_unregister(data->bl);
	return 0;
}

#ifdef CONFIG_PM
static int lm3528_bl_suspend(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct lm3528_backlight_data *data = i2c_get_clientdata(i2c);
	struct backlight_device *bl = data->bl;

	return lm3528_backlight_set_with_edp(bl, 0);
}

static int lm3528_bl_resume(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct lm3528_backlight_data *data = i2c_get_clientdata(i2c);
	struct backlight_device *bl = data->bl;

	backlight_update_status(bl);
	return 0;
}

static const struct dev_pm_ops lm3528_bl_pm_ops = {
	.suspend	= lm3528_bl_suspend,
	.resume		= lm3528_bl_resume,
};
#endif

static struct i2c_driver lm3528_bl_driver = {
	.driver = {
		.name	= "lm3528_display_bl",
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM
		.pm	= &lm3528_bl_pm_ops,
#endif
	},
	.id_table = lm3528_id,
	.probe	= lm3528_bl_probe,
	.remove	= lm3528_bl_remove,
};
module_i2c_driver(lm3528_bl_driver);

MODULE_AUTHOR("Chaitanya Bandi <bandik@nvidia.com>");
MODULE_DESCRIPTION("LM3528 Backlight display driver");
MODULE_LICENSE("GPL v2");
