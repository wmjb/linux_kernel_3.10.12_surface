/*
 * drivers/video/tegra/host/isp/isp.c
 *
 * Tegra Graphics ISP
 *
 * Copyright (c) 2012-2013, NVIDIA Corporation.  All rights reserved.
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
#include <linux/resource.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#include <mach/pm_domains.h>

#include "dev.h"
#include "bus_client.h"
#include "nvhost_acm.h"
#include "t20/t20.h"
#include "t30/t30.h"
#include "t114/t114.h"
#include "t148/t148.h"
#include "t124/t124.h"

#define T12_ISP_CG_CTRL		0x1d
#define T12_CG_2ND_LEVEL_EN	1

static struct of_device_id tegra_isp_of_match[] = {
#ifdef TEGRA_2X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra20-isp",
		.data = (struct nvhost_device_data *)&t20_isp_info },
#endif
#ifdef TEGRA_3X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra30-isp",
		.data = (struct nvhost_device_data *)&t30_isp_info },
#endif
#ifdef TEGRA_11X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra114-isp",
		.data = (struct nvhost_device_data *)&t11_isp_info },
#endif
#ifdef TEGRA_14X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra148-isp",
		.data = (struct nvhost_device_data *)&t14_isp_info },
#endif
#ifdef TEGRA_12X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra124-isp",
		.data = (struct nvhost_device_data *)&t124_isp_info },
#endif
	{ },
};

int nvhost_isp_t124_finalize_poweron(struct platform_device *pdev)
{
	nvhost_client_writel(pdev, T12_CG_2ND_LEVEL_EN, T12_ISP_CG_CTRL);
	return 0;
}

static int isp_probe(struct platform_device *dev)
{
	int err = 0;
	struct nvhost_device_data *pdata = NULL;

	if (dev->dev.of_node) {
		const struct of_device_id *match;

		match = of_match_device(tegra_isp_of_match, &dev->dev);
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

	nvhost_module_init(dev);

#ifdef CONFIG_PM_GENERIC_DOMAINS
	pdata->pd.name = "ve";

	/* add module power domain and also add its domain
	 * as sub-domain of MC domain */
	err = nvhost_module_add_domain(&pdata->pd, dev);
#endif

	err = nvhost_client_device_init(dev);
	if (err)
		return err;

	return 0;
}

static int __exit isp_remove(struct platform_device *dev)
{
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put(&dev->dev);
	pm_runtime_disable(&dev->dev);
#else
	nvhost_module_disable_clk(&dev->dev);
#endif
	return 0;
}

#ifdef CONFIG_PM
static const struct dev_pm_ops isp_pm_ops = {
	.suspend = nvhost_client_device_suspend,
	.resume = nvhost_client_device_resume,
#if defined(CONFIG_PM_RUNTIME) && !defined(CONFIG_PM_GENERIC_DOMAINS)
	.runtime_suspend = nvhost_module_disable_clk,
	.runtime_resume = nvhost_module_enable_clk,
#endif
};
#endif

static struct platform_driver isp_driver = {
	.probe = isp_probe,
	.remove = __exit_p(isp_remove),
	.driver = {
		.owner = THIS_MODULE,
		.name = "isp",
#ifdef CONFIG_PM
		.pm = &isp_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = tegra_isp_of_match,
#endif
	}
};

static int __init isp_init(void)
{
	return platform_driver_register(&isp_driver);
}

static void __exit isp_exit(void)
{
	platform_driver_unregister(&isp_driver);
}

module_init(isp_init);
module_exit(isp_exit);
