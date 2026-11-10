/*
 * Virtual thermal sensors and state coolers for Amazon's MT8183 Echo Shows.
 *
 * Fire OS loads these as thermal_module.ko, whose source Amazon's release
 * leaves out. This driver is written from the device tree bindings and the
 * behavior of that module, for the "amazon,virtual_sensor" and
 * "amazon,thermal_cooler" nodes.
 *
 * A virtual sensor is a weighted sum over other thermal zones. Its zone-list
 * child names the zones and gives each six cells:
 *
 *   <sign offset alpha weight min max>
 *
 * The offset counts as given when sign is not 0 and negated when it is 0.
 * alpha is the weight of a new reading in the zone's moving average and
 * weight the zone's share of the sum, both in thousandths. A reading outside
 * min..max (when either is set) is refused until the zone has given one good
 * reading.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2.
 */

#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#define VS_SCALE 1000

struct vs_zone {
	struct list_head node;
	const char *name;
	struct thermal_zone_device *tz;
	int offset;
	int alpha;
	int weight;
	int min;
	int max;
	bool check_range;
	bool seen;
	int avg;
};

struct vs_sensor {
	struct device *dev;
	struct thermal_zone_device *tzd;
	struct list_head zones;
	struct mutex lock;
	int last;
	bool reported;
};

static int vs_get_temp(void *data, int *temp)
{
	struct vs_sensor *vs = data;
	struct vs_zone *z;
	long sum = 0;
	int ret = 0;

	mutex_lock(&vs->lock);
	list_for_each_entry(z, &vs->zones, node) {
		int t;

		if (!z->weight)
			continue;
		if (!z->tz) {
			struct thermal_zone_device *tz =
				thermal_zone_get_zone_by_name(z->name);

			if (IS_ERR(tz)) {
				ret = -EAGAIN;
				goto out;
			}
			z->tz = tz;
		}
		ret = thermal_zone_get_temp(z->tz, &t);
		if (ret)
			goto out;
		if (!z->seen) {
			if (z->check_range && (t <= z->min || t >= z->max)) {
				dev_err_ratelimited(vs->dev,
					"%s reads %d, outside its range\n",
					z->name, t);
				ret = -EINVAL;
				goto out;
			}
			z->seen = true;
			z->avg = t - z->offset;
		} else {
			z->avg = (z->alpha * (t - z->offset) +
				  (VS_SCALE - z->alpha) * z->avg) / VS_SCALE;
		}
		sum += (long)z->weight * z->avg / VS_SCALE;
	}
	if (sum < 0)
		sum = 0;
	vs->last = sum;
	if (!vs->reported) {
		vs->reported = true;
		dev_info(vs->dev, "first reading %ld\n", sum);
	}
	*temp = sum;
out:
	mutex_unlock(&vs->lock);
	return ret;
}

static const struct thermal_zone_of_device_ops vs_ops = {
	.get_temp = vs_get_temp,
};

static int vs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *list;
	struct vs_sensor *vs;
	struct property *prop;
	const char *name;

	vs = devm_kzalloc(dev, sizeof(*vs), GFP_KERNEL);
	if (!vs)
		return -ENOMEM;
	vs->dev = dev;
	INIT_LIST_HEAD(&vs->zones);
	mutex_init(&vs->lock);

	list = of_get_child_by_name(dev->of_node, "zone-list");
	if (!list) {
		dev_err(dev, "no zone-list\n");
		return -EINVAL;
	}
	of_property_for_each_string(list, "zone-names", prop, name) {
		struct vs_zone *z;
		u32 v[6];

		if (of_property_read_u32_array(list, name, v, ARRAY_SIZE(v))) {
			dev_err(dev, "%s needs six cells\n", name);
			of_node_put(list);
			return -EINVAL;
		}
		z = devm_kzalloc(dev, sizeof(*z), GFP_KERNEL);
		if (!z) {
			of_node_put(list);
			return -ENOMEM;
		}
		z->name = name;
		z->offset = v[0] ? (int)v[1] : -(int)v[1];
		z->alpha = v[2];
		z->weight = v[3];
		z->min = v[4];
		z->max = v[5];
		z->check_range = v[4] || v[5];
		list_add_tail(&z->node, &vs->zones);
		dev_info(dev, "%s: offset %d alpha %d weight %d\n", name,
			 z->offset, z->alpha, z->weight);
	}
	of_node_put(list);

	vs->tzd = thermal_zone_of_sensor_register(dev, 0, vs, &vs_ops);
	if (IS_ERR(vs->tzd)) {
		dev_err(dev, "cannot register the sensor: %ld\n",
			PTR_ERR(vs->tzd));
		return PTR_ERR(vs->tzd);
	}
	platform_set_drvdata(pdev, vs);
	return 0;
}

static int vs_remove(struct platform_device *pdev)
{
	struct vs_sensor *vs = platform_get_drvdata(pdev);

	thermal_zone_of_sensor_unregister(&pdev->dev, vs->tzd);
	return 0;
}

static const struct of_device_id vs_of_match[] = {
	{ .compatible = "amazon,virtual_sensor", },
	{ }
};

static struct platform_driver vs_driver = {
	.probe = vs_probe,
	.remove = vs_remove,
	.driver = {
		.name = "amazon_virtual_sensor",
		.of_match_table = vs_of_match,
	},
};

/*
 * A cooler only holds a state. Each change goes out as a uevent, and what
 * acts on it (Wi-Fi power, speaker level, suspend) is up to userspace.
 */
struct az_cooler {
	struct device *dev;
	struct thermal_cooling_device *cdev;
	unsigned long max_state;
	unsigned long state;
};

static int az_get_max_state(struct thermal_cooling_device *cdev,
			    unsigned long *state)
{
	struct az_cooler *c = cdev->devdata;

	*state = c->max_state;
	return 0;
}

static int az_get_cur_state(struct thermal_cooling_device *cdev,
			    unsigned long *state)
{
	struct az_cooler *c = cdev->devdata;

	*state = c->state;
	return 0;
}

static int az_set_cur_state(struct thermal_cooling_device *cdev,
			    unsigned long state)
{
	struct az_cooler *c = cdev->devdata;
	char type[48], level[24];
	char *envp[] = { type, level, NULL };

	if (state > c->max_state)
		state = c->max_state;
	if (state == c->state)
		return 0;
	c->state = state;
	snprintf(type, sizeof(type), "CDTYPE=%s", cdev->type);
	snprintf(level, sizeof(level), "CDLEV=%lu", state);
	kobject_uevent_env(&c->dev->kobj, KOBJ_CHANGE, envp);
	return 0;
}

static const struct thermal_cooling_device_ops az_cooler_ops = {
	.get_max_state = az_get_max_state,
	.get_cur_state = az_get_cur_state,
	.set_cur_state = az_set_cur_state,
};

static int az_cooler_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct az_cooler *c;
	u32 max;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	if (of_property_read_u32(dev->of_node, "max_state", &max)) {
		dev_err(dev, "no max_state\n");
		return -EINVAL;
	}
	c->dev = dev;
	c->max_state = max;
	c->cdev = thermal_of_cooling_device_register(dev->of_node,
			(char *)dev->of_node->name, c, &az_cooler_ops);
	if (IS_ERR(c->cdev))
		return PTR_ERR(c->cdev);
	platform_set_drvdata(pdev, c);
	dev_info(dev, "cooler %s, %lu states\n", dev->of_node->name,
		 c->max_state);
	return 0;
}

static int az_cooler_remove(struct platform_device *pdev)
{
	struct az_cooler *c = platform_get_drvdata(pdev);

	thermal_cooling_device_unregister(c->cdev);
	return 0;
}

static const struct of_device_id az_cooler_of_match[] = {
	{ .compatible = "amazon,thermal_cooler", },
	{ }
};

static struct platform_driver az_cooler_driver = {
	.probe = az_cooler_probe,
	.remove = az_cooler_remove,
	.driver = {
		.name = "amazon_thermal_cooler",
		.of_match_table = az_cooler_of_match,
	},
};

static int __init amazon_thermal_init(void)
{
	int ret;

	ret = platform_driver_register(&az_cooler_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&vs_driver);
	if (ret)
		platform_driver_unregister(&az_cooler_driver);
	return ret;
}
late_initcall(amazon_thermal_init);

MODULE_DESCRIPTION("Amazon virtual thermal sensors and state coolers");
MODULE_LICENSE("GPL v2");
