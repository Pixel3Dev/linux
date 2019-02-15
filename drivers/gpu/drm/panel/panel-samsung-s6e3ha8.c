// SPDX-License-Identifier: GPL-2.0+
/*
 * MIPI-DSI Samsung s6e3ha8 panel driver. This is a 864x480
 * AMOLED panel with a command-only DSI interface.
 */

#include <drm/drm_modes.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/module.h>

struct s6e3ha8 {
	struct device *dev;
	struct drm_panel panel;
	struct regulator *supply[2];
	struct gpio_desc *reset_gpio;
};

/*
 * The timings are not very helpful as the display is used in
 * command mode.
 */
static const struct drm_display_mode samsung_s6e3ha8_mode = {
	.clock = 342651,
	.hdisplay = 1440,
	.hsync_start = 1440 + 116,
	.hsync_end = 1440 + 116 + 44,
	.htotal = 1440 + 116 + 44 + 116,
	.vdisplay = 2960,
	.vsync_start = 2960 + 124,
	.vsync_end = 2960 + 124 + 120,
	.vtotal = 2960 + 124 + 120 + 124,
	.vrefresh = 60,
	.width_mm = 70,
	.height_mm = 144,
};

static inline struct s6e3ha8 *panel_to_s6e3ha8(struct drm_panel *panel)
{
	return container_of(panel, struct s6e3ha8, panel);
}

static int s6e3ha8_unprepare(struct drm_panel *panel)
{
	struct s6e3ha8 *s6 = panel_to_s6e3ha8(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(s6->dev);
	int ret;

	/* Enter sleep mode */
	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret) {
		DRM_DEV_ERROR(s6->dev, "failed to enter sleep mode (%d)\n",
			      ret);
		return ret;
	}

	/* Assert RESET */
	gpiod_set_value_cansleep(s6->reset_gpio, 1);
	regulator_disable(s6->supply[0]);
	regulator_disable(s6->supply[1]);

	return 0;
}

static int s6e3ha8_prepare(struct drm_panel *panel)
{
	struct s6e3ha8 *s6 = panel_to_s6e3ha8(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(s6->dev);
	int ret;

	ret = regulator_enable(s6->supply[0]);
	if (ret) {
		DRM_DEV_ERROR(s6->dev, "failed to enable supply (%d)\n", ret);
		return ret;
	}
	ret = regulator_enable(s6->supply[1]);
	if (ret) {
		DRM_DEV_ERROR(s6->dev, "failed to enable supply (%d)\n", ret);
		return ret;
	}

	/* Assert RESET */
	gpiod_set_value_cansleep(s6->reset_gpio, 1);
	udelay(10);
	/* De-assert RESET */
	gpiod_set_value_cansleep(s6->reset_gpio, 0);
	msleep(120);

	/* Enabe tearing mode: send TE (tearing effect) at VBLANK */
	ret = mipi_dsi_dcs_set_tear_on(dsi,
				       MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret) {
		DRM_DEV_ERROR(s6->dev, "failed to enable vblank TE (%d)\n",
			      ret);
		return ret;
	}
	/* Exit sleep mode and power on */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret) {
		DRM_DEV_ERROR(s6->dev, "failed to exit sleep mode (%d)\n",
			      ret);
		return ret;
	}

	return 0;
}

static int s6e3ha8_enable(struct drm_panel *panel)
{
	struct s6e3ha8 *s6 = panel_to_s6e3ha8(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(s6->dev);
	int ret;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret) {
		DRM_DEV_ERROR(s6->dev, "failed to turn display on (%d)\n",
			      ret);
		return ret;
	}

	return 0;
}

static int s6e3ha8_disable(struct drm_panel *panel)
{
	struct s6e3ha8 *s6 = panel_to_s6e3ha8(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(s6->dev);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret) {
		DRM_DEV_ERROR(s6->dev, "failed to turn display off (%d)\n",
			      ret);
		return ret;
	}

	return 0;
}

static int s6e3ha8_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct drm_display_mode *mode;

	strncpy(connector->display_info.name, "Samsung S6D16D0\0",
		DRM_DISPLAY_INFO_LEN);

	mode = drm_mode_duplicate(panel->drm, &samsung_s6e3ha8_mode);
	if (!mode) {
		DRM_ERROR("bad mode or failed to add mode\n");
		return -EINVAL;
	}
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	drm_mode_probed_add(connector, mode);

	return 1; /* Number of modes */
}

static const struct drm_panel_funcs s6e3ha8_drm_funcs = {
	.disable = s6e3ha8_disable,
	.unprepare = s6e3ha8_unprepare,
	.prepare = s6e3ha8_prepare,
	.enable = s6e3ha8_enable,
	.get_modes = s6e3ha8_get_modes,
};

static int s6e3ha8_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct s6e3ha8 *s6;
	int ret;

	s6 = devm_kzalloc(dev, sizeof(struct s6e3ha8), GFP_KERNEL);
	if (!s6)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, s6);
	s6->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/*
	 * This display uses command mode so no MIPI_DSI_MODE_VIDEO
	 * or MIPI_DSI_MODE_VIDEO_SYNC_PULSE
	 *
	 * As we only send commands we do not need to be continuously
	 * clocked.
	 */
	dsi->mode_flags =
		MIPI_DSI_CLOCK_NON_CONTINUOUS |
		MIPI_DSI_MODE_EOT_PACKET |
		MIPI_DSI_MODE_LPM;

	s6->supply[0] = devm_regulator_get(dev, "vddi");
	if (IS_ERR(s6->supply[0]))
		return PTR_ERR(s6->supply[0]);
	s6->supply[1] = devm_regulator_get(dev, "vci");
	if (IS_ERR(s6->supply[1]))
		return PTR_ERR(s6->supply[1]);

	/* This asserts RESET by default */
	s6->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						 GPIOD_OUT_HIGH);
	if (IS_ERR(s6->reset_gpio)) {
		ret = PTR_ERR(s6->reset_gpio);
		if (ret != -EPROBE_DEFER)
			DRM_DEV_ERROR(dev, "failed to request GPIO (%d)\n",
				      ret);
		return ret;
	}

	drm_panel_init(&s6->panel);
	s6->panel.dev = dev;
	s6->panel.funcs = &s6e3ha8_drm_funcs;

	ret = drm_panel_add(&s6->panel);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&s6->panel);

	return ret;
}

static int s6e3ha8_remove(struct mipi_dsi_device *dsi)
{
	struct s6e3ha8 *s6 = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&s6->panel);

	return 0;
}

static const struct of_device_id s6e3ha8_of_match[] = {
	{ .compatible = "samsung,s6e3ha8" },
	{ }
};
MODULE_DEVICE_TABLE(of, s6e3ha8_of_match);

static struct mipi_dsi_driver s6e3ha8_driver = {
	.probe = s6e3ha8_probe,
	.remove = s6e3ha8_remove,
	.driver = {
		.name = "panel-samsung-s6e3ha8",
		.of_match_table = s6e3ha8_of_match,
	},
};
module_mipi_dsi_driver(s6e3ha8_driver);

MODULE_AUTHOR("Linus Wallei <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("MIPI-DSI s6e3ha8 Panel Driver");
MODULE_LICENSE("GPL v2");
