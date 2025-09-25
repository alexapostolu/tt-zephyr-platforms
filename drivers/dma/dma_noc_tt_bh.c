/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_noc_dma

#include <errno.h>
#include <sys/util.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(dma_noc_tt_bh, CONFIG_DMA_LOG_LEVEL);

/* NOC DMA Register Offsets */
#define NOC_DMA_CTRL_REG        0x00
#define NOC_DMA_STATUS_REG      0x04
#define NOC_DMA_SRC_ADDR_REG    0x08
#define NOC_DMA_DST_ADDR_REG    0x0C
#define NOC_DMA_SIZE_REG        0x10
#define NOC_DMA_INT_EN_REG      0x14
#define NOC_DMA_INT_STATUS_REG  0x18
#define NOC_DMA_INT_CLEAR_REG   0x1C

/* NOC DMA Control Register Bits */
#define NOC_DMA_CTRL_ENABLE     BIT(0)
#define NOC_DMA_CTRL_START      BIT(1)
#define NOC_DMA_CTRL_RESET      BIT(2)
#define NOC_DMA_CTRL_DIR_MASK   GENMASK(5, 4)
#define NOC_DMA_CTRL_DIR_M2M    (0 << 4)
#define NOC_DMA_CTRL_DIR_M2P    (1 << 4)
#define NOC_DMA_CTRL_DIR_P2M    (2 << 4)
#define NOC_DMA_CTRL_DIR_P2P    (3 << 4)
#define NOC_DMA_CTRL_BURST_MASK GENMASK(9, 8)
#define NOC_DMA_CTRL_BURST_1    (0 << 8)
#define NOC_DMA_CTRL_BURST_4    (1 << 8)
#define NOC_DMA_CTRL_BURST_8    (2 << 8)
#define NOC_DMA_CTRL_BURST_16   (3 << 8)

/* NOC DMA Status Register Bits */
#define NOC_DMA_STATUS_BUSY     BIT(0)
#define NOC_DMA_STATUS_DONE     BIT(1)
#define NOC_DMA_STATUS_ERROR    BIT(2)

/* NOC DMA Interrupt Bits */
#define NOC_DMA_INT_DONE        BIT(0)
#define NOC_DMA_INT_ERROR       BIT(1)

/* NOC DMA Configuration */
#define NOC_DMA_MAX_CHANNELS    8
#define NOC_DMA_MAX_BLOCK_COUNT 1024
#define NOC_DMA_ADDR_ALIGNMENT  4
#define NOC_DMA_SIZE_ALIGNMENT  4
#define NOC_DMA_COPY_ALIGNMENT  4

struct noc_dma_channel {
	bool in_use;
	bool busy;
	dma_callback_t callback;
	void *user_data;
	struct dma_config config;
	struct dma_block_config *block_config;
	uint32_t num_blocks;
	uint32_t current_block;
};

struct noc_dma_config {
	uintptr_t base;
	uint32_t irq;
	void (*irq_config_func)(const struct device *dev);
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct noc_dma_data {
	struct dma_context dma_ctx;
	struct noc_dma_channel channels[NOC_DMA_MAX_CHANNELS];
	uint32_t channel_mask;
};

static inline uint32_t noc_dma_read(const struct device *dev, uint32_t offset)
{
	const struct noc_dma_config *config = dev->config;
	return sys_read32(config->base + offset);
}

static inline void noc_dma_write(const struct device *dev, uint32_t offset, uint32_t value)
{
	const struct noc_dma_config *config = dev->config;
	sys_write32(value, config->base + offset);
}

static uint32_t noc_dma_get_direction_bits(enum dma_channel_direction direction)
{
	switch (direction) {
	case MEMORY_TO_MEMORY:
		return NOC_DMA_CTRL_DIR_M2M;
	case MEMORY_TO_PERIPHERAL:
		return NOC_DMA_CTRL_DIR_M2P;
	case PERIPHERAL_TO_MEMORY:
		return NOC_DMA_CTRL_DIR_P2M;
	case PERIPHERAL_TO_PERIPHERAL:
		return NOC_DMA_CTRL_DIR_P2P;
	default:
		return NOC_DMA_CTRL_DIR_M2M;
	}
}

static uint32_t noc_dma_get_burst_bits(uint32_t burst_length)
{
	switch (burst_length) {
	case 1:
		return NOC_DMA_CTRL_BURST_1;
	case 4:
		return NOC_DMA_CTRL_BURST_4;
	case 8:
		return NOC_DMA_CTRL_BURST_8;
	case 16:
		return NOC_DMA_CTRL_BURST_16;
	default:
		return NOC_DMA_CTRL_BURST_1;
	}
}

static int noc_dma_config_channel(const struct device *dev, uint32_t channel,
				  struct dma_config *config)
{
	struct noc_dma_data *data = dev->data;
	struct noc_dma_channel *chan;

	if (channel >= NOC_DMA_MAX_CHANNELS) {
		LOG_ERR("Invalid channel %u", channel);
		return -EINVAL;
	}

	chan = &data->channels[channel];

	if (chan->busy) {
		LOG_ERR("Channel %u is busy", channel);
		return -EBUSY;
	}

	if (!config || !config->head_block) {
		LOG_ERR("Invalid configuration");
		return -EINVAL;
	}

	/* Validate block configuration */
	struct dma_block_config *block = config->head_block;
	uint32_t block_count = 0;

	while (block) {
		if (block->block_size == 0 || block->block_size % NOC_DMA_SIZE_ALIGNMENT != 0) {
			LOG_ERR("Invalid block size %u", block->block_size);
			return -EINVAL;
		}

		if (block->source_address % NOC_DMA_ADDR_ALIGNMENT != 0 ||
		    block->dest_address % NOC_DMA_ADDR_ALIGNMENT != 0) {
			LOG_ERR("Unaligned addresses");
			return -EINVAL;
		}

		block_count++;
		if (block_count > NOC_DMA_MAX_BLOCK_COUNT) {
			LOG_ERR("Too many blocks");
			return -EINVAL;
		}

		block = block->next_block;
	}

	chan->config = *config;
	chan->block_config = config->head_block;
	chan->num_blocks = block_count;
	chan->current_block = 0;
	chan->callback = config->dma_callback;
	chan->user_data = config->user_data;
	chan->in_use = true;

	LOG_DBG("Configured channel %u with %u blocks", channel, block_count);

	return 0;
}

static int noc_dma_start_transfer(const struct device *dev, uint32_t channel)
{
	struct noc_dma_data *data = dev->data;
	struct noc_dma_channel *chan;
	struct dma_block_config *block;
	uint32_t ctrl_reg;

	if (channel >= NOC_DMA_MAX_CHANNELS) {
		return -EINVAL;
	}

	chan = &data->channels[channel];

	if (!chan->in_use) {
		LOG_ERR("Channel %u not configured", channel);
		return -EINVAL;
	}

	if (chan->busy) {
		LOG_ERR("Channel %u already busy", channel);
		return -EBUSY;
	}

	if (chan->current_block >= chan->num_blocks) {
		LOG_ERR("No more blocks to transfer");
		return -EINVAL;
	}

	block = chan->block_config;
	for (uint32_t i = 0; i < chan->current_block; i++) {
		block = block->next_block;
	}

	/* Configure the transfer */
	noc_dma_write(dev, NOC_DMA_SRC_ADDR_REG, block->source_address);
	noc_dma_write(dev, NOC_DMA_DST_ADDR_REG, block->dest_address);
	noc_dma_write(dev, NOC_DMA_SIZE_REG, block->block_size);

	/* Build control register value */
	ctrl_reg = NOC_DMA_CTRL_ENABLE | NOC_DMA_CTRL_START;
	ctrl_reg |= noc_dma_get_direction_bits(chan->config.channel_direction);
	
	if (chan->config.source_burst_length > 0) {
		ctrl_reg |= noc_dma_get_burst_bits(chan->config.source_burst_length);
	}

	/* Enable interrupts */
	noc_dma_write(dev, NOC_DMA_INT_EN_REG, NOC_DMA_INT_DONE | NOC_DMA_INT_ERROR);

	/* Clear any pending interrupts */
	noc_dma_write(dev, NOC_DMA_INT_CLEAR_REG, NOC_DMA_INT_DONE | NOC_DMA_INT_ERROR);

	/* Start the transfer */
	noc_dma_write(dev, NOC_DMA_CTRL_REG, ctrl_reg);

	chan->busy = true;

	LOG_DBG("Started transfer on channel %u, block %u/%u, size %u",
		channel, chan->current_block + 1, chan->num_blocks, block->block_size);

	return 0;
}

static int noc_dma_start(const struct device *dev, uint32_t channel)
{
	return noc_dma_start_transfer(dev, channel);
}

static int noc_dma_stop(const struct device *dev, uint32_t channel)
{
	struct noc_dma_data *data = dev->data;
	struct noc_dma_channel *chan;

	if (channel >= NOC_DMA_MAX_CHANNELS) {
		return -EINVAL;
	}

	chan = &data->channels[channel];

	if (!chan->in_use) {
		return -EINVAL;
	}

	/* Disable DMA controller */
	noc_dma_write(dev, NOC_DMA_CTRL_REG, NOC_DMA_CTRL_RESET);

	/* Disable interrupts */
	noc_dma_write(dev, NOC_DMA_INT_EN_REG, 0);

	/* Clear pending interrupts */
	noc_dma_write(dev, NOC_DMA_INT_CLEAR_REG, NOC_DMA_INT_DONE | NOC_DMA_INT_ERROR);

	chan->busy = false;
	chan->current_block = 0;

	LOG_DBG("Stopped channel %u", channel);

	return 0;
}

static int noc_dma_reload(const struct device *dev, uint32_t channel,
			  uint32_t src, uint32_t dst, size_t size)
{
	struct noc_dma_data *data = dev->data;
	struct noc_dma_channel *chan;

	if (channel >= NOC_DMA_MAX_CHANNELS) {
		return -EINVAL;
	}

	chan = &data->channels[channel];

	if (!chan->in_use || chan->busy) {
		return -EINVAL;
	}

	if (size == 0 || size % NOC_DMA_SIZE_ALIGNMENT != 0) {
		return -EINVAL;
	}

	if (src % NOC_DMA_ADDR_ALIGNMENT != 0 || dst % NOC_DMA_ADDR_ALIGNMENT != 0) {
		return -EINVAL;
	}

	/* Update the current block configuration */
	if (chan->block_config) {
		struct dma_block_config *block = chan->block_config;
		for (uint32_t i = 0; i < chan->current_block; i++) {
			block = block->next_block;
		}
		
		if (block) {
			block->source_address = src;
			block->dest_address = dst;
			block->block_size = size;
		}
	}

	LOG_DBG("Reloaded channel %u: src=0x%x, dst=0x%x, size=%zu", channel, src, dst, size);

	return 0;
}

static int noc_dma_get_status(const struct device *dev, uint32_t channel,
			      struct dma_status *status)
{
	struct noc_dma_data *data = dev->data;
	struct noc_dma_channel *chan;
	uint32_t hw_status;

	if (channel >= NOC_DMA_MAX_CHANNELS || !status) {
		return -EINVAL;
	}

	chan = &data->channels[channel];

	if (!chan->in_use) {
		return -EINVAL;
	}

	hw_status = noc_dma_read(dev, NOC_DMA_STATUS_REG);

	status->busy = chan->busy && (hw_status & NOC_DMA_STATUS_BUSY);
	status->dir = chan->config.channel_direction;
	status->pending_length = 0; /* Not supported by this hardware */

	if (chan->busy && (hw_status & NOC_DMA_STATUS_DONE)) {
		status->busy = false;
	}

	return 0;
}

static int noc_dma_get_attribute(const struct device *dev, uint32_t type, uint32_t *value)
{
	if (!value) {
		return -EINVAL;
	}

	switch (type) {
	case DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT:
		*value = NOC_DMA_ADDR_ALIGNMENT;
		break;
	case DMA_ATTR_BUFFER_SIZE_ALIGNMENT:
		*value = NOC_DMA_SIZE_ALIGNMENT;
		break;
	case DMA_ATTR_COPY_ALIGNMENT:
		*value = NOC_DMA_COPY_ALIGNMENT;
		break;
	case DMA_ATTR_MAX_BLOCK_COUNT:
		*value = NOC_DMA_MAX_BLOCK_COUNT;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

/* Channel management functions - not part of standard DMA API */
int noc_dma_request_channel(const struct device *dev, void *filter_param)
{
	struct noc_dma_data *data = dev->data;

	ARG_UNUSED(filter_param);

	for (uint32_t i = 0; i < NOC_DMA_MAX_CHANNELS; i++) {
		if (!data->channels[i].in_use) {
			data->channels[i].in_use = true;
			data->channel_mask |= BIT(i);
			LOG_DBG("Allocated channel %u", i);
			return i;
		}
	}

	LOG_WRN("No available channels");
	return -ENODEV;
}

void noc_dma_release_channel(const struct device *dev, uint32_t channel)
{
	struct noc_dma_data *data = dev->data;
	struct noc_dma_channel *chan;

	if (channel >= NOC_DMA_MAX_CHANNELS) {
		return;
	}

	chan = &data->channels[channel];

	if (chan->busy) {
		noc_dma_stop(dev, channel);
	}

	memset(chan, 0, sizeof(*chan));
	data->channel_mask &= ~BIT(channel);

	LOG_DBG("Released channel %u", channel);
}

int noc_dma_chan_filter(const struct device *dev, int channel, void *filter_param)
{
	ARG_UNUSED(filter_param);

	/* Basic channel availability check */
	if (channel < 0 || channel >= NOC_DMA_MAX_CHANNELS) {
		return 0;
	}

	/* Accept any valid channel for now */
	return 1;
}

static void noc_dma_isr(const struct device *dev)
{
	struct noc_dma_data *data = dev->data;
	uint32_t int_status;

	int_status = noc_dma_read(dev, NOC_DMA_INT_STATUS_REG);

	if (!int_status) {
		return;
	}

	/* Clear interrupts */
	noc_dma_write(dev, NOC_DMA_INT_CLEAR_REG, int_status);

	/* Find which channel caused the interrupt */
	for (uint32_t channel = 0; channel < NOC_DMA_MAX_CHANNELS; channel++) {
		struct noc_dma_channel *chan = &data->channels[channel];

		if (!chan->busy) {
			continue;
		}

		if (int_status & NOC_DMA_INT_ERROR) {
			LOG_ERR("DMA error on channel %u", channel);
			chan->busy = false;
			
			if (chan->callback) {
				chan->callback(dev, chan->user_data, channel, -EIO);
			}
		} else if (int_status & NOC_DMA_INT_DONE) {
			LOG_DBG("DMA completed on channel %u, block %u/%u", 
				channel, chan->current_block + 1, chan->num_blocks);
			
			chan->current_block++;
			
			if (chan->current_block >= chan->num_blocks) {
				/* All blocks completed */
				chan->busy = false;
				chan->current_block = 0;
				
				if (chan->callback) {
					chan->callback(dev, chan->user_data, channel, DMA_STATUS_COMPLETE);
				}
			} else {
				/* Start next block */
				if (chan->callback) {
					chan->callback(dev, chan->user_data, channel, DMA_STATUS_BLOCK);
				}
				noc_dma_start_transfer(dev, channel);
			}
		}
	}
}

static const struct dma_driver_api noc_dma_api = {
	.config = noc_dma_config_channel,
	.reload = noc_dma_reload,
	.start = noc_dma_start,
	.stop = noc_dma_stop,
	.get_status = noc_dma_get_status,
	.get_attribute = noc_dma_get_attribute,
};

static int noc_dma_init(const struct device *dev)
{
	const struct noc_dma_config *config = dev->config;
	struct noc_dma_data *data = dev->data;

	/* Initialize DMA context */
	data->dma_ctx.magic = DMA_MAGIC;
	data->dma_ctx.dma_channels = NOC_DMA_MAX_CHANNELS;
	data->dma_ctx.atomic = data;

	/* Reset the DMA controller */
	noc_dma_write(dev, NOC_DMA_CTRL_REG, NOC_DMA_CTRL_RESET);

	/* Disable all interrupts */
	noc_dma_write(dev, NOC_DMA_INT_EN_REG, 0);

	/* Clear all pending interrupts */
	noc_dma_write(dev, NOC_DMA_INT_CLEAR_REG, 0xFFFFFFFF);

	/* Enable clock if specified */
	if (config->clock_dev) {
		int ret = clock_control_on(config->clock_dev, config->clock_subsys);
		if (ret < 0) {
			LOG_ERR("Failed to enable clock: %d", ret);
			return ret;
		}
	}

	/* Configure IRQ */
	if (config->irq_config_func) {
		config->irq_config_func(dev);
	}

	LOG_INF("NOC DMA initialized with %d channels", NOC_DMA_MAX_CHANNELS);

	return 0;
}

/* Device instantiation macro */
#define NOC_DMA_INIT(n)								\
	static void noc_dma_irq_config_##n(const struct device *dev);		\
										\
	static const struct noc_dma_config noc_dma_config_##n = {		\
		.base = DT_INST_REG_ADDR(n),					\
		.irq = DT_INST_IRQN(n),						\
		.irq_config_func = noc_dma_irq_config_##n,			\
		.clock_dev = DEVICE_DT_GET_OR_NULL(DT_INST_CLOCKS_CTLR(n)),	\
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name), \
	};									\
										\
	static struct noc_dma_data noc_dma_data_##n;				\
										\
	DEVICE_DT_INST_DEFINE(n, &noc_dma_init, NULL,				\
			      &noc_dma_data_##n, &noc_dma_config_##n,		\
			      POST_KERNEL, CONFIG_DMA_INIT_PRIORITY,		\
			      &noc_dma_api);					\
										\
	static void noc_dma_irq_config_##n(const struct device *dev)		\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    noc_dma_isr, DEVICE_DT_INST_GET(n), 0);		\
		irq_enable(DT_INST_IRQN(n));					\
	}

DT_INST_FOREACH_STATUS_OKAY(NOC_DMA_INIT)
