// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019-2024 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 *	 Dmitry Baryshkov <dmitry.baryshkov@linaro.org>
 * Copyright (C) 2025 Mike Jones <mike@mjones.io>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

#define NUM_SUPPLIES 2

struct sw43402_panel {
	struct drm_panel base;
	struct mipi_dsi_device *link;

	struct regulator_bulk_data supplies[NUM_SUPPLIES];

	struct gpio_desc *reset_gpio;

	struct drm_dsc_config dsc;
};

static inline struct sw43402_panel *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct sw43402_panel, base);
}

static int sw43402_unprepare(struct drm_panel *panel)
{
	struct sw43402_panel *sw43402 = to_panel_info(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = sw43402->link };
	int ret;

	mipi_dsi_dcs_set_display_off_multi(&ctx);

	mipi_dsi_dcs_enter_sleep_mode_multi(&ctx);

	mipi_dsi_msleep(&ctx, 100);

	gpiod_set_value(sw43402->reset_gpio, 1);

	ret = regulator_bulk_disable(ARRAY_SIZE(sw43402->supplies), sw43402->supplies);

	return ret ? : ctx.accum_err;
}

static int sw43402_program(struct drm_panel *panel)
{
	struct sw43402_panel *sw43402 = to_panel_info(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = sw43402->link };
	struct drm_dsc_picture_parameter_set pps;

	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb0, 0x20, 0x43);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb0, 0xa5, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb2, 0x5d, 0x01, 0x02, 0x80, 0x00, 0xff, 0xff, 0x15, 0x00, 0x00, 0x00, 0x00);

	/* MIPI_DCS_SET_TEAR_ON
	 * Downstream DTS doesn't provide a parameter here, so use raw command */
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x35);
	mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 60);

	mipi_dsi_dcs_write_seq_multi(&ctx, 0xe7,
			       0x00, 0x0d, 0x76, 0x23, 0x00, 0x00, 0x5d, 0x44,
			       0x0d, 0x76, 0x0d, 0x0d, 0x00, 0x0d, 0x0d, 0x0d,
			       0x4a, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x03, 0x77);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xed, 0x13, 0x00, 0x06, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xe2,
			       0x20, 0x0d, 0x08, 0xa8, 0x0a, 0xaa, 0x04, 0x44,
			       0x80, 0x80, 0x80, 0x5c, 0x5c, 0x5c);
	mipi_dsi_msleep(&ctx, 90);

	mipi_dsi_dcs_write_seq_multi(&ctx, 0xe7,
			       0x00, 0x0d, 0x76, 0x23, 0x00, 0x00, 0x0d, 0x44,
			       0x0d, 0x76, 0x0d, 0x0d, 0x00, 0x0d, 0x0d, 0x0d,
			       0x4a, 0x00);
	mipi_dsi_msleep(&ctx, 20);

	/* Set brightness to 0 before PPS (vendor does this) */
	mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00);

	drm_dsc_pps_payload_pack(&pps, sw43402->link->dsc);
	mipi_dsi_picture_parameter_set_multi(&ctx, &pps);

	/* TE on with V-Blank parameter (vendor sends 0x35 0x00 after PPS) */
	mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_SET_TEAR_ON, 0x00);

	mipi_dsi_dcs_set_display_on_multi(&ctx);
	mipi_dsi_msleep(&ctx, 50);

	mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x1e);
	mipi_dsi_msleep(&ctx, 50);

	return ctx.accum_err;
}

static int sw43402_prepare(struct drm_panel *panel)
{
	struct sw43402_panel *ctx = to_panel_info(panel);
	struct mipi_dsi_device *dsi = ctx->link;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	usleep_range(5000, 6000);

	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);

	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	ret = sw43402_program(panel);

	if (ret)
		goto poweroff;

	return 0;

poweroff:
	gpiod_set_value(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	return ret;
}

static const struct drm_display_mode sw43402_mode = {
	.clock = (1440 + 20 + 32 + 20) * (2880 + 20 + 4 + 20) * 60 / 1000,

	.hdisplay = 1440,
	.hsync_start = 1440 + 20,
	.hsync_end = 1440 + 20 + 32,
	.htotal = 1440 + 20 + 32 + 20,

	.vdisplay = 2880,
	.vsync_start = 2880 + 12,
	.vsync_end = 2880 + 12 + 4,
	.vtotal = 2880 + 12 + 2 + 20,

	.width_mm = 68, /* check */
	.height_mm = 136, /* check */

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int sw43402_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &sw43402_mode);
}

static int sw43402_backlight_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);

	return mipi_dsi_dcs_set_display_brightness(dsi, brightness);
}

static const struct backlight_ops sw43402_backlight_ops = {
	.update_status = sw43402_backlight_update_status,
};

static int sw43402_backlight_init(struct sw43402_panel *ctx)
{
	struct device *dev = &ctx->link->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.brightness = 30,
		.max_brightness = 255,
	};

	ctx->base.backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
							     ctx->link,
							     &sw43402_backlight_ops,
							     &props);

	if (IS_ERR(ctx->base.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->base.backlight),
				     "Failed to create backlight\n");

	return 0;
}

static const struct drm_panel_funcs sw43402_funcs = {
	.unprepare = sw43402_unprepare,
	.prepare = sw43402_prepare,
	.get_modes = sw43402_get_modes,
};

static const struct of_device_id sw43402_of_match[] = {
	{ .compatible = "lg,sw43402", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sw43402_of_match);

static int sw43402_add(struct sw43402_panel *ctx)
{
	struct device *dev = &ctx->link->dev;
	int ret;

	ctx->supplies[0].supply = "vddi"; /* 1.88 V */
	ctx->supplies[0].init_load_uA = 62000;
	ctx->supplies[1].supply = "vpnl"; /* 3.0 V */
	ctx->supplies[1].init_load_uA = 857000;

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		return dev_err_probe(dev, ret, "cannot get reset gpio\n");
	}

	ret = sw43402_backlight_init(ctx);
	if (ret < 0)
		return ret;

	ctx->base.prepare_prev_first = true;

	drm_panel_init(&ctx->base, dev, &sw43402_funcs, DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->base);
	return ret;
}

static int sw43402_probe(struct mipi_dsi_device *dsi)
{
	struct sw43402_panel *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_VIDEO_BURST;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;

	ctx->link = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = sw43402_add(ctx);
	if (ret < 0)
		return ret;

	/* The panel works only in the DSC mode. Set DSC params. */
	ctx->dsc.dsc_version_major = 0x1;
	ctx->dsc.dsc_version_minor = 0x1;

	/* slice_count * slice_width == width */
	ctx->dsc.slice_height = 16;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;

	dsi->dsc = &ctx->dsc;

	return mipi_dsi_attach(dsi);
}

static void sw43402_remove(struct mipi_dsi_device *dsi)
{
	struct sw43402_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = sw43402_unprepare(&ctx->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to unprepare panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->base);
}

static struct mipi_dsi_driver sw43402_driver = {
	.driver = {
		.name = "panel-lg-sw43402",
		.of_match_table = sw43402_of_match,
	},
	.probe = sw43402_probe,
	.remove = sw43402_remove,
};
module_mipi_dsi_driver(sw43402_driver);

MODULE_AUTHOR("Mike Jones <mike@mjones.io>");
MODULE_DESCRIPTION("LG SW436402 MIPI-DSI LED panel");
MODULE_LICENSE("GPL");
