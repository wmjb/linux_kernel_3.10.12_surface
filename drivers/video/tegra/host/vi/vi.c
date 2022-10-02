/*
 * drivers/video/tegra/host/vi/vi.c
 *
 * Tegra Graphics Host VI
 *
 * Copyright (c) 2012-2013, NVIDIA CORPORATION. All rights reserved.
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

#include <linux/export.h>
#include <linux/module.h>
#include <linux/resource.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/nvhost_vi_ioctl.h>
#include <linux/clk/tegra.h>

#include <mach/pm_domains.h>
#include <media/tegra_v4l2_camera.h>

#include "dev.h"
#include "bus_client.h"
#include "nvhost_acm.h"
#include "t20/t20.h"
#include "t30/t30.h"
#include "t114/t114.h"
#include "t148/t148.h"
#include "t124/t124.h"
#include "vi.h"

#define MAX_DEVID_LENGTH	16
#define T12_VI_CFG_CG_CTRL	0x2e
#define T12_CG_2ND_LEVEL_EN	1
#define T12_VI_CSI_0_SW_RESET	0x40
#define T12_VI_CSI_1_SW_RESET	0x80
#define T12_VI_CSI_SW_RESET_MCCIF_RESET 3

static struct of_device_id tegra_vi_of_match[] = {
#ifdef TEGRA_2X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra20-vi",
		.data = (struct nvhost_device_data *)&t20_vi_info },
#endif
#ifdef TEGRA_3X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra30-vi",
		.data = (struct nvhost_device_data *)&t30_vi_info },
#endif
#ifdef TEGRA_11X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra114-vi",
		.data = (struct nvhost_device_data *)&t11_vi_info },
#endif
#ifdef TEGRA_14X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra148-vi",
		.data = (struct nvhost_device_data *)&t14_vi_info },
#endif
#ifdef TEGRA_12X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra124-vi",
		.data = (struct nvhost_device_data *)&t124_vi_info },
#endif
	{ },
};

static struct i2c_camera_ctrl *i2c_ctrl;

static int vi_probe(struct platform_device *dev)
{
	int err = 0;
	struct vi *tegra_vi;
	struct nvhost_device_data *pdata = NULL;

	if (dev->dev.of_node) {
		const struct of_device_id *match;

		match = of_match_device(tegra_vi_of_match, &dev->dev);
		if (match)
			pdata = (struct nvhost_device_data *)match->data;
	} else
		pdata = (struct nvhost_device_data *)dev->dev.platform_data;

	WARN_ON(!pdata);
	if (!pdata) {
		dev_info(&dev->dev, "no platform data\n");
		return -ENODATA;
	}

	pdata->pdev = dev;
	mutex_init(&pdata->lock);
	platform_set_drvdata(dev, pdata);

	err = nvhost_client_device_get_resources(dev);
	if (err)
		return err;

	i2c_ctrl = pdata->private_data;

	dev_info(&dev->dev, "%s: ++\n", __func__);

	tegra_vi = kzalloc(sizeof(struct vi), GFP_KERNEL);
	if (!tegra_vi) {
		dev_err(&dev->dev, "can't allocate memory for vi\n");
		return -ENOMEM;
	}

	tegra_vi->ndev = dev;
	pdata->private_data = tegra_vi;

	/* Create I2C Devices according to settings from board file */
	if (i2c_ctrl && i2c_ctrl->new_devices)
		i2c_ctrl->new_devices(dev);

#ifdef CONFIG_TEGRA_CAMERA
	tegra_vi->camera = tegra_camera_register(dev);
	if (!tegra_vi->camera) {
		dev_err(&dev->dev, "%s: can't register tegra_camera\n",
				__func__);
		goto camera_i2c_unregister;
	}
#endif

	nvhost_module_init(dev);

#ifdef CONFIG_PM_GENERIC_DOMAINS
	pdata->pd.name = "ve";

	/* add module power domain and also add its domain
	 * as sub-domain of MC domain */
	err = nvhost_module_add_domain(&pdata->pd, dev);
#endif

	err = nvhost_client_device_init(dev);
	if (err)
		goto camera_unregister;

	return 0;

camera_unregister:
#ifdef CONFIG_TEGRA_CAMERA
	tegra_camera_unregister(tegra_vi->camera);
camera_i2c_unregister:
#endif
	if (i2c_ctrl && i2c_ctrl->remove_devices)
		i2c_ctrl->remove_devices(dev);
	pdata->private_data = i2c_ctrl;
	kfree(tegra_vi);
	return err;
}

static int __exit vi_remove(struct platform_device *dev)
{
#ifdef CONFIG_TEGRA_CAMERA
	int err = 0;
#endif
	struct nvhost_device_data *pdata =
		(struct nvhost_device_data *)platform_get_drvdata(dev);
	struct vi *tegra_vi = (struct vi *)pdata->private_data;

	dev_info(&dev->dev, "%s: ++\n", __func__);

	nvhost_client_device_release(dev);

#ifdef CONFIG_TEGRA_CAMERA
	err = tegra_camera_unregister(tegra_vi->camera);
	if (err)
		return err;
#endif

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put(&dev->dev);
	pm_runtime_disable(&dev->dev);
#else
	nvhost_module_disable_clk(&dev->dev);
#endif
	/* Remove I2C Devices according to settings from board file */
	if (i2c_ctrl && i2c_ctrl->remove_devices)
		i2c_ctrl->remove_devices(dev);

	pdata->private_data = i2c_ctrl;
	kfree(tegra_vi);

	return 0;
}

#ifdef CONFIG_PM
static int vi_suspend(struct device *dev)
{
#ifdef CONFIG_TEGRA_CAMERA
	struct platform_device *pdev = to_platform_device(dev);
	struct nvhost_device_data *pdata =
		(struct nvhost_device_data *)platform_get_drvdata(pdev);
	struct vi *tegra_vi = (struct vi *)pdata->private_data;
	int ret;
#endif

	dev_info(dev, "%s: ++\n", __func__);

#ifdef CONFIG_TEGRA_CAMERA
	ret = tegra_camera_suspend(tegra_vi->camera);
	if (ret) {
		dev_info(dev, "%s: tegra_camera_suspend error=%d\n",
		__func__, ret);
		return ret;
	}
#endif

	return nvhost_client_device_suspend(dev);
}

static int vi_resume(struct device *dev)
{
#ifdef CONFIG_TEGRA_CAMERA
	struct platform_device *pdev = to_platform_device(dev);
	struct nvhost_device_data *pdata =
		(struct nvhost_device_data *)platform_get_drvdata(pdev);
	struct vi *tegra_vi = (struct vi *)pdata->private_data;
#endif

	dev_info(dev, "%s: ++\n", __func__);

#ifdef CONFIG_TEGRA_CAMERA
	tegra_camera_resume(tegra_vi->camera);
#endif

	return 0;
}

static const struct dev_pm_ops vi_pm_ops = {
	.suspend = vi_suspend,
	.resume = vi_resume,
#if defined(CONFIG_PM_RUNTIME) && !defined(CONFIG_PM_GENERIC_DOMAINS)
	.runtime_suspend = nvhost_module_disable_clk,
	.runtime_resume = nvhost_module_enable_clk,
#endif
};
#endif

static struct platform_driver vi_driver = {
	.probe = vi_probe,
	.remove = __exit_p(vi_remove),
	.driver = {
		.owner = THIS_MODULE,
		.name = "vi",
#ifdef CONFIG_PM
		.pm = &vi_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = tegra_vi_of_match,
#endif
	}
};

static int __init vi_init(void)
{
	return platform_driver_register(&vi_driver);
}

static void __exit vi_exit(void)
{
	platform_driver_unregister(&vi_driver);
}

#ifdef TEGRA_12X_OR_HIGHER_CONFIG
int nvhost_vi_init(struct platform_device *dev)
{
	int ret = 0;
	struct vi *tegra_vi;
	tegra_vi = (struct vi *)nvhost_get_private_data(dev);

	tegra_vi->reg = regulator_get(&dev->dev, "avdd_dsi_csi");
	if (IS_ERR(tegra_vi->reg)) {
		if (tegra_vi->reg == ERR_PTR(-ENODEV)) {
			ret = -ENODEV;
			dev_info(&dev->dev,
				"%s: no regulator device\n",
				__func__);
		} else {
			dev_err(&dev->dev,
				"%s: couldn't get regulator\n",
				__func__);
		}
		tegra_vi->reg = NULL;
	}
	return ret;
}

void nvhost_vi_deinit(struct platform_device *dev)
{
	struct vi *tegra_vi;
	tegra_vi = (struct vi *)nvhost_get_private_data(dev);

	if (tegra_vi->reg)
		regulator_put(tegra_vi->reg);
}

int nvhost_vi_finalize_poweron(struct platform_device *dev)
{
	int ret = 0;
	struct vi *tegra_vi;
	tegra_vi = (struct vi *)nvhost_get_private_data(dev);

	if (tegra_vi->reg) {
		ret = regulator_enable(tegra_vi->reg);
		if (ret)
			dev_err(&dev->dev,
				"%s: enable csi regulator failed.\n",
				__func__);
	}

	if (nvhost_client_can_writel(dev))
		nvhost_client_writel(dev,
				T12_CG_2ND_LEVEL_EN, T12_VI_CFG_CG_CTRL);
	return ret;
}

int nvhost_vi_prepare_poweroff(struct platform_device *dev)
{
	int ret = 0;
	struct vi *tegra_vi;
	tegra_vi = (struct vi *)nvhost_get_private_data(dev);

	if (tegra_vi->reg) {
		ret = regulator_disable(tegra_vi->reg);
		if (ret)
			dev_err(&dev->dev,
				"%s: disable csi regulator failed.\n",
				__func__);
	}
	return ret;
}

long tegra_vi_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	struct vi *tegra_vi;

	if (_IOC_TYPE(cmd) != NVHOST_VI_IOCTL_MAGIC)
		return -EFAULT;

	tegra_vi = file->private_data;
	switch (cmd) {
	case NVHOST_VI_IOCTL_ENABLE_TPG: {
		uint enable;
		int ret;
		struct clk *clk;

		if (copy_from_user(&enable,
			(const void __user *)arg, sizeof(uint))) {
			dev_err(&tegra_vi->ndev->dev,
				"%s: Failed to copy arg from user\n", __func__);
			return -EFAULT;
		}

		clk = clk_get(&tegra_vi->ndev->dev, "pll_d");
		if (enable)
			ret = tegra_clk_cfg_ex(clk,
				TEGRA_CLK_PLLD_CSI_OUT_ENB, 1);
		else
			ret = tegra_clk_cfg_ex(clk,
				TEGRA_CLK_MIPI_CSI_OUT_ENB, 1);
		clk_put(clk);

		return ret;
	}
	default:
		dev_err(&tegra_vi->ndev->dev,
			"%s: Unknown vi ioctl.\n", __func__);
		return -EINVAL;
	}
	return 0;
}

static int tegra_vi_open(struct inode *inode, struct file *file)
{
	struct nvhost_device_data *pdata;
	struct vi *vi;

	pdata = container_of(inode->i_cdev,
		struct nvhost_device_data, ctrl_cdev);
	BUG_ON(pdata == NULL);

	vi = (struct vi *)pdata->private_data;
	BUG_ON(vi == NULL);

	file->private_data = vi;
	return 0;
}

static int tegra_vi_release(struct inode *inode, struct file *file)
{
	return 0;
}

const struct file_operations tegra_vi_ctrl_ops = {
	.owner = THIS_MODULE,
	.open = tegra_vi_open,
	.unlocked_ioctl = tegra_vi_ioctl,
	.release = tegra_vi_release,
};
#endif

void nvhost_vi_reset(struct platform_device *pdev)
{
	u32 reset_reg;

	if (pdev->id == 0)
		reset_reg = T12_VI_CSI_0_SW_RESET;
	else
		reset_reg = T12_VI_CSI_1_SW_RESET;

	nvhost_client_writel(pdev, T12_VI_CSI_SW_RESET_MCCIF_RESET,
			reset_reg);

	udelay(10);

	nvhost_client_writel(pdev, 0, reset_reg);
}

late_initcall(vi_init);
module_exit(vi_exit);
MODULE_LICENSE("GPL v2");
