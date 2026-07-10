// SPDX-License-Identifier: GPL-2.0+
//
// Panel driver for third generation rM devices
//
// Copyright (C) 2022 reMarkable AS - http://www.remarkable.com/

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>

#include <video/of_display_timing.h>

#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct rm_cumulus_data {
	struct drm_panel panel;
	struct device *dev;

	struct regulator_bulk_data *supplies;
	u32 num_supplies;
	u32 vcom_voltage;
	struct gpio_desc *oev;
	struct gpio_desc *oeh;

	struct drm_display_mode display_mode;
	u32 display_bus_flags;
	bool prepared;
};

struct rm_cumulus_reg_data {
	const char *name;
	u32 target_uV;
	u32 tolerance_uV;
};

static struct rm_cumulus_reg_data rm_cumulus_regulator_data[] = {
	{
		.name = "vgl",
		.target_uV = 0,
		.tolerance_uV = 0,
	},
	{
		.name = "vneg3",
		.target_uV = 24000000,
		.tolerance_uV = 400000 / 2,
	},
	{
		.name = "vneg2",
		.target_uV = 12000000,
		.tolerance_uV = 400000 / 2,
	},
	{
		.name = "vneg1",
		.target_uV = 6000000,
		.tolerance_uV = 400000 / 2,
	},
	{
		.name = "vpos3",
		.target_uV = 24000000,
		.tolerance_uV = 400000 / 2,
	},
	{
		.name = "vpos2",
		.target_uV = 12000000,
		.tolerance_uV = 400000 / 2,
	},
	{
		.name = "vpos1",
		.target_uV = 6000000,
		.tolerance_uV = 400000 / 2,
	},
	{
		.name = "vgh2",
		.target_uV = 0,
		.tolerance_uV = 0,
	},
	{
		.name = "vpdd",
		.target_uV = 3000000,
		.tolerance_uV = 500000,
	},
	{
		.name = "vcom",
		.target_uV = 500000,
		.tolerance_uV = 19600,
	},
};

static inline struct rm_cumulus_data *to_rm_cumulus_data(struct drm_panel *panel)
{
	return container_of(panel, struct rm_cumulus_data, panel);
}

static struct regulator *rm_cumulus_find_regulator_by_name(struct rm_cumulus_data *cumulus,
							   const char *name,
							   int *index)
{
	int i;

	for (i = 0; i < cumulus->num_supplies; i++) {
		if (!strcmp(cumulus->supplies[i].supply, name)) {
			*index = i;
			return cumulus->supplies[i].consumer;
		}
	}

	return NULL;
}

static int rm_cumulus_set_regulator_voltage(struct rm_cumulus_data *cumulus,
					    const char *buf, const char *name)
{
	struct regulator *reg;
	int val_uv;
	int ret;
	int index = -1;

	ret = kstrtoint(buf, 0, &val_uv);
	if (ret)
		return ret;

	reg = rm_cumulus_find_regulator_by_name(cumulus, name, &index);
	if (!reg) {
		dev_err(cumulus->dev, "Unable to find regulator %s\n", name);
		return -ENODEV;
	}

	return regulator_set_voltage_tol(reg, val_uv,
					 rm_cumulus_regulator_data[index].tolerance_uV);
}

static int rm_cumulus_get_regulator_voltage(struct rm_cumulus_data *cumulus,
					    const char *name)
{
	struct regulator *reg;
	int index = -1;

	reg = rm_cumulus_find_regulator_by_name(cumulus, name, &index);
	if (!reg) {
		dev_err(cumulus->dev, "Unable to find regulator %s\n", name);
		return -ENODEV;
	}

	return regulator_get_voltage(reg);
}


#define VOLTAGE_RAIL_STORE(__name) \
	static ssize_t __name ## _store(struct device *dev, \
					struct device_attribute *attr, \
					const char *buf, size_t count) \
	{ \
		int ret; \
		ret = rm_cumulus_set_regulator_voltage(dev_get_drvdata(dev), \
						       buf, attr->attr.name); \
		return ret ? ret : count; \
	} \

#define VOLTAGE_RAIL_SHOW(__name) \
	static ssize_t __name ## _show(struct device *dev, \
					struct device_attribute *attr, \
					char *buf) \
	{ \
		int ret; \
		ret = rm_cumulus_get_regulator_voltage(dev_get_drvdata(dev), \
						       attr->attr.name); \
		return sysfs_emit(buf, "%d\n", ret); \
	} \

VOLTAGE_RAIL_STORE(vpos1)
VOLTAGE_RAIL_STORE(vpos2)
VOLTAGE_RAIL_STORE(vpos3)
VOLTAGE_RAIL_STORE(vneg1)
VOLTAGE_RAIL_STORE(vneg2)
VOLTAGE_RAIL_STORE(vneg3)
VOLTAGE_RAIL_STORE(vcom)
VOLTAGE_RAIL_STORE(vpdd)

VOLTAGE_RAIL_SHOW(vpos1)
VOLTAGE_RAIL_SHOW(vpos2)
VOLTAGE_RAIL_SHOW(vpos3)
VOLTAGE_RAIL_SHOW(vneg1)
VOLTAGE_RAIL_SHOW(vneg2)
VOLTAGE_RAIL_SHOW(vneg3)
VOLTAGE_RAIL_SHOW(vcom)
VOLTAGE_RAIL_SHOW(vpdd)

static DEVICE_ATTR_RW(vpos1);
static DEVICE_ATTR_RW(vpos2);
static DEVICE_ATTR_RW(vpos3);
static DEVICE_ATTR_RW(vneg1);
static DEVICE_ATTR_RW(vneg2);
static DEVICE_ATTR_RW(vneg3);
static DEVICE_ATTR_RW(vcom);
static DEVICE_ATTR_RW(vpdd);

static struct attribute *rm_cumulus_attributes[] = {
	&dev_attr_vpos1.attr,
	&dev_attr_vpos2.attr,
	&dev_attr_vpos3.attr,
	&dev_attr_vneg1.attr,
	&dev_attr_vneg2.attr,
	&dev_attr_vneg3.attr,
	&dev_attr_vcom.attr,
	&dev_attr_vpdd.attr,
	NULL,
};

static const struct attribute_group rm_cumulus_attr_group = {
	.attrs	= rm_cumulus_attributes,
};


static int rm_cumulus_unprepare(struct drm_panel *panel)
{
	struct rm_cumulus_data *cumulus = to_rm_cumulus_data(panel);
	int ret;

	if (!cumulus->prepared)
		return 0;

	gpiod_set_value_cansleep(cumulus->oeh, 0);
	gpiod_set_value_cansleep(cumulus->oev, 0);

	ret = regulator_bulk_disable(cumulus->num_supplies, cumulus->supplies);
	if (!ret)
		cumulus->prepared = false;

	return ret;

}

static int rm_cumulus_prepare(struct drm_panel *panel)
{
	struct rm_cumulus_data *cumulus = to_rm_cumulus_data(panel);
	int ret;

	if (cumulus->prepared)
		return 0;

	gpiod_set_value_cansleep(cumulus->oeh, 1);
	gpiod_set_value_cansleep(cumulus->oev, 1);

	ret = regulator_bulk_enable(cumulus->num_supplies, cumulus->supplies);
	if (ret) {
		gpiod_set_value_cansleep(cumulus->oeh, 0);
		gpiod_set_value_cansleep(cumulus->oev, 0);
		return ret;
	}

	cumulus->prepared = true;
	return ret;
}

static int rm_cumulus_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct rm_cumulus_data *cumulus = to_rm_cumulus_data(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &cumulus->display_mode);
	if (!mode)
		return -ENOMEM;

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	connector->display_info.bus_flags = cumulus->display_bus_flags;

	return 1;
}

static const struct drm_panel_funcs rm_cumulus_funcs = {
	.unprepare = rm_cumulus_unprepare,
	.prepare = rm_cumulus_prepare,
	.get_modes = rm_cumulus_get_modes,
};

static int rm_cumulus_init_regulator_voltages(struct rm_cumulus_data *cumulus)
{
	int i;
	int ret;

	for (i = 0; i < cumulus->num_supplies; i++) {
		struct regulator *reg = cumulus->supplies[i].consumer;
		struct rm_cumulus_reg_data *data =
			&rm_cumulus_regulator_data[i];
		uint32_t target_uV = data->target_uV;
		uint32_t tolerance_uV = data->tolerance_uV;

		/* Fixed voltage nodes */
		if (target_uV == 0)
			continue;

		/* Override default with vcom voltage from dts */
		if (!strcmp("vcom", data->name)) {
			if (cumulus->vcom_voltage)
				target_uV = cumulus->vcom_voltage;
		}

		ret = regulator_set_voltage_tol(reg, target_uV, tolerance_uV);
		if (ret < 0)
			break;
	}

	return ret;
}

static int rm_cumulus_parse_of(struct rm_cumulus_data *cumulus)
{
	struct device_node *np = cumulus->dev->of_node;
	int ret, i;

	ret = of_get_drm_display_mode(np, &cumulus->display_mode,
				      &cumulus->display_bus_flags,
				      OF_USE_NATIVE_MODE);
	if (ret < 0) {
		dev_err(cumulus->dev, "OF: Failed to get display-timing (%d)\n", ret);
		return ret;
	}

	cumulus->oeh = devm_gpiod_get_optional(cumulus->dev, "oeh", GPIOD_OUT_LOW);
	if(IS_ERR(cumulus->oeh)) {
		dev_err(cumulus->dev, "OF: Failed to get OEH gpio (%ld)\n",
			PTR_ERR(cumulus->oeh));
		return ret;
	}
	
	cumulus->oev = devm_gpiod_get_optional(cumulus->dev, "oev", GPIOD_OUT_LOW);
	if(IS_ERR(cumulus->oev)) {
		dev_err(cumulus->dev, "OF: Failed to get OEv gpio (%ld)\n",
			PTR_ERR(cumulus->oev));
		return ret;
	}

	cumulus->num_supplies = ARRAY_SIZE(rm_cumulus_regulator_data);
	cumulus->supplies = devm_kcalloc(cumulus->dev, cumulus->num_supplies,
					 sizeof(*cumulus->supplies), GFP_KERNEL);
	if (!cumulus->supplies)
		return -ENOMEM;

	for (i = 0; i < cumulus->num_supplies; i++)
		cumulus->supplies[i].supply = rm_cumulus_regulator_data[i].name;

	ret = devm_regulator_bulk_get(cumulus->dev, cumulus->num_supplies,
				      cumulus->supplies);
	if (ret < 0) {
		dev_err(cumulus->dev, "OF: Failed to get regulators (%d)\n", ret);
		return ret;
	}

	if (of_property_read_u32(np, "vcom-voltage", &cumulus->vcom_voltage)) {
		dev_err(cumulus->dev,
			"OF: Failed to get VCOM voltage. Using default\n");
		cumulus->vcom_voltage = 0;
	}

	return ret;
}

static int rm_cumulus_probe(struct platform_device *pdev)
{
	struct rm_cumulus_data *cumulus;
	int ret;

	cumulus = devm_kzalloc(&pdev->dev, sizeof(*cumulus), GFP_KERNEL);
	if (!cumulus)
		return -ENOMEM;

	cumulus->dev = &pdev->dev;
	dev_set_drvdata(cumulus->dev, cumulus);

	ret = rm_cumulus_parse_of(cumulus);
	if (ret) {
		dev_err(cumulus->dev, "Failed to parse OF (%d)\n", ret);
		return ret;
	}

	ret = sysfs_create_group(&cumulus->dev->kobj, &rm_cumulus_attr_group);
	if (ret != 0) {
		dev_err(cumulus->dev,
			"Failed to create attribute group: %d\n", ret);
		return ret;
	}

	ret = rm_cumulus_init_regulator_voltages(cumulus);
	if (ret) {
		dev_err(cumulus->dev, "Failed to set regulator voltage (%d)\n", ret);
		sysfs_remove_group(&cumulus->dev->kobj, &rm_cumulus_attr_group);
		return ret;
	}

	drm_panel_init(&cumulus->panel, cumulus->dev, &rm_cumulus_funcs,
		       DRM_MODE_CONNECTOR_LVDS);

	drm_panel_add(&cumulus->panel);

	return 0;
}

static void rm_cumulus_remove(struct platform_device *pdev)
{
	struct rm_cumulus_data *cumulus = platform_get_drvdata(pdev);

	sysfs_remove_group(&cumulus->dev->kobj, &rm_cumulus_attr_group);

	drm_panel_remove(&cumulus->panel);

	drm_panel_disable(&cumulus->panel);
}

static const struct of_device_id rm_cumulus_id_table[] = {
	{ .compatible = "remarkable,cumulus-panel", },
	{},
};
MODULE_DEVICE_TABLE(of, rm_cumulus_id_table);

static struct platform_driver rm_cumulus_driver = {
	.probe = rm_cumulus_probe,
	.remove = rm_cumulus_remove,
	.driver = {
		.name = "remarkable-cumulus-panel",
		.of_match_table = rm_cumulus_id_table,
	},
};
module_platform_driver(rm_cumulus_driver);

MODULE_AUTHOR("Morten Schade Flå <morten.schade.fla@remarkable.no>");
MODULE_DESCRIPTION("reMarkable Cumulus lvds panel driver");
MODULE_LICENSE("GPL");
