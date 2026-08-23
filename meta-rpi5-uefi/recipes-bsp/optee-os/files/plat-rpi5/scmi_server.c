// SPDX-License-Identifier: BSD-2-Clause
/*
 * Raspberry Pi 5 SCMI server (plat hooks for the lightweight scmi-msg
 * stack). Exposes two protocols to the normal world over the SMT
 * shared-memory transport driven by a fast SiP SMC (Linux
 * "arm,scmi-smc"): TF-A's rpi_scmi_svc forwards RPI_SIP_SCMI_AGENT0 into
 * OP-TEE's fast-SMC vector, where tee_entry_fast (overridden below) runs
 * the SMT channel.
 *
 *   - Sensor Management (0x15): the BCM2712 die temperature, read from the
 *     AVS monitor. Read-only status register, so this never conflicts with
 *     a native Linux thermal driver reading the same register.
 *
 *   - Performance Domain (0x13): the RP1 Active Cooler fan as a 5-level
 *     performance domain. OP-TEE owns the RP1 PWM exclusively (the OS DT
 *     drops the pwm-fan), and Linux thermal delegates cooling via SCMI.
 *
 * One non-threaded SMT channel (fastcall entry runs in atomic context),
 * its 128-byte slot in a dedicated page at the top of the reserved SHM
 * window. The RP1 window is mapped by the sensor pTA's EDK2 handshake
 * (rp1_periph), which the fan controller shares.
 */

#include <config.h>
#include <drivers/scmi-msg.h>
#include <drivers/scmi.h>
#include <initcall.h>
#include <kernel/panic.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <sm/optee_smc.h>
#include <string.h>
#include <tee/entry_fast.h>
#include <trace.h>
#include <util.h>

#include "rp1_pwm.h"
#include "soc_temp.h"

#define RPI5_SCMI_CHANNEL_ID	0

/*
 * SiP fast SMC function ID Linux's arm,scmi-smc rings (arm,smc-id in the
 * DT). Forwarded into OP-TEE by TF-A's rpi_scmi_svc; keep the three in
 * step (TF-A 0002 patch, EDK2 FdtDxe, here).
 */
#define RPI_SIP_SCMI_AGENT0	0x82000010

/* SCMI sensor type: temperature in degrees Celsius (SCMI sensor type table) */
#define SCMI_SENSOR_TYPE_DEGREES_C	0x2

static_assert(SMT_BUF_SLOT_SIZE <= SCMI_SMT_SIZE);

register_phys_mem(MEM_AREA_IO_NSEC, SCMI_SMT_BASE, SCMI_SMT_SIZE);

/* Protocols advertised to the agent, NUL-terminated for the base protocol. */
static const uint8_t rpi5_scmi_protocols[] = {
	SCMI_PROTOCOL_ID_SENSOR,
#ifdef CFG_SCMI_MSG_PERF_DOMAIN
	SCMI_PROTOCOL_ID_PERF,
#endif
	0,
};

static struct scmi_msg_channel rpi5_scmi_channel = {
	.shm_addr = { .pa = SCMI_SMT_BASE },
	.shm_size = SMT_BUF_SLOT_SIZE,
	.threaded = false,
};

struct scmi_msg_channel *plat_scmi_get_channel(unsigned int channel_id)
{
	if (channel_id != RPI5_SCMI_CHANNEL_ID)
		return NULL;

	return &rpi5_scmi_channel;
}

/*
 * Fast-SMC hook: TF-A forwards the SCMI doorbell here with the SiP FID in
 * a0. Run the SMT channel (atomic context) and return OK; the response is
 * already in the shared page, and opteed lands a0 back in the caller.
 */
void tee_entry_fast(struct thread_smc_args *args)
{
	if (args->a0 == RPI_SIP_SCMI_AGENT0) {
		scmi_smt_fastcall_smc_entry(RPI5_SCMI_CHANNEL_ID);
		args->a0 = OPTEE_SMC_RETURN_OK;
		return;
	}

	__tee_entry_fast(args);
}

static TEE_Result rpi5_scmi_init(void)
{
	struct scmi_msg_channel *chan = &rpi5_scmi_channel;

	chan->shm_addr.va = (vaddr_t)phys_to_virt(chan->shm_addr.pa,
						  MEM_AREA_IO_NSEC,
						  SMT_BUF_SLOT_SIZE);
	if (!chan->shm_addr.va)
		panic("SCMI SMT shared memory not mapped");

	scmi_smt_init_agent_channel(chan);

	return TEE_SUCCESS;
}

driver_init_late(rpi5_scmi_init);

size_t plat_scmi_protocol_count(void)
{
	/* Excludes the trailing NUL and the always-present base protocol. */
	return ARRAY_SIZE(rpi5_scmi_protocols) - 1;
}

const uint8_t *plat_scmi_protocol_list(unsigned int channel_id __unused)
{
	return rpi5_scmi_protocols;
}

const char *plat_scmi_vendor_name(void)
{
	return "pi-bmc";
}

const char *plat_scmi_sub_vendor_name(void)
{
	return "RPi5";
}

/* --- Sensor Management protocol (0x15) --- */

#define RPI5_SENSOR_SOC_TEMP	0
#define RPI5_SENSOR_COUNT	1

size_t plat_scmi_sensor_count(unsigned int channel_id __unused)
{
	return RPI5_SENSOR_COUNT;
}

const char *plat_scmi_sensor_name(unsigned int channel_id __unused,
				  unsigned int scmi_id)
{
	if (scmi_id == RPI5_SENSOR_SOC_TEMP)
		return "soc";

	return NULL;
}

int32_t plat_scmi_sensor_attributes(unsigned int channel_id __unused,
				    unsigned int scmi_id,
				    uint8_t *type, int8_t *scale)
{
	if (scmi_id != RPI5_SENSOR_SOC_TEMP)
		return SCMI_NOT_FOUND;

	/* Value reported in milli-Celsius: type degrees C, scale 10^-3. */
	*type = SCMI_SENSOR_TYPE_DEGREES_C;
	*scale = -3;

	return SCMI_SUCCESS;
}

int32_t plat_scmi_sensor_reading_get(unsigned int channel_id __unused,
				     unsigned int scmi_id, uint64_t *value)
{
	int32_t mc = 0;

	if (scmi_id != RPI5_SENSOR_SOC_TEMP)
		return SCMI_NOT_FOUND;

	if (!soc_temp_read_mc(&mc))
		return SCMI_HARDWARE_ERROR;

	/* Sign-extend the milli-Celsius reading into the 64-bit value. */
	*value = (uint64_t)(int64_t)mc;

	return SCMI_SUCCESS;
}

/* --- Performance Domain protocol (0x13): the Active Cooler fan --- */

#ifdef CFG_SCMI_MSG_PERF_DOMAIN

#define RPI5_PERF_FAN		0
#define RPI5_PERF_COUNT		1

size_t plat_scmi_perf_count(unsigned int channel_id __unused)
{
	return RPI5_PERF_COUNT;
}

const char *plat_scmi_perf_domain_name(unsigned int channel_id __unused,
				       unsigned int domain_id)
{
	if (domain_id == RPI5_PERF_FAN)
		return "fan";

	return NULL;
}

int32_t plat_scmi_perf_sustained_freq(unsigned int channel_id __unused,
				      unsigned int domain_id,
				      unsigned int *freq)
{
	if (domain_id != RPI5_PERF_FAN)
		return SCMI_NOT_FOUND;

	/* Not a frequency-scaled domain; 0 is a valid "unspecified". */
	*freq = 0;

	return SCMI_SUCCESS;
}

int32_t plat_scmi_perf_levels_array(unsigned int channel_id __unused,
				    unsigned int domain_id, size_t start_index,
				    unsigned int *elt, size_t *nb_elts)
{
	size_t i = 0;

	if (domain_id != RPI5_PERF_FAN)
		return SCMI_NOT_FOUND;

	/* Query form: report the total level count. */
	if (!elt) {
		*nb_elts = RP1_FAN_LEVEL_COUNT;
		return SCMI_SUCCESS;
	}

	if (start_index >= RP1_FAN_LEVEL_COUNT)
		return SCMI_INVALID_PARAMETERS;

	/* Performance level value == fan level index (0 off .. max). */
	for (i = 0; i < *nb_elts && start_index + i < RP1_FAN_LEVEL_COUNT; i++)
		elt[i] = start_index + i;
	*nb_elts = i;

	return SCMI_SUCCESS;
}

int32_t plat_scmi_perf_level_get(unsigned int channel_id __unused,
				 unsigned int domain_id, unsigned int *level)
{
	if (domain_id != RPI5_PERF_FAN)
		return SCMI_NOT_FOUND;

	*level = rp1_fan_get_level();

	return SCMI_SUCCESS;
}

int32_t plat_scmi_perf_level_set(unsigned int channel_id __unused,
				 unsigned int domain_id, unsigned int level)
{
	if (domain_id != RPI5_PERF_FAN)
		return SCMI_NOT_FOUND;

	if (rp1_fan_set_level(level))
		return SCMI_INVALID_PARAMETERS;

	return SCMI_SUCCESS;
}

#endif /* CFG_SCMI_MSG_PERF_DOMAIN */
