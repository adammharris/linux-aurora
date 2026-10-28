// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Enable gate for the T6041 internal display of the M4 Pro J616s. m1n1
 * describes the DCP that iBoot leaves running (apple,t6041-handoff on dcp,
 * display and piodma, their firmware regions, apple,firmware-compat) and
 * leaves the display nodes disabled. They are enabled here, before devices
 * are created, only when that handoff is complete, so a boot loader without
 * it, or nomodeset, keeps the boot framebuffer.
 */
#define pr_fmt(fmt) "apple-t6041-display: " fmt

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/string.h>

#include <video/nomodeset.h>

#define T6041_HANDOFF "apple,t6041-handoff"
#define T6041_DCP_SID 23
#define T6041_DISP_SID 0
#define T6041_PIODMA_SID 16
#define ASC_CPU_CONTROL 0x44
#define ASC_CPU_CONTROL_RUN BIT(4)

enum {
	T6041_DCP, T6041_DISPLAY, T6041_PIODMA, T6041_DCP_DART,
	T6041_DISP_DART, T6041_DCP_MBOX, T6041_PANEL, T6041_NR_NODES
};

/* Nodes enabled, in this order: the DARTs and the mailbox before their users. */
static const int t6041_enable[] __initconst = { T6041_DCP_DART, T6041_DISP_DART, T6041_DCP_MBOX,
				    T6041_DISPLAY, T6041_DCP, T6041_PANEL };

static struct of_changeset t6041_gate;

/* The single @list target of @np, if it is @compat and (stream >= 0) that stream. */
static struct device_node *__init t6041_target(const struct device_node *np, const char *list,
					const char *cells, int stream, const char *compat)
{
	struct of_phandle_args args;

	if (of_count_phandle_with_args(np, list, cells) != 1 ||
	    of_parse_phandle_with_args(np, list, cells, 0, &args))
		return NULL;
	if (args.args_count != (stream < 0 ? 0 : 1) ||
	    (stream >= 0 && args.args[0] != (u32)stream) ||
	    !of_device_is_compatible(args.np, compat)) {
		of_node_put(args.np);
		return NULL;
	}
	return args.np;
}

static bool __init t6041_marked(const struct device_node *np)
{
	u32 marker;

	return np && of_property_count_u32_elems(np, T6041_HANDOFF) == 1 &&
	       !of_property_read_u32(np, T6041_HANDOFF, &marker) && marker == 1;
}

static bool __init t6041_disabled(const struct device_node *np)
{
	const char *status;

	return !of_property_read_string(np, "status", &status) && !strcmp(status, "disabled");
}

static int __init apple_t6041_display_gate(void)
{
	struct device_node *nodes[T6041_NR_NODES] = {};
	struct device_node *piodma_dart = NULL;
	void __iomem *cpu;
	u32 version[3];
	unsigned int i;
	int ret = 0;

	if (!of_machine_is_compatible("apple,j616s"))
		return 0;
	if (IS_ENABLED(CONFIG_VIDEO) && video_firmware_drivers_only())
		return 0;
	nodes[T6041_DCP] = of_find_node_by_path("dcp");
	if (!nodes[T6041_DCP] || !of_device_is_compatible(nodes[T6041_DCP], "apple,t6041-dcp") ||
	    !of_property_present(nodes[T6041_DCP], T6041_HANDOFF))
		goto out;	/* no handoff: the boot framebuffer stays */

	ret = -EINVAL;
	nodes[T6041_DISPLAY] = of_find_node_by_path("disp0");
	nodes[T6041_PIODMA] = of_get_child_by_name(nodes[T6041_DCP], "piodma");
	nodes[T6041_PANEL] = of_get_child_by_name(nodes[T6041_DCP], "panel");
	if (!nodes[T6041_DISPLAY] || !nodes[T6041_PIODMA] || !nodes[T6041_PANEL] ||
	    !of_device_is_compatible(nodes[T6041_DISPLAY], "apple,display-subsystem") ||
	    !t6041_marked(nodes[T6041_DCP]) || !t6041_marked(nodes[T6041_DISPLAY]) ||
	    !t6041_marked(nodes[T6041_PIODMA]))
		goto out;

	/* The desktop IOMFB instance is written for the 26.6.2 firmware only. */
	if (of_property_count_u32_elems(nodes[T6041_DCP], "apple,firmware-compat") != 3 ||
	    of_property_read_u32_array(nodes[T6041_DCP], "apple,firmware-compat", version, 3) ||
	    version[0] != 26 || version[1] != 6 || version[2] != 2)
		goto out;

	nodes[T6041_DCP_DART] = t6041_target(nodes[T6041_DCP], "iommus", "#iommu-cells",
					     T6041_DCP_SID, "apple,t8110-dart");
	nodes[T6041_DISP_DART] = t6041_target(nodes[T6041_DISPLAY], "iommus", "#iommu-cells",
					      T6041_DISP_SID, "apple,t8110-dart");
	piodma_dart = t6041_target(nodes[T6041_PIODMA], "iommus", "#iommu-cells",
				   T6041_PIODMA_SID, "apple,t8110-dart");
	nodes[T6041_DCP_MBOX] = t6041_target(nodes[T6041_DCP], "mboxes", "#mbox-cells", -1,
					     "apple,asc-mailbox-v4");
	if (!nodes[T6041_DCP_DART] || !nodes[T6041_DISP_DART] || !nodes[T6041_DCP_MBOX] ||
	    piodma_dart != nodes[T6041_DISP_DART] ||
	    nodes[T6041_DCP_DART] == nodes[T6041_DISP_DART])
		goto out;

	for (i = 0; i < ARRAY_SIZE(t6041_enable); i++) {
		if (!t6041_disabled(nodes[t6041_enable[i]])) {
			ret = -EBUSY;
			goto out;
		}
	}

	/* The handoff describes a DCP that iBoot left running. */
	cpu = of_iomap(nodes[T6041_DCP], 0);
	if (!cpu) {
		ret = -ENOMEM;
		goto out;
	}
	ret = readl(cpu + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN ? 0 : -EBUSY;
	iounmap(cpu);
	if (ret)
		goto out;

	of_changeset_init(&t6041_gate);
	for (i = 0; i < ARRAY_SIZE(t6041_enable) && !ret; i++)
		ret = of_changeset_update_prop_string(&t6041_gate, nodes[t6041_enable[i]],
						      "status", "okay");
	if (!ret)
		ret = of_changeset_apply(&t6041_gate);
	if (ret)
		of_changeset_destroy(&t6041_gate);
	else
		pr_info("display handoff accepted\n");
out:
	of_node_put(piodma_dart);
	for (i = 0; i < ARRAY_SIZE(nodes); i++)
		of_node_put(nodes[i]);
	if (ret)
		pr_info("display handoff refused: %d; keeping the boot framebuffer\n", ret);
	return 0;
}

/* Runs before of_platform_default_populate_init() at arch_initcall_sync. */
arch_initcall(apple_t6041_display_gate);

/*
 * External displays (HDMI behind dcpext0, Type-C behind dcpext1). The
 * device tree describes the whole chain disabled; it is enabled here once the
 * boot loader has given each external display processor its firmware memory
 * (the memory-region nodes of the dcpext are available, or they stay disabled
 * placeholders). Before any device exists only the Type-C routes are enabled
 * (the port controllers then wait for dcpext1's mode switches) and the
 * display subsystem is held back. The power-up waits for the PMP firmware,
 * whose acknowledgement the DISPEXT domains need: HDMI chain, Type-C chain,
 * then both processors and the display subsystem together, so the display
 * master is created once with every DART in its iommus.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/stringify.h>

/*
 * Two stages, each with its own display DART (a DART still disabled in the
 * display subsystem's iommus would make of_iommu drop its whole fwspec): the
 * HDMI chain, then the Type-C chain. Run at the same time the two processors
 * crashed (dcpext0: axi_rd_err), as they never did one after the other.
 * Parents first; the genpd links in the tree do the real ordering.
 */
struct gate_stage {
	const char *dart;		/* display DART of this stage */
	const char *const *nodes;
	unsigned int nr;
};

static const char *const gate_hdmi_nodes[] = {
	/* ATC3 power, PMP DISPEXT0 request, DISPEXT0, AOP pins, PHY, crossbar, DARTs, mailbox, dcpext0 */
	"/power-management@502800000/power-controller@408",
	"/power-management@508300000/power-controller@e0",
	"/power-management@508300000/power-controller@168",
	"/power-management@508300000/power-controller@b8",
	"/power-management@508300000/power-controller@148",
	"/pmp-report@5003c0000/report@11",
	"/power-management@502800000/power-controller@340",
	"/power-management@502800000/power-controller@348",
	"/pinctrl@510824000",
	"/mailbox@39b148000",
	"/phy@39aa90000",
	"/mux@39b24c000",
	"/iommu@3a2340000",
	"/iommu@3a2300000",
	"/mailbox@3a2e08000",
	"/dcp@3a2e00000/typec-routes/route@0",
	"/dcp@3a2e00000/typec-routes/route@1",
	"/dcp@3a2e00000/typec-routes/route@2",
	"/dcp@3a2e00000",
};

static const char *const gate_usbc_nodes[] = {
	/* PMP DISPEXT1 request, FAB7_SOC, DISPEXT1, crossbars, DARTs, mailbox, routes, dcpext1 */
	"/pmp-report@5003c0000/report@12",
	"/power-management@502800000/power-controller@488",
	"/power-management@502800000/power-controller@490",
	"/power-management@502800000/power-controller@498",
	"/mux@38324c000",
	"/mux@38b24c000",
	"/mux@39324c000",
	"/iommu@602340000",
	"/iommu@602300000",
	"/mailbox@602e08000",
	"/dcp@602e00000/typec-routes/route@0",
	"/dcp@602e00000/typec-routes/route@1",
	"/dcp@602e00000/typec-routes/route@2",
	"/dcp@602e00000",
};

static const struct gate_stage gate_stages[] = {
	{ "/iommu@3a2300000", gate_hdmi_nodes, ARRAY_SIZE(gate_hdmi_nodes) },
	{ "/iommu@602300000", gate_usbc_nodes, ARRAY_SIZE(gate_usbc_nodes) },
};

/* A display processor (a direct child of the root) and a Type-C display route. */
static bool gate_is_dcp(const char *path)
{
	return !strncmp(path, "/dcp@", 5) && !strchr(path + 5, '/');
}

static bool gate_is_route(const char *path)
{
	return strstr(path, "/typec-routes/");
}

static int gate_display_add_dart(struct of_changeset *cs, struct device_node *display,
					struct device_node *dart)
{
	struct property *old, *prop;
	__be32 *value;

	old = of_find_property(display, "iommus", NULL);
	if (!old || !dart->phandle || old->length % (2 * sizeof(*value)))
		return -EINVAL;
	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	value = kmalloc(old->length + 2 * sizeof(*value), GFP_KERNEL);
	if (!prop || !value) {
		kfree(prop);
		kfree(value);
		return -ENOMEM;
	}
	memcpy(value, old->value, old->length);
	value[old->length / sizeof(*value)] = cpu_to_be32(dart->phandle);
	value[old->length / sizeof(*value) + 1] = cpu_to_be32(0);
	prop->name = kstrdup("iommus", GFP_KERNEL);
	prop->length = old->length + 2 * sizeof(*value);
	prop->value = value;
	if (!prop->name) {
		kfree(value);
		kfree(prop);
		return -ENOMEM;
	}
	return of_changeset_update_property(cs, display, prop);
}

/*
 * The processor's firmware memory must be described: every memory-region of
 * the dcpext node (the stage's last node) must be an available reserved-memory
 * node. A loader that fills the regions from the ADT (m4-loader
 * m4_dcpext_fixup) leaves disabled placeholders when it refuses, and an older
 * loader leaves them all disabled; enabling the processor then would only
 * make its driver fail. /chosen m4,dcpext is that loader's record (0 filled,
 * 2 static memory), printed when the check fails.
 */
static bool gate_firmware_memory(const char *dcp)
{
	struct device_node *np, *region;
	int i, n;
	u32 record[2];

	np = of_find_node_by_path(dcp);
	if (!np)
		return false;
	n = of_count_phandle_with_args(np, "memory-region", NULL);
	for (i = 0; i < n; i++) {
		region = of_parse_phandle(np, "memory-region", i);
		if (region && of_device_is_available(region) &&
		    of_device_is_compatible(region, "apple,asc-mem")) {
			of_node_put(region);
			continue;
		}
		of_node_put(region);
		n = -ENODEV;
		break;
	}
	of_node_put(np);
	if (n > 0)
		return true;
	np = of_find_node_by_path("/chosen");
	if (np && !of_property_read_u32_array(np, "m4,dcpext", record, 2))
		pr_err("%s: no firmware memory (loader record <%u %u>), nothing enabled\n", dcp,
		       record[0], record[1]);
	else
		pr_err("%s: no firmware memory, nothing enabled\n", dcp);
	of_node_put(np);
	return false;
}

/* Every node of @st is there and still disabled, and its processor has firmware memory. */
static bool gate_stage_ready(const struct gate_stage *st)
{
	struct device_node *np;
	unsigned int i;

	if (!gate_firmware_memory(st->nodes[st->nr - 1]))
		return false;
	for (i = 0; i < st->nr; i++) {
		np = of_find_node_by_path(st->nodes[i]);
		if (!np) {
			pr_err("%s: no such node, nothing enabled\n", st->nodes[i]);
			return false;
		}
		if (of_device_is_available(np)) {
			pr_err("%s: already enabled, nothing changed\n", st->nodes[i]);
			of_node_put(np);
			return false;
		}
		of_node_put(np);
	}
	return true;
}

/* Enable @st's nodes and put its display DART into the display subsystem's iommus. */
static int gate_stage_apply(const struct gate_stage *st, struct device_node *display)
{
	struct device_node *np;
	struct of_changeset *cs;
	unsigned int i;
	int ret;

	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	of_changeset_init(cs);

	np = of_find_node_by_path(st->dart);
	ret = np ? gate_display_add_dart(cs, display, np) : -ENODEV;
	of_node_put(np);
	for (i = 0; !ret && i < st->nr; i++) {
		if (gate_is_dcp(st->nodes[i]) || gate_is_route(st->nodes[i]))
			continue;
		np = of_find_node_by_path(st->nodes[i]);
		ret = np ? of_changeset_update_prop_string(cs, np, "status", "okay") : -ENODEV;
		of_node_put(np);
	}
	if (!ret)
		ret = of_changeset_apply(cs);
	if (ret) {
		of_changeset_destroy(cs);
		kfree(cs);
	}
	/* Applied, it stays: the live tree refers to it. */
	return ret;
}

#define GATE_PMP_WAIT_MS 20000

/* A tether boot reserves ATC0 for the debug link. Its DP PHY is disabled,
 * so exposing a route@0 would make its DCP (dcpext1, or the dcpext0 hybrid)
 * defer on that missing supplier and take the usable ATC1/2 display routes
 * (and dcpext0's HDMI) down with it.
 */
static bool gate_route_phy_available(struct device_node *route)
{
	struct device_node *routes, *dcp;
	struct of_phandle_args phy;
	char name[16];
	u32 index;
	int match;
	bool available = false;

	if (of_property_read_u32(route, "reg", &index))
		return false;
	routes = of_get_parent(route);
	dcp = routes ? of_get_parent(routes) : NULL;
	of_node_put(routes);
	if (!dcp)
		return false;
	/* The fabric takes a route's PHY by name ("typecN"), not by position. */
	snprintf(name, sizeof(name), "typec%u", index);
	match = of_property_match_string(dcp, "phy-names", name);
	if (match >= 0)
		index = match;
	if (!of_parse_phandle_with_args(dcp, "phys", "#phy-cells", index, &phy)) {
		available = of_device_is_available(phy.np);
		of_node_put(phy.np);
	}
	of_node_put(dcp);
	return available;
}

/* Set the status of the processors (externals) or the routes, and the display subsystem, in one changeset. */
static int gate_set(struct device_node *display, bool dcps, const char *dcp_status, bool routes,
		    const char *route_status, const char *display_status)
{
	struct device_node *np;
	struct of_changeset *cs;
	unsigned int i, j;
	int ret = 0;

	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	of_changeset_init(cs);
	for (i = 0; !ret && i < ARRAY_SIZE(gate_stages); i++)
		for (j = 0; !ret && j < gate_stages[i].nr; j++) {
			const char *path = gate_stages[i].nodes[j];

			if (!(dcps && gate_is_dcp(path)) && !(routes && gate_is_route(path)))
				continue;
			np = of_find_node_by_path(path);
			if (np && gate_is_route(path) && route_status &&
			    !strcmp(route_status, "okay") && !gate_route_phy_available(np)) {
				pr_info("%s remains disabled: DP PHY unavailable\n", path);
				of_node_put(np);
				continue;
			}
			ret = np ? of_changeset_update_prop_string(cs, np, "status",
								   gate_is_dcp(path) ? dcp_status :
								   route_status) : -ENODEV;
			of_node_put(np);
		}
	if (!ret && display_status)
		ret = of_changeset_update_prop_string(cs, display, "status", display_status);
	if (!ret)
		ret = of_changeset_apply(cs);
	if (ret) {
		of_changeset_destroy(cs);
		kfree(cs);
	}
	return ret;
}

static struct device_node *gate_display;
static struct device_node *gate_pmp;
static atomic_t gate_started = ATOMIC_INIT(0);

/* Internal panel only: the routes go again, so the port controllers probe without mode switches. */
static void gate_fallback(const char *why)
{
	int ret = gate_set(gate_display, false, NULL, true, "disabled", "okay");

	pr_err("%s: external displays stay off, internal panel only (%d)\n", why, ret);
}

/*
 * The Type-C core treats a mode switch below a disabled node as absent, so the
 * port controllers, which probe before dcpext1 is enabled, only took the USB
 * muxes. Once dcpext1 is bound (its route mode switches are registered), let
 * each controller wired to a route probe again, so that it takes them too.
 */
static void gate_reprobe_port_controllers(void)
{
	struct device *done[ARRAY_SIZE(gate_usbc_nodes)] = {};
	struct device_node *dcp, *np, *ep, *connector, *ctrl;
	struct device *dev = NULL;
	unsigned int i, n = 0, tries;

	dcp = of_find_node_by_path("/dcp@602e00000");
	for (tries = 0; dcp && tries < 150; tries++) {
		dev = dcp->fwnode.dev;
		if (dev && READ_ONCE(dev->driver))
			break;
		msleep(200);
	}
	of_node_put(dcp);
	if (!dev || !READ_ONCE(dev->driver)) {
		pr_err("dcpext1 not bound, port controllers left as they are\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(gate_usbc_nodes); i++) {
		unsigned int k;

		if (!gate_is_route(gate_usbc_nodes[i]))
			continue;
		np = of_find_node_by_path(gate_usbc_nodes[i]);
		ep = np ? of_graph_get_next_endpoint(np, NULL) : NULL;
		connector = ep ? of_graph_get_remote_port_parent(ep) : NULL;
		ctrl = connector ? of_get_parent(connector) : NULL;
		dev = ctrl ? get_device(ctrl->fwnode.dev) : NULL;
		of_node_put(ctrl);
		of_node_put(connector);
		of_node_put(ep);
		of_node_put(np);
		if (!dev)
			continue;
		for (k = 0; k < n && done[k] != dev; k++)
			;
		if (k < n || !READ_ONCE(dev->driver)) {
			put_device(dev);
			continue;
		}
		done[n++] = dev;
		if (device_reprobe(dev))
			dev_err(dev, "probe again failed\n");
		else
			dev_info(dev, "probed again for the display routes\n");
	}
	while (n)
		put_device(done[--n]);
}

/*
 * A processor that finds its crossbar or DP PHY without a driver is given up
 * for good (the deferred probe timeout is 0 here), and those drivers are
 * modules that load only once the stage has created their devices. Wait until
 * every such device of the enabled infrastructure has its driver. Returns the
 * first path still without one, or NULL.
 */
static const char *gate_unbound_infra(void)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(gate_stages); i++)
		for (j = 0; j < gate_stages[i].nr; j++) {
			const char *path = gate_stages[i].nodes[j];
			struct device_node *np;
			struct device *dev;

			/* Power domains are the genpd core's to wait for; the modules are the point here. */
			if (gate_is_dcp(path) || gate_is_route(path) ||
			    !strncmp(path, "/power-management@", 18))
				continue;
			np = of_find_node_by_path(path);
			dev = np ? READ_ONCE(np->fwnode.dev) : NULL;
			of_node_put(np);
			if (dev && !READ_ONCE(dev->driver))
				return path;
		}
	return NULL;
}

#define GATE_DRIVER_WAIT_MS 10000

static void gate_wait_for_drivers(void)
{
	const char *path = NULL;
	unsigned int tries;

	for (tries = 0; tries < GATE_DRIVER_WAIT_MS / 100; tries++) {
		path = gate_unbound_infra();
		if (!path)
			return;
		msleep(100);
	}
	pr_warn("%s has no driver after " __stringify(GATE_DRIVER_WAIT_MS) " ms, enabling the processors anyway\n",
		path);
}

static void gate_power_up(struct work_struct *work)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; !ret && i < ARRAY_SIZE(gate_stages); i++) {
		ret = gate_stage_apply(&gate_stages[i], gate_display);
		if (!ret)
			pr_info("stage %u enabled\n", i);
	}
	if (!ret)
		gate_wait_for_drivers();
	/* Both processors and the display subsystem together: the master binds once, with all DCPs. */
	if (!ret)
		ret = gate_set(gate_display, true, "okay", false, NULL, "okay");
	if (ret) {
		gate_fallback("could not enable the external displays");
		return;
	}
	pr_info("processors and display subsystem enabled\n");
	gate_reprobe_port_controllers();
}
static DECLARE_WORK(gate_power_up_work, gate_power_up);

static void gate_timeout(struct work_struct *work)
{
	if (!atomic_xchg(&gate_started, 1))
		gate_fallback("no PMP within " __stringify(GATE_PMP_WAIT_MS) " ms");
}
static DECLARE_DELAYED_WORK(gate_timeout_work, gate_timeout);

/* The PMP's driver is bound: DISPEXT can be acknowledged now. */
static int gate_pmp_bound(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;

	if (action != BUS_NOTIFY_BOUND_DRIVER || dev->of_node != gate_pmp || atomic_xchg(&gate_started, 1))
		return NOTIFY_DONE;
	cancel_delayed_work(&gate_timeout_work);
	queue_work(system_long_wq, &gate_power_up_work);
	return NOTIFY_OK;
}
static struct notifier_block gate_pmp_nb = { .notifier_call = gate_pmp_bound };

static int __init apple_t6041_extdisp_gate(void)
{
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("apple,j616s"))
		return 0;
	if (IS_ENABLED(CONFIG_VIDEO) && video_firmware_drivers_only())
		return 0;

	/* Everything must be there and disabled, with firmware memory, before anything is changed. */
	for (i = 0; i < ARRAY_SIZE(gate_stages); i++)
		if (!gate_stage_ready(&gate_stages[i]))
			return 0;
	gate_display = of_find_node_by_path("/display-subsystem");
	gate_pmp = of_find_node_by_path("/pmp@500700000");
	if (!gate_display || !gate_pmp || !of_device_is_available(gate_display) ||
	    !of_device_is_available(gate_pmp)) {
		pr_err("no display subsystem or PMP, nothing enabled\n");
		return 0;
	}

	/* No device exists yet: routes on, display subsystem held back until the power-up. */
	ret = gate_set(gate_display, false, NULL, true, "okay", "disabled");
	if (ret) {
		pr_err("could not prepare the tree: %d, nothing enabled\n", ret);
		return 0;
	}
	ret = bus_register_notifier(&platform_bus_type, &gate_pmp_nb);
	if (ret) {
		gate_fallback("no bus notifier");
		return 0;
	}
	schedule_delayed_work(&gate_timeout_work, msecs_to_jiffies(GATE_PMP_WAIT_MS));
	pr_info("routes enabled, waiting for the PMP\n");
	return 0;
}
arch_initcall(apple_t6041_extdisp_gate);
