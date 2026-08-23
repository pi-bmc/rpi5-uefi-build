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

#include "pwr_button.h"
#include "vpu_mbox.h"
#include "rp1_periph.h"
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
#ifdef CFG_SCMI_MSG_SYSTEM_POWER
	SCMI_PROTOCOL_ID_SYS_POWER,
#endif
#ifdef CFG_SCMI_MSG_CLOCK
	SCMI_PROTOCOL_ID_CLOCK,
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

/* --- System Power Management protocol (0x12) --- */

#ifdef CFG_SCMI_MSG_SYSTEM_POWER

/*
 * Vendor system states for SYSTEM_POWER_STATE_GET (0x80000000+ is the
 * spec's vendor range). RUNNING is the quiescent answer;
 * BUTTON_SHUTDOWN reports a latched power-button press, which
 * PowerButtonScmiDxe polls for during the firmware phase and acts on
 * through gRT->ResetSystem (honoring the blconfig POWER_OFF_ON_HALT
 * policy and the reset-notification flush). At OS runtime nothing polls
 * this; the same press reaches the BMC through the sensor record's
 * POWER_BUTTON status bit, and the BMC orchestrates a graceful shutdown.
 */
#define RPI5_SYS_POWER_STATE_RUNNING		0x80000000
#define RPI5_SYS_POWER_STATE_BUTTON_SHUTDOWN	0x80000001

/*
 * Vendor SYSTEM_POWER_STATE_SET states carrying the power-button policy
 * (PowerButtonScmiDxe delivers the blconfig POWER_OFF_ON_HALT verdict at
 * boot). Keep in step with EDK2's RpiScmiLib.h.
 */
#define RPI5_SYS_POWER_SET_POLICY_OFF	0x80000002
#define RPI5_SYS_POWER_SET_POLICY_RESET	0x80000003

/* Architectural states from the System Power module (0x12) we act on. */
#define SCMI_SYS_POWER_STATE_SHUTDOWN	0
#define SCMI_SYS_POWER_STATE_COLD_RESET	1
#define SCMI_SYS_POWER_STATE_WARM_RESET	2

int32_t plat_scmi_sys_power_state_set(unsigned int channel_id __unused,
				      uint32_t flags __unused,
				      uint32_t system_state)
{
	/*
	 * Power actions run THROUGH EL3 (TF-A SiP secure-caller wrappers
	 * over psci_system_off/reset -- plain PSCI is rejected from the
	 * secure world). Agents wanting a flush-first orderly path still
	 * use gRT->ResetSystem/PSCI themselves; this is for delegation
	 * and for the button policy delivery.
	 */
	switch (system_state) {
	case SCMI_SYS_POWER_STATE_SHUTDOWN:
		rpi5_power_act(true);
		return SCMI_HARDWARE_ERROR;	/* only reached on failure */
	case SCMI_SYS_POWER_STATE_COLD_RESET:
	case SCMI_SYS_POWER_STATE_WARM_RESET:
		rpi5_power_act(false);
		return SCMI_HARDWARE_ERROR;
	case RPI5_SYS_POWER_SET_POLICY_OFF:
		rpi5_pwr_button_set_policy(true);
		return SCMI_SUCCESS;
	case RPI5_SYS_POWER_SET_POLICY_RESET:
		rpi5_pwr_button_set_policy(false);
		return SCMI_SUCCESS;
	default:
		return SCMI_NOT_SUPPORTED;
	}
}

int32_t plat_scmi_sys_power_state_get(unsigned int channel_id __unused,
				      uint32_t *system_state)
{
	if (rpi5_pwr_button_pending())
		*system_state = RPI5_SYS_POWER_STATE_BUTTON_SHUTDOWN;
	else
		*system_state = RPI5_SYS_POWER_STATE_RUNNING;

	return SCMI_SUCCESS;
}

#endif /* CFG_SCMI_MSG_SYSTEM_POWER */

/* --- Clock Management protocol (0x14): read-only RP1 observability --- */

#ifdef CFG_SCMI_MSG_CLOCK

/*
 * Two RP1 clocks, observability only -- mutation is denied because both
 * have hard owners (clk_pwm1: the fan controller above; clk_sys: the RP1
 * fabric and the I2C timing constants in rp1_i2c.c). Rates are the fixed
 * values this platform programs: clk_pwm1 runs xosc/1 = 50 MHz when
 * enabled, clk_sys is the RP1's fixed 200 MHz system clock. Both report
 * 0 / disabled until the RP1 BAR handshake maps the window.
 */
#define RPI5_CLK_PWM1		0
#define RPI5_CLK_SYS		1
#define RPI5_CLK_FW_ARM		2
#define RPI5_CLK_FW_CORE	3
#define RPI5_CLK_FW_V3D		4
#define RPI5_CLK_FW_EMMC2	5
#define RPI5_CLK_COUNT		6

/*
 * Ids 2+ are VPU firmware clocks, served over the secure mailbox
 * (vpu_mbox.c). They read 0 / disabled until EDK2 hands the mailbox
 * over at ExitBootServices -- exactly the phase in which the OS, whose
 * device tree has no native firmware-clock driver any more, starts
 * asking for them over SCMI.
 */
static const uint32_t rpi5_fw_clock_id[RPI5_CLK_COUNT] = {
	[RPI5_CLK_FW_ARM] = VPU_CLOCK_ARM,
	[RPI5_CLK_FW_CORE] = VPU_CLOCK_CORE,
	[RPI5_CLK_FW_V3D] = VPU_CLOCK_V3D,
	[RPI5_CLK_FW_EMMC2] = VPU_CLOCK_EMMC2,
};

#define RPI5_CLK_PWM1_HZ	50000000UL
#define RPI5_CLK_SYS_HZ		200000000UL

size_t plat_scmi_clock_count(unsigned int channel_id __unused)
{
	return RPI5_CLK_COUNT;
}

const char *plat_scmi_clock_get_name(unsigned int channel_id __unused,
				     unsigned int scmi_id)
{
	switch (scmi_id) {
	case RPI5_CLK_PWM1:
		return "clk_pwm1";
	case RPI5_CLK_SYS:
		return "clk_sys";
	case RPI5_CLK_FW_ARM:
		return "fw-clk-arm";
	case RPI5_CLK_FW_CORE:
		return "fw-clk-core";
	case RPI5_CLK_FW_V3D:
		return "fw-clk-v3d";
	case RPI5_CLK_FW_EMMC2:
		return "fw-clk-emmc2";
	default:
		return NULL;
	}
}

unsigned long plat_scmi_clock_get_rate(unsigned int channel_id __unused,
				       unsigned int scmi_id)
{
	switch (scmi_id) {
	case RPI5_CLK_PWM1:
		return rp1_pwm1_clk_enabled() ? RPI5_CLK_PWM1_HZ : 0;
	case RPI5_CLK_SYS:
		return rp1_periph_base() ? RPI5_CLK_SYS_HZ : 0;
	case RPI5_CLK_FW_ARM:
	case RPI5_CLK_FW_CORE:
	case RPI5_CLK_FW_V3D:
	case RPI5_CLK_FW_EMMC2:
		return vpu_clock_get_rate(rpi5_fw_clock_id[scmi_id]);
	default:
		return 0;
	}
}

int32_t plat_scmi_clock_rates_array(unsigned int channel_id,
				    unsigned int scmi_id, size_t start_index,
				    unsigned long *rates, size_t *nb_elts)
{
	if (scmi_id >= RPI5_CLK_COUNT)
		return SCMI_NOT_FOUND;

	if (!rates) {
		*nb_elts = 1;
		return SCMI_SUCCESS;
	}

	if (start_index || !*nb_elts)
		return SCMI_INVALID_PARAMETERS;

	rates[0] = plat_scmi_clock_get_rate(channel_id, scmi_id);
	*nb_elts = 1;

	return SCMI_SUCCESS;
}

int32_t plat_scmi_clock_get_state(unsigned int channel_id __unused,
				  unsigned int scmi_id)
{
	switch (scmi_id) {
	case RPI5_CLK_PWM1:
		return rp1_pwm1_clk_enabled() ? 1 : 0;
	case RPI5_CLK_SYS:
		return rp1_periph_base() ? 1 : 0;
	case RPI5_CLK_FW_ARM:
	case RPI5_CLK_FW_CORE:
	case RPI5_CLK_FW_V3D:
	case RPI5_CLK_FW_EMMC2:
		return vpu_mbox_owned() ? 1 : 0;
	default:
		return SCMI_NOT_FOUND;
	}
}

int32_t plat_scmi_clock_set_rate(unsigned int channel_id __unused,
				 unsigned int scmi_id,
				 unsigned long rate __unused)
{
	if (scmi_id >= RPI5_CLK_COUNT)
		return SCMI_NOT_FOUND;

	return SCMI_DENIED;
}

int32_t plat_scmi_clock_set_state(unsigned int channel_id __unused,
				  unsigned int scmi_id,
				  bool enable_not_disable __unused)
{
	if (scmi_id >= RPI5_CLK_COUNT)
		return SCMI_NOT_FOUND;

	return SCMI_DENIED;
}

#endif /* CFG_SCMI_MSG_CLOCK */
