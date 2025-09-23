/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include "throttler.h"
#include "aiclk_ppm.h"
#include "cm2dm_msg.h"
#include <zephyr/drivers/misc/bh_fwtable.h>
#include "telemetry_internal.h"
#include "telemetry.h"
#include "timer.h"

#define kThrottlerAiclkScaleFactor 500.0F
#define DEFAULT_BOARD_POWER_LIMIT  150

/* CSM memory region for telemetry data */
#define CSM_TELEMETRY_BASE_ADDR     0x10060000
#define CSM_TELEMETRY_END_ADDR      0x1007FFFF
#define CSM_TELEMETRY_SIZE          (CSM_TELEMETRY_END_ADDR - CSM_TELEMETRY_BASE_ADDR + 1)

/* CSM memory offsets for specific telemetry values */
#define CSM_OFFSET_TDP_POWER        0x0004  /* TDP power value (float as uint32_t) */
#define CSM_OFFSET_BOARD_POWER      0x0008  /* Board power value (float as uint32_t) */
#define CSM_OFFSET_TDP_LIMIT        0x000C  /* TDP limit value (float as uint32_t) */
#define CSM_OFFSET_BOARD_POWER_LIMIT 0x0010 /* Board power limit value (float as uint32_t) */

LOG_MODULE_REGISTER(throttler);

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

typedef enum {
	kThrottlerTDP,
	kThrottlerFastTDC,
	kThrottlerTDC,
	kThrottlerThm,
	kThrottlerBoardPower,
	kThrottlerGDDRThm,
	kThrottlerCount,
} ThrottlerId;

typedef struct {
	float min;
	float max;
} ThrottlerLimitRange;

/* This table is used to restrict the throttler limits to reasonable ranges. */
/* They are passed in from the FW table in SPI */
static const ThrottlerLimitRange throttler_limit_ranges[kThrottlerCount] = {
	[kThrottlerTDP] = {
			.min = 50,
			.max = 500,
		},
	[kThrottlerFastTDC] = {
			.min = 50,
			.max = 500,
		},
	[kThrottlerTDC] = {
			.min = 50,
			.max = 400,
		},
	[kThrottlerThm] = {
			.min = 50,
			.max = 100,
		},
	[kThrottlerBoardPower] = {
			.min = 50,
			.max = 600,
		},
	[kThrottlerGDDRThm] = {
		.min = 50,
		.max = 100,
	}};

typedef struct {
	float alpha_filter;
	float p_gain;
	float d_gain;
} ThrottlerParams;

typedef struct {
	const AiclkArbMax arb_max; /* The arbiter associated with this throttler */

	const ThrottlerParams params;
	float limit;
	float value;
	float error;
	float prev_error;
	float output;
} Throttler;

static Throttler throttler[kThrottlerCount] = {
	[kThrottlerTDP] = {

			.arb_max = kAiclkArbMaxTDP,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerFastTDC] = {

			.arb_max = kAiclkArbMaxFastTDC,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.5,
					.d_gain = 0,
				},
		},
	[kThrottlerTDC] = {

			.arb_max = kAiclkArbMaxTDC,
			.params = {

					.alpha_filter = 0.1,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerThm] = {

			.arb_max = kAiclkArbMaxThm,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerBoardPower] = {
			.arb_max = kAiclkArbMaxBoardPower,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.1,
					.d_gain = 0.1,
			}
	},
	[kThrottlerGDDRThm] = {
			.arb_max = kAiclkArbMaxGDDRThm,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
	}
};

static void SetThrottlerLimit(ThrottlerId id, float limit)
{
	float clamped_limit =
		CLAMP(limit, throttler_limit_ranges[id].min, throttler_limit_ranges[id].max);

	LOG_INF("Throttler %d limit set to %d", id, (uint32_t)clamped_limit);
	throttler[id].limit = clamped_limit;
}

void InitThrottlers(void)
{
	SetThrottlerLimit(kThrottlerTDP,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit);
	SetThrottlerLimit(kThrottlerFastTDC,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdc_fast_limit);
	SetThrottlerLimit(kThrottlerTDC,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdc_limit);
	SetThrottlerLimit(kThrottlerThm,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.thm_limit);
	SetThrottlerLimit(kThrottlerBoardPower, DEFAULT_BOARD_POWER_LIMIT);
	SetThrottlerLimit(kThrottlerGDDRThm,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.gddr_thm_limit);
}

static void UpdateThrottler(ThrottlerId id, float value)
{
	Throttler *t = &throttler[id];

	t->value = t->params.alpha_filter * value + (1 - t->params.alpha_filter) * t->value;
	t->error = (t->limit - t->value) / t->limit;
	t->output = t->params.p_gain * t->error + t->params.d_gain * (t->error - t->prev_error);
	t->prev_error = t->error;
}

static void UpdateThrottlerArb(ThrottlerId id)
{
	Throttler *t = &throttler[id];

	float arb_val = GetThrottlerArbMax(t->arb_max);

	arb_val += t->output * kThrottlerAiclkScaleFactor;

	SetAiclkArbMax(t->arb_max, arb_val);
}

void CalculateThrottlers(void)
{
	static int j = 0;

	TelemetryInternalData telemetry_internal_data;

	ReadTelemetryInternal(1, &telemetry_internal_data);

	UpdateThrottler(kThrottlerTDP, telemetry_internal_data.vcore_power);
	UpdateThrottler(kThrottlerFastTDC, telemetry_internal_data.vcore_current);
	UpdateThrottler(kThrottlerTDC, telemetry_internal_data.vcore_current);
	UpdateThrottler(kThrottlerThm, telemetry_internal_data.asic_temperature);
	UpdateThrottler(kThrottlerBoardPower, GetInputPower());
	UpdateThrottler(kThrottlerGDDRThm, GetMaxGDDRTemp());

	/* Write telemetry values to CSM memory for external access */
	union {
		float f;
		uint32_t u;
	} float_to_uint;

	if (j < 500) {
		WriteTelemetryToCSM(0x0000 + (j * 12), TimerTimestamp());

		/* Write TDP power value */
		float_to_uint.f = telemetry_internal_data.vcore_power;
		WriteTelemetryToCSM(0x0004 + (j * 12), float_to_uint.u);

		/* Write Board power value */
		float_to_uint.f = GetInputPower();
		WriteTelemetryToCSM(0x0008 + (j * 12), float_to_uint.u);

		/* Write TDP limit value */
		//float_to_uint.f = throttler[kThrottlerTDP].limit;
		//WriteTelemetryToCSM(CSM_OFFSET_TDP_LIMIT + (j * 16), float_to_uint.u);

		/* Write Board power limit value */
		//float_to_uint.f = throttler[kThrottlerBoardPower].limit;
		//WriteTelemetryToCSM(CSM_OFFSET_BOARD_POWER_LIMIT + (j * 16), float_to_uint.u);

		j++;
	}

	for (ThrottlerId i = 0; i < kThrottlerCount; i++) {
		UpdateThrottlerArb(i);
	}
}

int32_t Dm2CmSetBoardPowerLimit(const uint8_t *data, uint8_t size)
{
	if (size != 2) {
		return -1;
	}

	uint32_t power_limit = sys_get_le16(data);

	LOG_INF("Cable Power Limit: %u", power_limit);
	power_limit = MIN(power_limit,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.board_power_limit);

	SetThrottlerLimit(kThrottlerBoardPower, power_limit);
	UpdateTelemetryBoardPowerLimit(power_limit);

	return 0;
}

/**
 * @brief Read telemetry data from CSM memory region
 * 
 * @param offset Offset from CSM_TELEMETRY_BASE_ADDR (must be 4-byte aligned)
 * @return uint32_t Value read from the specified CSM memory location
 */
uint32_t ReadTelemetryFromCSM(uint32_t offset)
{
	uintptr_t addr = CSM_TELEMETRY_BASE_ADDR + offset;
	
	/* Validate address is within CSM telemetry region */
	if (addr > CSM_TELEMETRY_END_ADDR) {
		LOG_ERR("CSM telemetry read address 0x%08x out of range", (uint32_t)addr);
		return 0;
	}
	
	/* Ensure 4-byte alignment for 32-bit reads */
	if (offset & 0x3) {
		LOG_WRN("CSM telemetry read offset 0x%08x not 4-byte aligned", offset);
	}
	
	uint32_t value = sys_read32(addr);
	LOG_DBG("CSM telemetry read: addr=0x%08x, value=0x%08x", (uint32_t)addr, value);
	
	return value;
}

/**
 * @brief Write telemetry data to CSM memory region
 * 
 * @param offset Offset from CSM_TELEMETRY_BASE_ADDR (must be 4-byte aligned)
 * @param value Value to write to the specified CSM memory location
 */
void WriteTelemetryToCSM(uint32_t offset, uint32_t value)
{
	uintptr_t addr = CSM_TELEMETRY_BASE_ADDR + offset;
	
	/* Validate address is within CSM telemetry region */
	if (addr > CSM_TELEMETRY_END_ADDR) {
		LOG_ERR("CSM telemetry write address 0x%08x out of range", (uint32_t)addr);
		return;
	}
	
	/* Ensure 4-byte alignment for 32-bit writes */
	if (offset & 0x3) {
		LOG_WRN("CSM telemetry write offset 0x%08x not 4-byte aligned", offset);
	}
	
	LOG_DBG("CSM telemetry write: addr=0x%08x, value=0x%08x", (uint32_t)addr, value);
	sys_write32(value, addr);
}
