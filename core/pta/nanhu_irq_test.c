// SPDX-License-Identifier: BSD-2-Clause
/*
 * BOSC Nanhu secure-world IRQ trigger pseudo TA.
 */

#include <io.h>
#include <drivers/plic.h>
#include <kernel/delay.h>
#include <kernel/pseudo_ta.h>
#include <kernel/thread.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <pta_nanhu_irq_test.h>
#include <tee_api_defines.h>
#include <tee_api_types.h>
#include <trace.h>

#define TA_NAME "nanhu_irq_test.pta"
#define NANHU_IRQ_TEST_BUSY_MS 100
#define NANHU_IRQ_TEST_POLL_US 100
#define NANHU_IRQ_TEST_TIMEOUT_US 100000

static TEE_Result check_no_params(uint32_t param_types)
{
	uint32_t expected = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
					   TEE_PARAM_TYPE_NONE,
					   TEE_PARAM_TYPE_NONE,
					   TEE_PARAM_TYPE_NONE);

	if (param_types != expected)
		return TEE_ERROR_BAD_PARAMETERS;

	return TEE_SUCCESS;
}

static TEE_Result busy(uint32_t param_types)
{
	TEE_Result res = check_no_params(param_types);

	if (res)
		return res;

	IMSG("[TEE-CALL] ENTER command=busy");
	IMSG("[optee-test] busy begin");
	mdelay(NANHU_IRQ_TEST_BUSY_MS);
	IMSG("[optee-test] busy end");
	IMSG("[TEE-CALL] EXIT command=busy result=0x%x", TEE_SUCCESS);

	return TEE_SUCCESS;
}

static TEE_Result wait_for_irq(uint32_t expected_irq)
{
	size_t tries = NANHU_IRQ_TEST_TIMEOUT_US / NANHU_IRQ_TEST_POLL_US;
	uint32_t irq = 0;

	while (tries--) {
		if (plic_was_irq_handled(expected_irq))
			return TEE_SUCCESS;

		irq = plic_it_handle();
		if (irq == expected_irq)
			return TEE_SUCCESS;

		udelay(NANHU_IRQ_TEST_POLL_US);
	}

	EMSG("[optee-test] timeout waiting for irq%" PRIu32, expected_irq);

	return TEE_ERROR_BUSY;
}

static TEE_Result trigger_irq(uint32_t param_types, paddr_t base,
			      uint32_t irq, const char *name,
			      uint32_t followup_irq)
{
	TEE_Result res = check_no_params(param_types);
	uint32_t exceptions = 0;
	vaddr_t va = 0;

	if (res)
		return res;

	va = core_mmu_get_va(base, MEM_AREA_IO_SEC, NANHU_IRQGEN_REG_SIZE);
	if (!va) {
		EMSG("[optee-test] failed to map %s irqgen pa=0x%" PRIxPA,
		     name, base);
		return TEE_ERROR_GENERIC;
	}

	IMSG("[TEE-CALL] TRIGGER_IRQ test=%s irq=%" PRIu32 " followup=%" PRIu32,
	     name, irq, followup_irq);
	IMSG("[optee-test] trigger %s begin", name);
	exceptions = thread_mask_exceptions(THREAD_EXCP_FOREIGN_INTR);
	plic_enable_current_irq(irq);
	if (followup_irq)
		plic_enable_current_irq(followup_irq);
	plic_clear_last_handled_irq();
	plic_nanhu_trace_set(true, name);
	plic_set_current_world_state(1);
	io_write32(va + NANHU_IRQGEN_TRIGGER, 1);
	res = wait_for_irq(irq);
	if (!res && followup_irq)
		res = wait_for_irq(followup_irq);
	plic_set_current_world_state(0);
	plic_nanhu_trace_set(false, NULL);
	thread_unmask_exceptions(exceptions);
	if (res)
		return res;

	mdelay(NANHU_IRQ_TEST_BUSY_MS);
	IMSG("[optee-test] trigger %s end", name);

	return TEE_SUCCESS;
}

static TEE_Result invoke_command(void *session __unused, uint32_t cmd,
				 uint32_t param_types,
				 TEE_Param params[TEE_NUM_PARAMS] __unused)
{
	TEE_Result res = TEE_SUCCESS;
	const char *name = "unknown";

	switch (cmd) {
	case PTA_NANHU_IRQ_TEST_CMD_BUSY:
		return busy(param_types);
	case PTA_NANHU_IRQ_TEST_CMD_TRIGGER_NS:
		name = "trigger-ns";
		IMSG("[TEE-CALL] ENTER command=%s id=%" PRIu32, name, cmd);
		res = trigger_irq(param_types, NANHU_IRQGEN_NS2_BASE,
				  44, "trigger-ns", 0);
		IMSG("[TEE-CALL] EXIT command=%s result=0x%x", name, res);
		return res;
	case PTA_NANHU_IRQ_TEST_CMD_TRIGGER_SEC:
		name = "trigger-sec";
		IMSG("[TEE-CALL] ENTER command=%s id=%" PRIu32, name, cmd);
		res = trigger_irq(param_types, NANHU_IRQGEN_SEC2_BASE,
				  45, "trigger-sec", 0);
		IMSG("[TEE-CALL] EXIT command=%s result=0x%x", name, res);
		return res;
	case PTA_NANHU_IRQ_TEST_CMD_TRIGGER_SEC_WITH_NS:
		name = "trigger-sec-with-ns";
		IMSG("[TEE-CALL] ENTER command=%s id=%" PRIu32, name, cmd);
		nanhu_irq_test_set_trigger_ns_in_secure_handler(true);
		res = trigger_irq(param_types, NANHU_IRQGEN_SEC2_BASE,
				  45, "trigger-sec-with-ns", 44);
		nanhu_irq_test_set_trigger_ns_in_secure_handler(false);
		IMSG("[TEE-CALL] EXIT command=%s result=0x%x", name, res);
		return res;
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}
}

pseudo_ta_register(.uuid = PTA_NANHU_IRQ_TEST_UUID, .name = TA_NAME,
		   .flags = PTA_DEFAULT_FLAGS,
		   .invoke_command_entry_point = invoke_command);
