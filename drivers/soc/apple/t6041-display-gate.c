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
