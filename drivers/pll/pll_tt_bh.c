/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_pll

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pll_tt_bh);

#define PLL_CNTL_0_OFFSET             0x00
#define PLL_CNTL_1_OFFSET             0x04
#define PLL_CNTL_2_OFFSET             0x08
#define PLL_CNTL_3_OFFSET             0x0C
#define PLL_CNTL_4_OFFSET             0x10
#define PLL_CNTL_5_OFFSET             0x14
#define PLL_CNTL_6_OFFSET             0x18
#define PLL_USE_POSTDIV_OFFSET        0x1C
#define PLL_REFCLK_SEL_OFFSET         0x20
#define PLL_USE_FINE_DIVIDER_1_OFFSET 0x24
#define PLL_USE_FINE_DIVIDER_2_OFFSET 0x28
#define FINE_DUTYC_ADJUST_OFFSET      0x2C
#define CLK_COUNTER_EN_OFFSET         0x30
#define CLK_COUNTER_0_OFFSET          0x34
#define CLK_COUNTER_1_OFFSET          0x38
#define CLK_COUNTER_2_OFFSET          0x3C
#define CLK_COUNTER_3_OFFSET          0x40
#define CLK_COUNTER_4_OFFSET          0x44
#define CLK_COUNTER_5_OFFSET          0x48
#define CLK_COUNTER_6_OFFSET          0x4C
#define CLK_COUNTER_7_OFFSET          0x50

struct pll_tt_bh_config {
	const struct device *parent_clock;
	uintptr_t base;
};

struct pll_tt_bh_data {
	struct k_spinlock lock;
	uint32_t current_rate;
};

static int pll_tt_bh_on(const struct device *dev, clock_control_subsys_t sys)
{
	return -ENOSYS;
}

static int pll_tt_bh_off(const struct device *dev, clock_control_subsys_t sys)
{
	return -ENOSYS;
}

static int pll_tt_bh_async_on(const struct device *dev, clock_control_subsys_t sys,
			      clock_control_cb_t cb, void *user_data)
{
	return -ENOSYS;
}

static int pll_tt_bh_get_rate(const struct device *dev, clock_control_subsys_t sys, uint32_t *rate)
{
	return -ENOSYS;
}

static enum clock_control_status pll_tt_bh_get_status(const struct device *dev,
						      clock_control_subsys_t sys)
{
	return CLOCK_CONTROL_STATUS_UNKNOWN;
}

static int pll_tt_bh_set_rate(const struct device *dev, clock_control_subsys_t sys,
			      clock_control_subsys_rate_t rate)
{
	return -ENOSYS;
}

static int pll_tt_bh_configure(const struct device *dev, clock_control_subsys_t sys, void *data)
{
	return -ENOSYS;
}

static int pll_tt_bh_init(const struct device *dev)
{
	return 0;
}

static const struct clock_control_driver_api pll_tt_bh_api = {.on = pll_tt_bh_on,
							      .off = pll_tt_bh_off,
							      .async_on = pll_tt_bh_async_on,
							      .get_rate = pll_tt_bh_get_rate,
							      .get_status = pll_tt_bh_get_status,
							      .set_rate = pll_tt_bh_set_rate,
							      .configure = pll_tt_bh_configure};

#define PLL_TT_BH_INIT(_inst)                                                                      \
	static struct pll_tt_bh_data pll_tt_bh_data_##_inst;                                       \
                                                                                                   \
	static const struct pll_tt_bh_config pll_tt_bh_config_##_inst = {                          \
		.base = DT_INST_REG_ADDR(_inst)};                                                  \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(_inst, &pll_tt_bh_init, NULL, &pll_tt_bh_data_##_inst,               \
			      &pll_tt_bh_config_##_inst, POST_KERNEL, 3, &pll_tt_bh_api);

DT_INST_FOREACH_STATUS_OKAY(PLL_TT_BH_INIT)
