/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_noc_dma

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma/dma_noc_tt_bh.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include "noc.h"
#include "noc2axi.h"
#include "util.h"

LOG_MODULE_REGISTER(dma_noc_tt_bh, CONFIG_DMA_LOG_LEVEL);

#define NOC_DMA_TLB        0
#define NOC_DMA_NOC_ID     0
#define NOC_DMA_TIMEOUT_MS 100
#define NOC_MAX_BURST_SIZE 16384

/* NOC CMD fields */
#define NOC_CMD_CPY               (0 << 0)
#define NOC_CMD_RD                (0 << 1)
#define NOC_CMD_WR                (1 << 1)
#define NOC_CMD_RESP_MARKED       (1 << 4)
#define NOC_CMD_BRCST_PACKET      (1 << 5)
#define NOC_CMD_PATH_RESERVE      (1 << 8)
#define NOC_CMD_BRCST_SRC_INCLUDE (1 << 17)

/* NOC0 RISC0 DMA registers */
#define TARGET_ADDR_LO           0xFFB20000
#define TARGET_ADDR_MID          0xFFB20004
#define TARGET_ADDR_HI           0xFFB20008
#define RET_ADDR_LO              0xFFB2000C
#define RET_ADDR_MID             0xFFB20010
#define RET_ADDR_HI              0xFFB20014
#define PACKET_TAG               0xFFB20018
#define CMD_BRCST                0xFFB2001C
#define AT_LEN                   0xFFB20020
#define AT_LEN_1                 0xFFB20024
#define AT_DATA                  0xFFB20028
#define BRCST_EXCLUDE            0xFFB2002C
#define CMD_CTRL                 0xFFB20040
#define NIU_MST_WR_ACK_RECEIVED  0xFFB20204
#define NIU_MST_RD_RESP_RECEIVED 0xFFB20208

struct tt_bh_dma_noc_config {
};

struct tt_bh_dma_noc_data {
	struct dma_block_config block;
	struct tt_bh_dma_noc_coords coords;
};

struct ret_addr_hi {
	uint32_t end_x: 6;
	uint32_t end_y: 6;
	uint32_t start_x: 6;
	uint32_t start_y: 6;
};

union ret_addr_hi_u {
	struct ret_addr_hi f;
	uint32_t u;
};

static inline void program_noc_dma_tlb(uint8_t x, uint8_t y)
{
	uint32_t addr = TARGET_ADDR_LO;

	NOC2AXITlbSetup(NOC_DMA_NOC_ID, NOC_DMA_TLB, x, y, addr);
}

/* program_noc_dma_tlb must be invoked before this func call */
static inline void write_noc_dma_config(uint32_t addr, uint32_t value)
{
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, addr, value);
}

/* program noc_dma_tlb must be invoked before this func call */
static inline uint32_t read_noc_dma_config(uint32_t addr)
{
	return NOC2AXIRead32(NOC_DMA_NOC_ID, NOC_DMA_TLB, addr);
}

static bool noc_wait_cmd_ready(void)
{
#ifdef CONFIG_BOARD_NATIVE_SIM
	/* Fake completion */
	return true;
#else
	uint32_t cmd_ctrl;
	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(NOC_DMA_TIMEOUT_MS));

	do {
		cmd_ctrl = read_noc_dma_config(CMD_CTRL);
	} while (cmd_ctrl != 0 && !sys_timepoint_expired(timeout));

	return cmd_ctrl == 0;
#endif
}

static uint32_t get_expected_acks(uint32_t noc_cmd, uint64_t size)
{
	uint32_t ack_reg_addr =
		(noc_cmd & NOC_CMD_WR) ? NIU_MST_WR_ACK_RECEIVED : NIU_MST_RD_RESP_RECEIVED;
	uint32_t packet_received = read_noc_dma_config(ack_reg_addr);
	uint32_t expected_acks = packet_received + DIV_ROUND_UP(size, NOC_MAX_BURST_SIZE);

	return expected_acks;
}

/* wrap around aware comparison for half-range rule */
static inline bool is_behind(uint32_t current, uint32_t target)
{
	/*
	 * "target" and "current" are NOC transaction counters, and may wrap around, so we must
	 * consider the case where target and current have wrapped a different number of times.
	 * There's no way to know how many times they have wrapped, instead we assume that they are
	 * within 2**31 of each other as that gives an unambiguous ordering.
	 *
	 * We deal with this by considering
	 * target - 2**31 <  current < target         MOD 2**32 as before target and
	 * target         <= current < target + 2**31 MOD 2**32 as after target.
	 *
	 * We can't just check target == current because just one spurious NOC transaction could
	 * result in the loop hanging with current = target+1.
	 */
	return (int32_t)(current - target) < 0;
}

static bool wait_noc_dma_done(uint32_t noc_cmd, uint32_t expected_acks)
{
#ifdef CONFIG_BOARD_NATIVE_SIM
	/* Fake NOC completion */
	return true;
#else
	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(NOC_DMA_TIMEOUT_MS));
	uint32_t ack_reg_addr =
		(noc_cmd & NOC_CMD_WR) ? NIU_MST_WR_ACK_RECEIVED : NIU_MST_RD_RESP_RECEIVED;
	uint32_t ack_received;
	bool behind;

	do {
		ack_received = read_noc_dma_config(ack_reg_addr);
		behind = is_behind(ack_received, expected_acks);
	} while (behind && !sys_timepoint_expired(timeout));

	return !behind;
#endif
}

static uint32_t noc_dma_format_coord(uint8_t x, uint8_t y)
{
	return (union ret_addr_hi_u){
		.f = {.end_x = x, .end_y = y}}
		.u;
}

static bool noc_dma_transfer(uint32_t cmd, uint32_t ret_coord, uint64_t ret_addr,
			     uint32_t targ_coord, uint64_t targ_addr, uint32_t size, bool multicast,
			     uint8_t transaction_id, bool include_self, bool wait_for_done)
{
	uint32_t ret_addr_lo = low32(ret_addr);
	uint32_t ret_addr_mid = high32(ret_addr);
	uint32_t ret_addr_hi = ret_coord;

	uint32_t targ_addr_lo = low32(targ_addr);
	uint32_t targ_addr_mid = high32(targ_addr);
	uint32_t targ_addr_hi = targ_coord;

	uint32_t noc_at_len_be = size;
	uint32_t noc_packet_tag = transaction_id << 10;

	uint32_t noc_ctrl = NOC_CMD_CPY | cmd;
	uint32_t expected_acks;

	if (multicast) {
		noc_ctrl |= NOC_CMD_PATH_RESERVE | NOC_CMD_BRCST_PACKET;

		if (include_self) {
			noc_ctrl |= NOC_CMD_BRCST_SRC_INCLUDE;
		}
	}

	if (wait_for_done) {
		noc_ctrl |= NOC_CMD_RESP_MARKED;
		expected_acks = get_expected_acks(noc_ctrl, size);
	}

	if (!noc_wait_cmd_ready()) {
		return false;
	}

	write_noc_dma_config(TARGET_ADDR_LO, targ_addr_lo);
	write_noc_dma_config(TARGET_ADDR_MID, targ_addr_mid);
	write_noc_dma_config(TARGET_ADDR_HI, targ_addr_hi);
	write_noc_dma_config(RET_ADDR_LO, ret_addr_lo);
	write_noc_dma_config(RET_ADDR_MID, ret_addr_mid);
	write_noc_dma_config(RET_ADDR_HI, ret_addr_hi);
	write_noc_dma_config(PACKET_TAG, noc_packet_tag);
	write_noc_dma_config(AT_LEN, noc_at_len_be);
	write_noc_dma_config(AT_LEN_1, 0);
	write_noc_dma_config(AT_DATA, 0);
	write_noc_dma_config(BRCST_EXCLUDE, 0);
	write_noc_dma_config(CMD_BRCST, noc_ctrl);
	write_noc_dma_config(CMD_CTRL, 1);

	if (wait_for_done && !wait_noc_dma_done(noc_ctrl, expected_acks)) {
		return false;
	}

	return true;
}

/*
 * Config the source and dest NOC coordinates, the source and dest addresses and
 * the size of data transfer.
 *
 * Transfer data using only one block for simplicity.
 */
static int noc_dma_config(const struct device *dev, uint32_t channel, struct dma_config *config)
{
	struct dma_block_config *block = config->head_block;
	struct tt_bh_dma_noc_coords *coords = (struct tt_bh_dma_noc_coords *)config->user_data;

	struct tt_bh_dma_noc_data *data = (struct tt_bh_dma_noc_data *)dev->data;

	data->block = *block;
	data->coords = *coords;

	return 0;
}

static int noc_dma_start(const struct device *dev, uint32_t channel)
{
	struct tt_bh_dma_noc_data *data = (struct tt_bh_dma_noc_data *)dev->data;

	uint32_t ret_coord = noc_dma_format_coord(data->coords.source_x, data->coords.source_y);
	uint64_t ret_addr = data->block.source_address;

	uint32_t targ_coord = noc_dma_format_coord(data->coords.dest_x, data->coords.dest_y);
	uint64_t targ_addr = data->block.dest_address;

	program_noc_dma_tlb(data->coords.dest_x, data->coords.dest_y);
	return noc_dma_transfer(NOC_CMD_WR, ret_coord, ret_addr, targ_coord, targ_addr,
				data->block.block_size, false, 0, false, false);
}

static int noc_dma_init(const struct device *dev)
{
	return 0;
}

static const struct dma_driver_api noc_dma_api = {
	.config = noc_dma_config,
	.reload = NULL,
	.start = noc_dma_start,
	.stop = NULL,
	.suspend = NULL,
	.resume = NULL,
	.get_status = NULL,
	.get_attribute = NULL,
	.chan_filter = NULL,
	.chan_release = NULL,
};

#define NOC_DMA_INIT(n)                                                                            \
	static const struct tt_bh_dma_noc_config noc_dma_config_##n = {};                          \
	static struct tt_bh_dma_noc_data noc_dma_data_##n = {};                                    \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, &noc_dma_init, NULL, &noc_dma_data_##n, &noc_dma_config_##n,      \
			      POST_KERNEL, CONFIG_DMA_INIT_PRIORITY, &noc_dma_api);

DT_INST_FOREACH_STATUS_OKAY(NOC_DMA_INIT)
