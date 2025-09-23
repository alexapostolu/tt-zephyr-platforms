/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef THROTTLER_H
#define THROTTLER_H

#include <stdint.h>

void InitThrottlers(void);
void CalculateThrottlers(void);
int32_t Dm2CmSetBoardPowerLimit(const uint8_t *data, uint8_t size);

/* CSM Memory Telemetry Functions */
uint32_t ReadTelemetryFromCSM(uint32_t offset);
void WriteTelemetryToCSM(uint32_t offset, uint32_t value);

#endif
