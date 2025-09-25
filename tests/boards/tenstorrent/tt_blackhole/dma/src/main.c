/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(test_noc_dma, LOG_LEVEL_DBG);

/* Test constants */
#define TEST_BUFFER_SIZE   1024
#define TEST_SMALL_SIZE    64
#define TEST_LARGE_SIZE    4096
#define TEST_PATTERN_8BIT  0x55
#define TEST_PATTERN_32BIT 0xDEADBEEF
#define MAX_TEST_CHANNELS  4
#define DMA_TIMEOUT_MS     1000

/* Test buffers - aligned to 4 bytes as required by NOC DMA */
static uint8_t __aligned(4) src_buffer[TEST_LARGE_SIZE];
static uint8_t __aligned(4) dst_buffer[TEST_LARGE_SIZE];
static uint8_t __aligned(4) backup_buffer[TEST_LARGE_SIZE];

/* DMA device under test */
static const struct device *dma_dev;

/* Test completion tracking */
static volatile bool dma_done;
static volatile int dma_status;
static struct k_sem dma_sem;

/* DMA callback function */
static void dma_callback(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	LOG_DBG("DMA callback: channel=%u, status=%d", channel, status);

	dma_done = true;
	dma_status = status;
	k_sem_give(&dma_sem);
}

/* Initialize test buffers with patterns */
static void init_test_buffers(void)
{
	/* Fill source buffer with test pattern */
	for (size_t i = 0; i < sizeof(src_buffer); i++) {
		src_buffer[i] = (uint8_t)(i & 0xFF);
	}

	/* Clear destination buffer */
	memset(dst_buffer, 0, sizeof(dst_buffer));

	/* Backup original destination for comparison */
	memcpy(backup_buffer, dst_buffer, sizeof(backup_buffer));
}

/* Verify DMA transfer results */
static void verify_transfer(size_t size, size_t offset)
{
	/* Check transferred data */
	for (size_t i = 0; i < size; i++) {
		zassert_equal(dst_buffer[offset + i], src_buffer[offset + i],
			      "Mismatch at offset %zu: expected 0x%02x, got 0x%02x", offset + i,
			      src_buffer[offset + i], dst_buffer[offset + i]);
	}

	/* Check that untransferred areas remain unchanged */
	if (offset > 0) {
		zassert_mem_equal(dst_buffer, backup_buffer, offset,
				  "Destination modified before transfer area");
	}

	if (offset + size < sizeof(dst_buffer)) {
		zassert_mem_equal(&dst_buffer[offset + size], &backup_buffer[offset + size],
				  sizeof(dst_buffer) - (offset + size),
				  "Destination modified after transfer area");
	}
}

/* Wait for DMA completion with timeout */
static int wait_for_dma_completion(void)
{
	int ret = k_sem_take(&dma_sem, K_MSEC(DMA_TIMEOUT_MS));
	if (ret != 0) {
		LOG_ERR("DMA completion timeout");
		return -ETIMEDOUT;
	}
	return 0;
}

/* Setup function called before each test */
static void *dma_setup(void)
{
	dma_dev = DEVICE_DT_GET(DT_NODELABEL(noc_dma));
	zassert_not_null(dma_dev, "DMA device not found");
	zassert_true(device_is_ready(dma_dev), "DMA device not ready");

	k_sem_init(&dma_sem, 0, 1);

	init_test_buffers();

	return NULL;
}

/* Teardown function called after each test */
static void dma_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Reset completion flags */
	dma_done = false;
	dma_status = 0;
}

ZTEST_SUITE(dma_noc_basic, NULL, dma_setup, NULL, dma_teardown, NULL);

/**
 * @brief Test DMA device initialization and basic properties
 */
ZTEST(dma_noc_basic, test_dma_device_ready)
{
	zassert_not_null(dma_dev, "DMA device should not be NULL");
	zassert_true(device_is_ready(dma_dev), "DMA device should be ready");
}

/**
 * @brief Test DMA attribute queries
 */
ZTEST(dma_noc_basic, test_dma_attributes)
{
	uint32_t value;
	int ret;

	/* Test buffer address alignment */
	ret = dma_get_attribute(dma_dev, DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT, &value);
	zassert_ok(ret, "Failed to get address alignment attribute");
	zassert_equal(value, 4, "Expected 4-byte address alignment");

	/* Test buffer size alignment */
	ret = dma_get_attribute(dma_dev, DMA_ATTR_BUFFER_SIZE_ALIGNMENT, &value);
	zassert_ok(ret, "Failed to get size alignment attribute");
	zassert_equal(value, 4, "Expected 4-byte size alignment");

	/* Test copy alignment */
	ret = dma_get_attribute(dma_dev, DMA_ATTR_COPY_ALIGNMENT, &value);
	zassert_ok(ret, "Failed to get copy alignment attribute");
	zassert_equal(value, 4, "Expected 4-byte copy alignment");

	/* Test max block count */
	ret = dma_get_attribute(dma_dev, DMA_ATTR_MAX_BLOCK_COUNT, &value);
	zassert_ok(ret, "Failed to get max block count attribute");
	zassert_equal(value, 1024, "Expected max 1024 blocks");

	/* Test invalid attribute */
	ret = dma_get_attribute(dma_dev, 0xFFFF, &value);
	zassert_equal(ret, -ENOTSUP, "Invalid attribute should return -ENOTSUP");
}

/**
 * @brief Test channel configuration with different channel IDs
 */
ZTEST(dma_noc_basic, test_dma_channel_management)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	int ret;

	/* Configure basic transfer parameters */
	block_config.block_size = TEST_SMALL_SIZE;
	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer;
	block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	config.channel_direction = MEMORY_TO_MEMORY;
	config.source_data_size = 1;
	config.dest_data_size = 1;
	config.source_burst_length = 1;
	config.dest_burst_length = 1;
	config.head_block = &block_config;
	config.dma_callback = dma_callback;
	config.user_data = NULL;

	/* Test configuring different valid channel numbers */
	for (uint32_t channel = 0; channel < 4; channel++) {
		ret = dma_config(dma_dev, channel, &config);
		zassert_ok(ret, "Failed to configure channel %u", channel);

		ret = dma_start(dma_dev, channel);
		zassert_ok(ret, "Failed to start channel %u", channel);

		ret = wait_for_dma_completion();
		zassert_ok(ret, "DMA timeout on channel %u", channel);

		ret = dma_stop(dma_dev, channel);
		zassert_ok(ret, "Failed to stop channel %u", channel);

		/* Reset buffers for next iteration */
		init_test_buffers();
	}
}

/**
 * @brief Test basic memory-to-memory DMA transfer
 */
ZTEST(dma_noc_basic, test_dma_memory_to_memory)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	uint32_t channel = 0;
	int ret;

	/* Configure single block transfer */
	block_config.block_size = TEST_BUFFER_SIZE;
	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer;
	block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	config.channel_direction = MEMORY_TO_MEMORY;
	config.source_data_size = 1;
	config.dest_data_size = 1;
	config.source_burst_length = 1;
	config.dest_burst_length = 1;
	config.head_block = &block_config;
	config.dma_callback = dma_callback;
	config.user_data = NULL;

	/* Configure the channel */
	ret = dma_config(dma_dev, channel, &config);
	zassert_ok(ret, "Failed to configure DMA channel");

	/* Start the transfer */
	ret = dma_start(dma_dev, channel);
	zassert_ok(ret, "Failed to start DMA transfer");

	/* Wait for completion */
	ret = wait_for_dma_completion();
	zassert_ok(ret, "DMA transfer timeout");
	zassert_true(dma_done, "DMA should be completed");
	zassert_equal(dma_status, DMA_STATUS_COMPLETE, "Expected DMA_STATUS_COMPLETE");

	/* Verify the transfer */
	verify_transfer(TEST_BUFFER_SIZE, 0);

	/* Stop channel */
	ret = dma_stop(dma_dev, channel);
	zassert_ok(ret, "Failed to stop DMA");
}

/**
 * @brief Test DMA transfer with different sizes
 */
ZTEST(dma_noc_basic, test_dma_different_sizes)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	uint32_t channel = 1;
	int ret;

	size_t test_sizes[] = {4, 16, 64, 256, 1024, 2048};

	for (size_t i = 0; i < ARRAY_SIZE(test_sizes); i++) {
		size_t size = test_sizes[i];

		LOG_INF("Testing DMA transfer size: %zu bytes", size);

		/* Reset buffers */
		init_test_buffers();

		/* Configure transfer */
		block_config.block_size = size;
		block_config.source_address = (uintptr_t)src_buffer;
		block_config.dest_address = (uintptr_t)dst_buffer;
		block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

		config.channel_direction = MEMORY_TO_MEMORY;
		config.source_data_size = 1;
		config.dest_data_size = 1;
		config.source_burst_length = 1;
		config.dest_burst_length = 1;
		config.head_block = &block_config;
		config.dma_callback = dma_callback;
		config.user_data = NULL;

		ret = dma_config(dma_dev, channel, &config);
		zassert_ok(ret, "Failed to configure DMA for size %zu", size);

		ret = dma_start(dma_dev, channel);
		zassert_ok(ret, "Failed to start DMA for size %zu", size);

		ret = wait_for_dma_completion();
		zassert_ok(ret, "DMA timeout for size %zu", size);

		verify_transfer(size, 0);

		ret = dma_stop(dma_dev, channel);
		zassert_ok(ret, "Failed to stop DMA for size %zu", size);
	}
}

/**
 * @brief Test DMA transfer with different burst lengths
 */
ZTEST(dma_noc_basic, test_dma_burst_lengths)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	uint32_t channel = 2;
	int ret;

	uint32_t burst_lengths[] = {1, 4, 8, 16};

	for (size_t i = 0; i < ARRAY_SIZE(burst_lengths); i++) {
		uint32_t burst = burst_lengths[i];

		LOG_INF("Testing DMA burst length: %u", burst);

		/* Reset buffers */
		init_test_buffers();

		/* Configure transfer */
		block_config.block_size = TEST_BUFFER_SIZE;
		block_config.source_address = (uintptr_t)src_buffer;
		block_config.dest_address = (uintptr_t)dst_buffer;
		block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

		config.channel_direction = MEMORY_TO_MEMORY;
		config.source_data_size = 1;
		config.dest_data_size = 1;
		config.source_burst_length = burst;
		config.dest_burst_length = burst;
		config.head_block = &block_config;
		config.dma_callback = dma_callback;
		config.user_data = NULL;

		ret = dma_config(dma_dev, channel, &config);
		zassert_ok(ret, "Failed to configure DMA for burst %u", burst);

		ret = dma_start(dma_dev, channel);
		zassert_ok(ret, "Failed to start DMA for burst %u", burst);

		ret = wait_for_dma_completion();
		zassert_ok(ret, "DMA timeout for burst %u", burst);

		verify_transfer(TEST_BUFFER_SIZE, 0);

		ret = dma_stop(dma_dev, channel);
		zassert_ok(ret, "Failed to stop DMA for burst %u", burst);
	}

	dma_release_channel(dma_dev, channel);
}

/**
 * @brief Test multiple block DMA transfer
 */
ZTEST(dma_noc_basic, test_dma_multiple_blocks)
{
	struct dma_block_config block1 = {0};
	struct dma_block_config block2 = {0};
	struct dma_block_config block3 = {0};
	struct dma_config config = {0};
	int channel;
	int ret;

	const size_t block_size = TEST_BUFFER_SIZE / 4;

	/* Request a channel */
	channel = dma_request_channel(dma_dev, NULL);
	zassert_true(channel >= 0, "Failed to request channel");

	/* Configure first block */
	block1.block_size = block_size;
	block1.source_address = (uintptr_t)src_buffer;
	block1.dest_address = (uintptr_t)dst_buffer;
	block1.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block1.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block1.next_block = &block2;

	/* Configure second block */
	block2.block_size = block_size;
	block2.source_address = (uintptr_t)(src_buffer + block_size);
	block2.dest_address = (uintptr_t)(dst_buffer + block_size);
	block2.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block2.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block2.next_block = &block3;

	/* Configure third block */
	block3.block_size = block_size;
	block3.source_address = (uintptr_t)(src_buffer + 2 * block_size);
	block3.dest_address = (uintptr_t)(dst_buffer + 2 * block_size);
	block3.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block3.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block3.next_block = NULL;

	config.channel_direction = MEMORY_TO_MEMORY;
	config.source_data_size = 1;
	config.dest_data_size = 1;
	config.source_burst_length = 1;
	config.dest_burst_length = 1;
	config.head_block = &block1;
	config.dma_callback = dma_callback;
	config.user_data = NULL;

	ret = dma_config(dma_dev, channel, &config);
	zassert_ok(ret, "Failed to configure multi-block DMA");

	ret = dma_start(dma_dev, channel);
	zassert_ok(ret, "Failed to start multi-block DMA");

	/* Wait for completion of all blocks */
	ret = wait_for_dma_completion();
	zassert_ok(ret, "Multi-block DMA timeout");

	/* Verify all blocks were transferred */
	verify_transfer(3 * block_size, 0);

	ret = dma_stop(dma_dev, channel);
	zassert_ok(ret, "Failed to stop multi-block DMA");

	dma_release_channel(dma_dev, channel);
}

/**
 * @brief Test DMA reload functionality
 */
ZTEST(dma_noc_basic, test_dma_reload)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	int channel;
	int ret;

	/* Request a channel */
	channel = dma_request_channel(dma_dev, NULL);
	zassert_true(channel >= 0, "Failed to request channel");

	/* Initial configuration */
	block_config.block_size = TEST_SMALL_SIZE;
	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer;
	block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	config.channel_direction = MEMORY_TO_MEMORY;
	config.source_data_size = 1;
	config.dest_data_size = 1;
	config.source_burst_length = 1;
	config.dest_burst_length = 1;
	config.head_block = &block_config;
	config.dma_callback = dma_callback;
	config.user_data = NULL;

	ret = dma_config(dma_dev, channel, &config);
	zassert_ok(ret, "Failed to configure DMA for reload test");

	/* Test reload with new addresses and size */
	ret = dma_reload(dma_dev, channel, (uintptr_t)(src_buffer + 100),
			 (uintptr_t)(dst_buffer + 200), TEST_SMALL_SIZE);
	zassert_ok(ret, "Failed to reload DMA");

	/* Start the transfer with reloaded parameters */
	ret = dma_start(dma_dev, channel);
	zassert_ok(ret, "Failed to start reloaded DMA");

	ret = wait_for_dma_completion();
	zassert_ok(ret, "Reloaded DMA timeout");

	/* Verify transfer at new locations */
	verify_transfer(TEST_SMALL_SIZE, 200);

	ret = dma_stop(dma_dev, channel);
	zassert_ok(ret, "Failed to stop reloaded DMA");

	dma_release_channel(dma_dev, channel);
}

/**
 * @brief Test DMA status queries
 */
ZTEST(dma_noc_basic, test_dma_status)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	struct dma_status status;
	int channel;
	int ret;

	/* Request a channel */
	channel = dma_request_channel(dma_dev, NULL);
	zassert_true(channel >= 0, "Failed to request channel");

	/* Configure transfer */
	block_config.block_size = TEST_BUFFER_SIZE;
	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer;
	block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	config.channel_direction = MEMORY_TO_MEMORY;
	config.source_data_size = 1;
	config.dest_data_size = 1;
	config.source_burst_length = 1;
	config.dest_burst_length = 1;
	config.head_block = &block_config;
	config.dma_callback = dma_callback;
	config.user_data = NULL;

	ret = dma_config(dma_dev, channel, &config);
	zassert_ok(ret, "Failed to configure DMA");

	/* Check status before start */
	ret = dma_get_status(dma_dev, channel, &status);
	zassert_ok(ret, "Failed to get DMA status");
	zassert_false(status.busy, "DMA should not be busy before start");
	zassert_equal(status.dir, MEMORY_TO_MEMORY, "Wrong direction");

	/* Start transfer and check status */
	ret = dma_start(dma_dev, channel);
	zassert_ok(ret, "Failed to start DMA");

	/* Wait for completion and check final status */
	ret = wait_for_dma_completion();
	zassert_ok(ret, "DMA timeout");

	ret = dma_get_status(dma_dev, channel, &status);
	zassert_ok(ret, "Failed to get final DMA status");
	zassert_false(status.busy, "DMA should not be busy after completion");

	ret = dma_stop(dma_dev, channel);
	zassert_ok(ret, "Failed to stop DMA");

	dma_release_channel(dma_dev, channel);
}

ZTEST_SUITE(dma_noc_error, NULL, dma_setup, NULL, dma_teardown, NULL);

/**
 * @brief Test error conditions and invalid parameters
 */
ZTEST(dma_noc_error, test_dma_invalid_parameters)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	struct dma_status status;
	int ret;

	/* Test with invalid channel numbers */
	ret = dma_config(dma_dev, 999, &config);
	zassert_equal(ret, -EINVAL, "Should reject invalid channel");

	ret = dma_start(dma_dev, 999);
	zassert_equal(ret, -EINVAL, "Should reject invalid channel for start");

	ret = dma_stop(dma_dev, 999);
	zassert_equal(ret, -EINVAL, "Should reject invalid channel for stop");

	ret = dma_get_status(dma_dev, 999, &status);
	zassert_equal(ret, -EINVAL, "Should reject invalid channel for status");

	/* Test with NULL configuration */
	ret = dma_config(dma_dev, 0, NULL);
	zassert_equal(ret, -EINVAL, "Should reject NULL config");

	/* Test with NULL head_block */
	config.head_block = NULL;
	ret = dma_config(dma_dev, 0, &config);
	zassert_equal(ret, -EINVAL, "Should reject NULL head_block");

	/* Test with invalid block size (not aligned) */
	block_config.block_size = 5; /* Not 4-byte aligned */
	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer;
	config.head_block = &block_config;

	ret = dma_config(dma_dev, 0, &config);
	zassert_equal(ret, -EINVAL, "Should reject unaligned block size");

	/* Test with unaligned addresses */
	block_config.block_size = 64;
	block_config.source_address = (uintptr_t)src_buffer + 1; /* Unaligned */
	block_config.dest_address = (uintptr_t)dst_buffer;

	ret = dma_config(dma_dev, 0, &config);
	zassert_equal(ret, -EINVAL, "Should reject unaligned source address");

	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer + 3; /* Unaligned */

	ret = dma_config(dma_dev, 0, &config);
	zassert_equal(ret, -EINVAL, "Should reject unaligned dest address");
}

/**
 * @brief Test channel busy conditions
 */
ZTEST(dma_noc_error, test_dma_channel_busy)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	int channel;
	int ret;

	/* Request a channel */
	channel = dma_request_channel(dma_dev, NULL);
	zassert_true(channel >= 0, "Failed to request channel");

	/* Configure a long transfer */
	block_config.block_size = TEST_LARGE_SIZE;
	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer;
	block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	config.channel_direction = MEMORY_TO_MEMORY;
	config.source_data_size = 1;
	config.dest_data_size = 1;
	config.source_burst_length = 1;
	config.dest_burst_length = 1;
	config.head_block = &block_config;
	config.dma_callback = dma_callback;
	config.user_data = NULL;

	ret = dma_config(dma_dev, channel, &config);
	zassert_ok(ret, "Failed to configure DMA");

	ret = dma_start(dma_dev, channel);
	zassert_ok(ret, "Failed to start DMA");

	/* Try to reconfigure while busy - should fail */
	ret = dma_config(dma_dev, channel, &config);
	zassert_equal(ret, -EBUSY, "Should reject config while busy");

	/* Try to start again while busy - should fail */
	ret = dma_start(dma_dev, channel);
	zassert_equal(ret, -EBUSY, "Should reject start while busy");

	/* Wait for completion */
	ret = wait_for_dma_completion();
	zassert_ok(ret, "DMA timeout");

	ret = dma_stop(dma_dev, channel);
	zassert_ok(ret, "Failed to stop DMA");

	dma_release_channel(dma_dev, channel);
}

/**
 * @brief Test operations on unconfigured channels
 */
ZTEST(dma_noc_error, test_dma_unconfigured_channel)
{
	int channel;
	int ret;

	/* Request a channel but don't configure it */
	channel = dma_request_channel(dma_dev, NULL);
	zassert_true(channel >= 0, "Failed to request channel");

	/* Try to start without configuration */
	ret = dma_start(dma_dev, channel);
	zassert_equal(ret, -EINVAL, "Should reject start on unconfigured channel");

	dma_release_channel(dma_dev, channel);
}

ZTEST_SUITE(dma_noc_performance, NULL, dma_setup, NULL, dma_teardown, NULL);

/**
 * @brief Test DMA performance with large transfers
 */
ZTEST(dma_noc_performance, test_dma_large_transfer)
{
	struct dma_block_config block_config = {0};
	struct dma_config config = {0};
	int channel;
	int ret;
	uint32_t start_time, end_time, duration_ms;

	/* Request a channel */
	channel = dma_request_channel(dma_dev, NULL);
	zassert_true(channel >= 0, "Failed to request channel");

	/* Configure large transfer */
	block_config.block_size = TEST_LARGE_SIZE;
	block_config.source_address = (uintptr_t)src_buffer;
	block_config.dest_address = (uintptr_t)dst_buffer;
	block_config.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block_config.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	config.channel_direction = MEMORY_TO_MEMORY;
	config.source_data_size = 1;
	config.dest_data_size = 1;
	config.source_burst_length = 16; /* Use maximum burst for performance */
	config.dest_burst_length = 16;
	config.head_block = &block_config;
	config.dma_callback = dma_callback;
	config.user_data = NULL;

	ret = dma_config(dma_dev, channel, &config);
	zassert_ok(ret, "Failed to configure large DMA transfer");

	/* Measure transfer time */
	start_time = k_uptime_get_32();

	ret = dma_start(dma_dev, channel);
	zassert_ok(ret, "Failed to start large DMA transfer");

	ret = wait_for_dma_completion();
	zassert_ok(ret, "Large DMA transfer timeout");

	end_time = k_uptime_get_32();
	duration_ms = end_time - start_time;

	/* Verify the transfer */
	verify_transfer(TEST_LARGE_SIZE, 0);

	/* Log performance metrics */
	uint32_t throughput_kbps = (TEST_LARGE_SIZE * 1000) / (duration_ms * 1024);
	LOG_INF("Large transfer (%u bytes) completed in %u ms, throughput: %u KB/s",
		TEST_LARGE_SIZE, duration_ms, throughput_kbps);

	/* Basic performance check - should complete in reasonable time */
	zassert_true(duration_ms < 1000, "Transfer took too long: %u ms", duration_ms);

	ret = dma_stop(dma_dev, channel);
	zassert_ok(ret, "Failed to stop large DMA transfer");

	dma_release_channel(dma_dev, channel);
}

/**
 * @brief Test concurrent DMA operations on multiple channels
 */
ZTEST(dma_noc_performance, test_dma_concurrent_channels)
{
	struct dma_block_config block_configs[MAX_TEST_CHANNELS];
	struct dma_config configs[MAX_TEST_CHANNELS];
	int channels[MAX_TEST_CHANNELS];
	int ret;

	/* Prepare separate buffer areas for each channel */
	const size_t channel_buffer_size = TEST_BUFFER_SIZE / MAX_TEST_CHANNELS;

	/* Request and configure multiple channels */
	for (int i = 0; i < MAX_TEST_CHANNELS; i++) {
		channels[i] = dma_request_channel(dma_dev, NULL);
		zassert_true(channels[i] >= 0, "Failed to request channel %d", i);

		/* Configure each channel with different buffer regions */
		size_t offset = i * channel_buffer_size;

		memset(&block_configs[i], 0, sizeof(block_configs[i]));
		block_configs[i].block_size = channel_buffer_size;
		block_configs[i].source_address = (uintptr_t)(src_buffer + offset);
		block_configs[i].dest_address = (uintptr_t)(dst_buffer + offset);
		block_configs[i].source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		block_configs[i].dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

		memset(&configs[i], 0, sizeof(configs[i]));
		configs[i].channel_direction = MEMORY_TO_MEMORY;
		configs[i].source_data_size = 1;
		configs[i].dest_data_size = 1;
		configs[i].source_burst_length = 4;
		configs[i].dest_burst_length = 4;
		configs[i].head_block = &block_configs[i];
		configs[i].dma_callback = dma_callback;
		configs[i].user_data = (void *)(intptr_t)i;

		ret = dma_config(dma_dev, channels[i], &configs[i]);
		zassert_ok(ret, "Failed to configure channel %d", i);
	}

	/* Start all channels concurrently */
	uint32_t start_time = k_uptime_get_32();

	for (int i = 0; i < MAX_TEST_CHANNELS; i++) {
		ret = dma_start(dma_dev, channels[i]);
		zassert_ok(ret, "Failed to start channel %d", i);
	}

	/* Wait for all completions */
	for (int i = 0; i < MAX_TEST_CHANNELS; i++) {
		ret = wait_for_dma_completion();
		zassert_ok(ret, "Timeout waiting for channel completion");
	}

	uint32_t end_time = k_uptime_get_32();
	uint32_t duration_ms = end_time - start_time;

	/* Verify all transfers */
	for (int i = 0; i < MAX_TEST_CHANNELS; i++) {
		size_t offset = i * channel_buffer_size;
		verify_transfer(channel_buffer_size, offset);

		ret = dma_stop(dma_dev, channels[i]);
		zassert_ok(ret, "Failed to stop channel %d", i);
	}

	LOG_INF("Concurrent transfers on %d channels completed in %u ms", MAX_TEST_CHANNELS,
		duration_ms);

	/* Release all channels */
	for (int i = 0; i < MAX_TEST_CHANNELS; i++) {
		dma_release_channel(dma_dev, channels[i]);
	}
}