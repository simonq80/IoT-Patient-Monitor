/*
 * Copyright (c) 2017 RnDity Sp. z o.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _STM32F0X_FLASH_REGISTERS_H_
#define _STM32F0X_FLASH_REGISTERS_H_

#include <zephyr/types.h>

/**
 * @brief
 *
 * Based on reference manual:
 *   STM32F030x4/x6/x8/xC,
 *   STM32F070x6/xB advanced ARM ® -based MCUs
 *
 * Chapter 3.3.5: Embedded Flash Memory
 */

enum {
	STM32_FLASH_LATENCY_0 = 0x0,
	STM32_FLASH_LATENCY_1 = 0x1
};

/* 3.3.5.1 FLASH_ACR */
union ef_acr {
	u32_t val;
	struct {
		u32_t latency :3 __packed;
		u32_t rsvd__3 :1 __packed;
		u32_t prftbe :1 __packed;
		u32_t prftbs :1 __packed;
		u32_t rsvd__6_31 :26 __packed;
	} bit;
};

/* 3.3.5 Embedded flash registers */
struct stm32_flash {
	union ef_acr acr;
	u32_t keyr;
	u32_t optkeyr;
	u32_t sr;
	u32_t cr;
	u32_t ar;
	u32_t obr;
	u32_t wrpr;
};

#endif /* _STM32F0X_FLASH_REGISTERS_H_ */
