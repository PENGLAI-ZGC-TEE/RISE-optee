/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2022-2023 NXP
 */

#ifndef __DRIVERS_PLIC_H
#define __DRIVERS_PLIC_H

#include <kernel/interrupt.h>
#include <kernel/misc.h>
#include <platform_config.h>
#include <stdbool.h>

void plic_init(paddr_t plic_base_pa);
void plic_hart_init(void);
uint32_t plic_it_handle(void);
void plic_enable_current_irq(uint32_t source);
void plic_clear_last_handled_irq(void);
uint32_t plic_get_last_handled_irq(void);
bool plic_was_irq_handled(uint32_t source);
void plic_set_current_world_state(uint32_t ws);
void plic_dump_state(void);
void plic_nanhu_trace_set(bool active, const char *test);
bool plic_nanhu_trace_active(void);
const char *plic_nanhu_trace_test(void);

#endif /*__DRIVERS_PLIC_H*/
