// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based s6e3hc2 AMOLED 5.7 inch panel driver.
 *
 * Copyright (c) 2025 Mike Jones <mike@mjones.org>
 */

#include "video/mipi_display.h"
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define NUM_SUPPLIES 2

#define S6E3HC2_BRIGHTNESS_MIN 0x4
#define S6E3HC2_BRIGHTNESS_MAX 0x3ff

struct s6e3hc2_panel {
	struct drm_panel base;
	struct mipi_dsi_device *link;

	struct regulator_bulk_data supplies[NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;

	struct backlight_device *backlight;
	bool prepared;
};

static const struct drm_display_mode s6e3hc2_mode_90hz = {
	.clock = (1080 + 32 + 12 + 16) * (2340 + 12 + 4 + 16) * 90 / 1000,

	.hdisplay = 1080,
	.hsync_start = 1080 + 32,
	.hsync_end = 1080 + 32 + 12,
	.htotal = 1080 + 32 + 12 + 16,

	.vdisplay = 2340,
	.vsync_start = 2340 + 12,
	.vsync_end = 2340 + 12 + 4,
	.vtotal = 2340 + 12 + 4 + 16,

	.width_mm = 63,
	.height_mm = 137,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static struct drm_dsc_config s6e3hc2_dsc_config = {
	.dsc_version_major = 1,
	.dsc_version_minor = 1,
	.slice_height = 65,
	.slice_width = 540,
	.slice_count = 2,
	.bits_per_component = 8,
	.bits_per_pixel = 8 << 4,
	.block_pred_enable = true,
};

static inline struct s6e3hc2_panel *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct s6e3hc2_panel, base);
}

static int s6e3hc2_bl_update_status(struct backlight_device *bl)
{
	struct s6e3hc2_panel *ctx = bl_get_data(bl);
	struct mipi_dsi_device *dsi = ctx->link;
	u16 brightness;

	if (!ctx->prepared)
		return 0;

	brightness = backlight_get_brightness(bl);

	if (brightness)
		brightness = S6E3HC2_BRIGHTNESS_MIN +
			DIV_ROUND_UP(brightness * (S6E3HC2_BRIGHTNESS_MAX - S6E3HC2_BRIGHTNESS_MIN),
				     bl->props.max_brightness);

	return mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
}

static int s6e3hc2_bl_get_brightness(struct backlight_device *bl)
{
	struct s6e3hc2_panel *ctx = bl_get_data(bl);
	struct mipi_dsi_device *dsi = ctx->link;
	u16 brightness;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	return brightness;
}

static const struct backlight_ops s6e3hc2_bl_ops = {
	.update_status = s6e3hc2_bl_update_status,
	.get_brightness = s6e3hc2_bl_get_brightness,
};

static int s6e3hc2_unprepare(struct drm_panel *panel)
{
	struct s6e3hc2_panel *ctx = to_panel_info(panel);
	struct mipi_dsi_device *dsi = ctx->link;
	struct mipi_dsi_multi_context multi_ctx = { .dsi = dsi };

	ctx->prepared = false;

	mipi_dsi_dcs_set_display_off_multi(&multi_ctx);
	usleep_range(10000, 11000);
	mipi_dsi_dcs_enter_sleep_mode_multi(&multi_ctx);
	usleep_range(120000, 130000);

	gpiod_set_value(ctx->reset_gpio, 1);

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return multi_ctx.accum_err;
}

static int s6e3hc2_prepare(struct drm_panel *panel)
{
	struct s6e3hc2_panel *ctx = to_panel_info(panel);
	struct mipi_dsi_device *dsi = ctx->link;
	struct mipi_dsi_multi_context multi_ctx = { .dsi = dsi };
	int ret;

	/* Power on */
	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;
	usleep_range(50000, 51000);

	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);

	mipi_dsi_compression_mode_multi(&multi_ctx, true);

	mipi_dsi_dcs_exit_sleep_mode_multi(&multi_ctx);
	usleep_range(120000, 121000);

	/* write ctl 90hz */
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x38);

	mipi_dsi_dcs_set_tear_on_multi(&multi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xfc, 0x5a, 0x5a);

	/* Set Dimming frames to 8 */
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xb0, 0x05, 0xb1);
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xb1, 0x08);

	/* Dither IP On */
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xb0, 0x86, 0xeb);
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xeb, 0x01);

	/* TSP HSYNC Enable */
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xb0, 0x07, 0xb9);
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xb9, 0x11, 0x03);

	/* TOUT Enable */
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xff, 0x00, 0x01);

	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xf1, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&multi_ctx, 0xfc, 0xa5, 0xa5);

	mipi_dsi_dcs_set_display_on_multi(&multi_ctx);

	struct drm_dsc_picture_parameter_set pps;
	drm_dsc_pps_payload_pack(&pps, &s6e3hc2_dsc_config);
	mipi_dsi_picture_parameter_set_multi(&multi_ctx, &pps);

	if (multi_ctx.accum_err) {
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		gpiod_set_value(ctx->reset_gpio, 1);
		return multi_ctx.accum_err;
	}

	ctx->prepared = true;

	if (ctx->backlight)
		backlight_update_status(ctx->backlight);

	return 0;
}

static int s6e3hc2_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &s6e3hc2_mode_90hz);
}

static const struct drm_panel_funcs s6e3hc2_drm_funcs = {
	.unprepare = s6e3hc2_unprepare,
	.prepare = s6e3hc2_prepare,
	.get_modes = s6e3hc2_get_modes,
};

static int s6e3hc2_add(struct s6e3hc2_panel *ctx)
{
	struct device *dev = &ctx->link->dev;
	struct backlight_properties bl_props = {
		.type = BACKLIGHT_RAW,
		.brightness = 256,
		.max_brightness = 1023,
	};
	int ret;

	ctx->supplies[0].supply = "vddio"; /* 1.8 V */
	ctx->supplies[1].supply = "vdd";   /* 3.0 V */

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return ret;
	ret = regulator_set_voltage(ctx->supplies[0].consumer,
				    1800000, 1800000);
	if (ret < 0)
		return ret;
	ret = regulator_set_voltage(ctx->supplies[1].consumer,
				    3000000, 3000000);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		return dev_err_probe(dev, ret, "cannot get reset gpio\n");
	}

	ctx->backlight = devm_backlight_device_register(dev, "s6e3hc2",
							dev, ctx,
							&s6e3hc2_bl_ops,
							&bl_props);
	if (IS_ERR(ctx->backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight),
				     "failed to register backlight\n");

	ctx->base.prepare_prev_first = true;

	ctx->base.backlight = ctx->backlight;
	drm_panel_add(&ctx->base);

	return 0;
}

static int s6e3hc2_probe(struct mipi_dsi_device *dsi)
{
	struct s6e3hc2_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(&dsi->dev, struct s6e3hc2_panel, base,
				   &s6e3hc2_drm_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;
	dsi->dsc = &s6e3hc2_dsc_config;

	ctx->link = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = s6e3hc2_add(ctx);
	if (ret < 0)
		return ret;

	return mipi_dsi_attach(dsi);
}

static void s6e3hc2_remove(struct mipi_dsi_device *dsi)
{
	struct s6e3hc2_panel *ctx = mipi_dsi_get_drvdata(dsi);
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->base);
}

static const struct of_device_id s6e3hc2_of_match[] = {
	{ .compatible = "samsung,s6e3hc2" },
	{ }
};
MODULE_DEVICE_TABLE(of, s6e3hc2_of_match);

static struct mipi_dsi_driver s6e3hc2_driver = {
	.probe = s6e3hc2_probe,
	.remove = s6e3hc2_remove,
	.driver = {
		.name = "panel-samsung-s6e3hc2",
		.of_match_table = s6e3hc2_of_match,
	},
};
module_mipi_dsi_driver(s6e3hc2_driver);

MODULE_DESCRIPTION("MIPI-DSI based s6e3hc2 AMOLED Panel Driver");
MODULE_LICENSE("GPL v2");
