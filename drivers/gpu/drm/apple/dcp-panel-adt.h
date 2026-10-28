/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_PANEL_ADT_H
#define APPLE_DCP_PANEL_ADT_H

#include "dcp-display-metadata.h"

/* Only panel properties, never the machine or account identity. */
struct dcp_panel_adt {
	struct dcp_display_metadata identity;
	u32 width_mm, height_mm;
	bool found;
};

int dcp_panel_adt_parse(const void *data, size_t size, struct dcp_panel_adt *panel);
#endif
