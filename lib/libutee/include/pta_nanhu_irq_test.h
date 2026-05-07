/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * BOSC Nanhu IRQ test pseudo TA interface.
 */

#ifndef __PTA_NANHU_IRQ_TEST_H
#define __PTA_NANHU_IRQ_TEST_H

#define PTA_NANHU_IRQ_TEST_UUID \
	{ 0x6d99f02a, 0x9f6a, 0x4c30, \
		{ 0xa8, 0x4b, 0x4e, 0x21, 0x44, 0x9c, 0x10, 0x01 } }

#define PTA_NANHU_IRQ_TEST_CMD_BUSY		0
#define PTA_NANHU_IRQ_TEST_CMD_TRIGGER_NS	1
#define PTA_NANHU_IRQ_TEST_CMD_TRIGGER_SEC	2
#define PTA_NANHU_IRQ_TEST_CMD_TRIGGER_SEC_WITH_NS	3

#endif /* __PTA_NANHU_IRQ_TEST_H */
