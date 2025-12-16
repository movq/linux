/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2019 Linaro Limited
 * Copyright (c) 2021, AngeloGioacchino Del Regno
 *                     <angelogioacchino.delregno@somainline.org>
 */

#ifndef __CPR_H__
#define __CPR_H__

struct cpr_opp_voltage {
	unsigned long freq;
	int uV;
};

struct cpr_ext_data {
	int mem_acc_threshold_uV;
	int apm_threshold_uV;

	int num_voltages;
	struct cpr_opp_voltage *voltages;
};

#endif /* __CPR_H__ */
