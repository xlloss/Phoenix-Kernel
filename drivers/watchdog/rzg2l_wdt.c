// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G2L WDT Watchdog Driver
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/units.h>
#include <linux/watchdog.h>
#include <asm/system_misc.h>

#define PECR		0x10
#define PEEN		0x14
#define WDTCNT		0x00
#define WDTSET		0x04
#define WDTTIM		0x08
#define WDTINT		0x0C
#define WDTCNT_WDTEN	BIT(0)
#define WDTINT_INTDISP	BIT(0)
#define PEEN_FORCE_RST	BIT(0)

#define WDT_DEFAULT_TIMEOUT		60U

/* Setting period time register only 12 bit set in WDTSET[31:20] */
#define WDTSET_COUNTER_MASK		(0xFFF00000)
#define WDTSET_COUNTER_VAL(f)		((f) << 20)

#define F2CYCLE_NSEC(f)			(1000000000 / (f))

extern void (*arm_pm_restart)(int reboot_mode, const char *cmd);

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct rzg2l_wdt_priv {
	void __iomem *base;
	struct watchdog_device wdev;
	struct reset_control *rstc;
	unsigned long osc_clk_rate;
	unsigned long delay;
};

static void rzg2l_wdt_wait_delay(struct rzg2l_wdt_priv *priv)
{
	/* delay timer when change the setting register */
	ndelay(priv->delay);
}

static u32 rzg2l_wdt_get_cycle_usec(unsigned long cycle, u32 wdttime)
{
	u64 timer_cycle_us = 1024 * 1024 * ((u64)wdttime + 1) * MICRO;

	return div64_ul(timer_cycle_us, cycle);
}

static void rzg2l_wdt_write(struct rzg2l_wdt_priv *priv, u32 val, unsigned int reg)
{
	if (reg == WDTSET)
		val &= WDTSET_COUNTER_MASK;

	writel_relaxed(val, priv->base + reg);
	/* Registers other than the WDTINT is always synchronized with WDT_CLK */
	if (reg != WDTINT)
		rzg2l_wdt_wait_delay(priv);
}

static void rzg2l_wdt_init_timeout(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);
	u32 time_out;

	/* Clear Lapsed Time Register and clear Interrupt */
	rzg2l_wdt_write(priv, WDTINT_INTDISP, WDTINT);
	/* 2 consecutive overflow cycle needed to trigger reset */
	time_out = (wdev->timeout * (MICRO / 2)) /
		   rzg2l_wdt_get_cycle_usec(priv->osc_clk_rate, 0);
	rzg2l_wdt_write(priv, WDTSET_COUNTER_VAL(time_out), WDTSET);
}

static int rzg2l_wdt_start(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	reset_control_deassert(priv->rstc);
	udelay(35);

	pm_runtime_get_sync(wdev->parent);

	/* Initialize time out */
	rzg2l_wdt_init_timeout(wdev);

	/* Initialize watchdog counter register */
	rzg2l_wdt_write(priv, 0, WDTTIM);

	/* Enable watchdog timer*/
	rzg2l_wdt_write(priv, WDTCNT_WDTEN, WDTCNT);

	return 0;
}

static int rzg2l_wdt_stop(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	pm_runtime_put(wdev->parent);
	reset_control_assert(priv->rstc);

	return 0;
}

static int rzg2l_wdt_restart(struct watchdog_device *wdev,
			     unsigned long action, void *data)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	/* Reset the module before we modify any register */
	reset_control_reset(priv->rstc);
	udelay(35);

	pm_runtime_get_sync(wdev->parent);

	/* smallest counter value to reboot soon */
	rzg2l_wdt_write(priv, 0, PECR);

	/* Enable watchdog timer*/
	rzg2l_wdt_write(priv, PEEN_FORCE_RST, PEEN);

	return 0;
}

static const struct watchdog_info rzg2l_wdt_ident = {
	.options = WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
	.identity = "Renesas RZ/G2L WDT Watchdog",
};

static int rzg2l_wdt_ping(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	rzg2l_wdt_write(priv, WDTINT_INTDISP, WDTINT);

	return 0;
}

static const struct watchdog_ops rzg2l_wdt_ops = {
	.owner = THIS_MODULE,
	.start = rzg2l_wdt_start,
	.stop = rzg2l_wdt_stop,
	.ping = rzg2l_wdt_ping,
	.restart = rzg2l_wdt_restart,
};

static void rzg2l_wdt_reset_assert_pm_disable_put(void *data)
{
	struct watchdog_device *wdev = data;
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	pm_runtime_put(wdev->parent);
	pm_runtime_disable(wdev->parent);
	reset_control_assert(priv->rstc);
}

static int rzg2l_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rzg2l_wdt_priv *priv;
	unsigned long pclk_rate;
	struct clk *wdt_clk;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	/* Get watchdog main clock */
	wdt_clk = clk_get(&pdev->dev, "oscclk");
	if (IS_ERR(wdt_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(wdt_clk), "no oscclk");

	priv->osc_clk_rate = clk_get_rate(wdt_clk);
	clk_put(wdt_clk);
	if (!priv->osc_clk_rate)
		return dev_err_probe(&pdev->dev, -EINVAL, "oscclk rate is 0");

	/* Get Peripheral clock */
	wdt_clk = clk_get(&pdev->dev, "pclk");
	if (IS_ERR(wdt_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(wdt_clk), "no pclk");

	pclk_rate = clk_get_rate(wdt_clk);
	clk_put(wdt_clk);
	if (!pclk_rate)
		return dev_err_probe(&pdev->dev, -EINVAL, "pclk rate is 0");

	priv->delay = F2CYCLE_NSEC(priv->osc_clk_rate) * 6 + F2CYCLE_NSEC(pclk_rate) * 9;

	priv->rstc = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(priv->rstc))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->rstc),
				     "failed to get cpg reset");

	reset_control_deassert(priv->rstc);
	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_resume_and_get failed ret=%pe", ERR_PTR(ret));
		goto out_pm_get;
	}

	priv->wdev.info = &rzg2l_wdt_ident;
	priv->wdev.ops = &rzg2l_wdt_ops;
	priv->wdev.parent = dev;
	priv->wdev.min_timeout = 1;
	priv->wdev.max_timeout = rzg2l_wdt_get_cycle_usec(priv->osc_clk_rate, 0xfff) /
				 USEC_PER_SEC;
	priv->wdev.timeout = WDT_DEFAULT_TIMEOUT;

	watchdog_set_drvdata(&priv->wdev, priv);
	ret = devm_add_action_or_reset(&pdev->dev,
				       rzg2l_wdt_reset_assert_pm_disable_put,
				       &priv->wdev);
	if (ret < 0)
		return ret;

	watchdog_set_nowayout(&priv->wdev, nowayout);
	watchdog_stop_on_unregister(&priv->wdev);

	/*
	 * TODO:
	 * Reboot code is specific controlled by PSCI for ARM64.
	 * However, we do not support reboot code in TF-A currently.
	 * Will update when TF-A support reboot API.
	 */
	arm_pm_restart = NULL;
	watchdog_set_restart_priority(&priv->wdev, 0);

	ret = watchdog_init_timeout(&priv->wdev, 0, dev);
	if (ret)
		dev_warn(dev, "Specified timeout invalid, using default");

	return devm_watchdog_register_device(&pdev->dev, &priv->wdev);

out_pm_get:
	pm_runtime_disable(dev);
	reset_control_assert(priv->rstc);

	return ret;
}

static const struct of_device_id rzg2l_wdt_ids[] = {
	{ .compatible = "renesas,rzg2l-wdt", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzg2l_wdt_ids);

static struct platform_driver rzg2l_wdt_driver = {
	.driver = {
		.name = "rzg2l_wdt",
		.of_match_table = rzg2l_wdt_ids,
	},
	.probe = rzg2l_wdt_probe,
};
module_platform_driver(rzg2l_wdt_driver);

MODULE_DESCRIPTION("Renesas RZ/G2L WDT Watchdog Driver");
MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_LICENSE("GPL v2");
