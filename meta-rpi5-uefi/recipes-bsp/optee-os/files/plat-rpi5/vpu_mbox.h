/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * BCM2712 VPU mailbox (property channel) driver -- OP-TEE as the
 * post-firmware owner.
 *
 * OWNERSHIP MODEL (there is no bus firewall on this SoC, so this is a
 * convention every party must honor):
 *   - firmware phase: the mailbox belongs to the NORMAL world. EDK2's
 *     RpiFirmwareDxe drives it natively (framebuffer, board revision,
 *     MAC, SD clocks, ...); OP-TEE refuses every call here with
 *     TEE_ERROR_BAD_STATE.
 *   - at ExitBootServices EDK2 hands the mailbox over
 *     (PTA_BMC_SENSOR_CMD_MBOX_HANDOFF, the same late-init pattern as
 *     the RP1 BAR), after which OP-TEE is the ONLY mailbox user: the OS
 *     device tree disables the mailbox and firmware nodes and consumes
 *     the services (firmware clocks, later PMIC telemetry) over SCMI.
 *
 * The driver is polled -- no interrupt anywhere in the path -- which is
 * part of the point: the runtime failures this replaces were lost
 * mailbox-response interrupts. Calls are spinlocked and safe from the
 * SCMI fastcall (atomic) context; the poll is bounded (~10 ms) so a
 * wedged VPU costs a bounded stall, not a hang.
 */

#ifndef VPU_MBOX_H
#define VPU_MBOX_H

#include <stdbool.h>
#include <stdint.h>
#include <tee_api_types.h>

/*
 * Mark the mailbox as handed over by the normal world (one-way; called
 * from the pTA when EDK2 signals ExitBootServices).
 */
void vpu_mbox_set_owned(void);
bool vpu_mbox_owned(void);

/*
 * One property-tag call: send @req_len bytes of request data for @tag,
 * receive up to @resp_cap bytes back. Fails with TEE_ERROR_BAD_STATE
 * before the handoff, TEE_ERROR_BUSY on poll timeout,
 * TEE_ERROR_COMMUNICATION on a firmware error response.
 */
TEE_Result vpu_mbox_prop_call(uint32_t tag, const uint32_t *req,
			      size_t req_len, uint32_t *resp, size_t resp_cap);

/* Firmware clock ids (mailbox property interface) */
#define VPU_CLOCK_EMMC		1
#define VPU_CLOCK_UART		2
#define VPU_CLOCK_ARM		3
#define VPU_CLOCK_CORE		4
#define VPU_CLOCK_V3D		5
#define VPU_CLOCK_EMMC2		12

/* GET_CLOCK_RATE / SET_CLOCK_RATE wrappers; rate in Hz, 0 on failure */
uint32_t vpu_clock_get_rate(uint32_t clock_id);
TEE_Result vpu_clock_set_rate(uint32_t clock_id, uint32_t hz);
uint32_t vpu_clock_get_max_rate(uint32_t clock_id);

#endif /* VPU_MBOX_H */
