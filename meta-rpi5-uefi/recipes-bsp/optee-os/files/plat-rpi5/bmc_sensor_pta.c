// SPDX-License-Identifier: BSD-2-Clause
/*
 * BMC sensor push service.
 *
 * A secure-timer (CNTPS) callout samples the BCM2712 die temperature
 * every sample period and caches a bmc_sensor_record. The record is
 * pushed to the BMC over RP1 I2C1 (the pi-bmc EEPROM wire contract:
 * BMC-emulated 24c256 at 0x50, record at EEPROM offset 0x7800):
 *
 *  - before the normal world has enabled async notifications (UEFI
 *    phase, early boot), the push happens directly in the callout
 *    callback (polled I2C, interrupts already masked);
 *  - once Linux's OP-TEE driver is up, the callout instead raises an
 *    async notification (DO_BOTTOM_HALF over SPI 200) and the push
 *    runs in the yielding bottom half, which also wakes any
 *    PTA_BMC_SENSOR_CMD_WAIT callers.
 *
 * The RP1 lives behind PCIe, so the I2C controller has no address until
 * the bootloader enumerates the bus: nothing touches I2C until
 * PTA_BMC_SENSOR_CMD_INIT delivers the BAR (the EDK2->OP-TEE late
 * initialization handshake; see RpiOpteeSensorDxe).
 */

#include <arm.h>
#include <initcall.h>
#include <io.h>
#include <kernel/callout.h>
#include <kernel/mutex.h>
#include <kernel/notif.h>
#include <kernel/pseudo_ta.h>
#include <kernel/spinlock.h>
#include <stddef.h>
#include <string.h>
#include <trace.h>

#include "pta_bmc_sensor.h"
#include "rp1_i2c.h"
#include "rp1_periph.h"
#include "soc_temp.h"

#define PTA_NAME "bmc_sensor.pta"

#define DEFAULT_PERIOD_MS	1000
#define MIN_PERIOD_MS		100
#define MAX_PERIOD_MS		60000
#define EEPROM_PAGE_SIZE	64

struct bmc_sensor_state {
	/* Guards hardware access and the record cache, all contexts */
	unsigned int lock;

	/* I2C transport, valid once PTA_BMC_SENSOR_CMD_INIT succeeded */
	struct rp1_i2c i2c;
	bool i2c_ready;
	paddr_t bar_pa;
	uint32_t i2c_offset;
	uint8_t slave_addr;
	uint16_t eeprom_offset;

	uint32_t period_ms;
	bool callout_added;
	struct callout callout;

	/* Latest sample */
	struct bmc_sensor_record rec;
	uint32_t push_errors;
	bool push_pending;

	/* Async notification plumbing */
	bool notif_started;

	/* CMD_WAIT support, yielding contexts only */
	struct mutex mu;
	struct condvar cv;
};

static struct bmc_sensor_state state = {
	.lock = SPINLOCK_UNLOCK,
	.period_ms = DEFAULT_PERIOD_MS,
	.mu = MUTEX_INITIALIZER,
	.cv = CONDVAR_INITIALIZER,
};

static uint32_t crc32_ieee(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint32_t crc = 0xffffffff;
	size_t n = 0;
	int b = 0;

	for (n = 0; n < len; n++) {
		crc ^= p[n];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320 & (-(crc & 1)));
	}

	return ~crc;
}

static uint32_t uptime_s(void)
{
	return barrier_read_counter_timer() / read_cntfrq();
}

/* Caller holds state.lock (or is single-context by construction) */
static void sample_locked(void)
{
	struct bmc_sensor_record *rec = &state.rec;
	int32_t mc = 0;
	bool valid = soc_temp_read_mc(&mc);

	rec->magic = BMC_SENSOR_RECORD_MAGIC;
	rec->version = BMC_SENSOR_RECORD_VERSION;
	rec->length = sizeof(*rec);
	rec->seq++;
	if (valid)
		rec->soc_temp_mc = mc;
	rec->uptime_s = uptime_s();
	rec->status = (valid ? PTA_BMC_SENSOR_STATUS_TEMP_VALID : 0) |
		      (state.i2c_ready ? PTA_BMC_SENSOR_STATUS_I2C_READY : 0) |
		      (rec->status & PTA_BMC_SENSOR_STATUS_LAST_PUSH_OK);
	rec->reserved = 0;
	rec->crc32 = crc32_ieee(rec, offsetof(struct bmc_sensor_record, crc32));
}

/* Caller holds state.lock */
static void push_record_locked(void)
{
	uint8_t buf[2 + sizeof(struct bmc_sensor_record)] = { };
	TEE_Result res = TEE_SUCCESS;

	if (!state.i2c_ready)
		return;

	/* 24c256-style 2-byte big-endian data address, then the record */
	buf[0] = state.eeprom_offset >> 8;
	buf[1] = state.eeprom_offset;
	memcpy(&buf[2], &state.rec, sizeof(state.rec));

	res = rp1_i2c_write(&state.i2c, state.slave_addr, buf, sizeof(buf));
	if (res) {
		state.push_errors++;
		state.rec.status &= ~PTA_BMC_SENSOR_STATUS_LAST_PUSH_OK;
	} else {
		state.rec.status |= PTA_BMC_SENSOR_STATUS_LAST_PUSH_OK;
	}
	/* status changed after the fact; recompute the wire CRC next push */
}

static bool sensor_callout_cb(struct callout *co __unused)
{
	uint32_t exceptions = cpu_spin_lock_xsave(&state.lock);
	bool defer = state.notif_started;

	sample_locked();
	if (defer)
		state.push_pending = true;
	else
		push_record_locked();

	cpu_spin_unlock_xrestore(&state.lock, exceptions);

	/*
	 * Hand the push (and CMD_WAIT wake-up) to the yielding bottom
	 * half once the normal world services notifications.
	 */
	if (defer)
		notif_send_async(NOTIF_VALUE_DO_BOTTOM_HALF, 0);

	/* Pick up a period change from CMD_INIT (plain u32 read is fine) */
	callout_set_next_timeout(co, state.period_ms);

	return true;
}

static void sensor_notif_atomic_cb(struct notif_driver *ndrv __unused,
				   enum notif_event ev,
				   uint16_t guest_id __unused)
{
	uint32_t exceptions = 0;

	switch (ev) {
	case NOTIF_EVENT_STARTED:
		exceptions = cpu_spin_lock_xsave(&state.lock);
		state.notif_started = true;
		cpu_spin_unlock_xrestore(&state.lock, exceptions);
		DMSG(PTA_NAME ": normal world notifications up");
		break;
	default:
		break;
	}
}
DECLARE_KEEP_PAGER(sensor_notif_atomic_cb);

static void sensor_notif_yielding_cb(struct notif_driver *ndrv __unused,
				     enum notif_event ev)
{
	uint32_t exceptions = 0;
	bool do_push = false;

	switch (ev) {
	case NOTIF_EVENT_DO_BOTTOM_HALF:
		exceptions = cpu_spin_lock_xsave(&state.lock);
		do_push = state.push_pending;
		state.push_pending = false;
		if (do_push)
			push_record_locked();
		cpu_spin_unlock_xrestore(&state.lock, exceptions);

		if (do_push) {
			mutex_lock(&state.mu);
			condvar_broadcast(&state.cv);
			mutex_unlock(&state.mu);
		}
		break;
	case NOTIF_EVENT_STOPPED:
	case NOTIF_EVENT_SHUTDOWN:
		exceptions = cpu_spin_lock_xsave(&state.lock);
		state.notif_started = false;
		cpu_spin_unlock_xrestore(&state.lock, exceptions);
		break;
	default:
		break;
	}
}

static struct notif_driver sensor_notif_driver __nex_data = {
	.atomic_cb = sensor_notif_atomic_cb,
	.yielding_cb = sensor_notif_yielding_cb,
};

static TEE_Result cmd_init(uint32_t types, TEE_Param params[TEE_NUM_PARAMS])
{
	TEE_Result res = TEE_SUCCESS;
	struct rp1_i2c i2c = { };
	uint32_t exceptions = 0;
	paddr_t bar_pa = 0;
	uint32_t i2c_offset = 0;
	uint32_t slave = 0;
	uint32_t eeprom_offset = 0;
	uint32_t period = 0;

	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
				     TEE_PARAM_TYPE_VALUE_INPUT,
				     TEE_PARAM_TYPE_VALUE_INPUT,
				     TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	bar_pa = ((paddr_t)params[0].value.b << 32) | params[0].value.a;
	i2c_offset = params[1].value.a;
	slave = params[1].value.b;
	eeprom_offset = params[2].value.a;
	period = params[2].value.b;

	if (i2c_offset >= RP1_PERIPH_WINDOW_SIZE ||
	    !slave || slave > 0x7f || eeprom_offset > UINT16_MAX)
		return TEE_ERROR_BAD_PARAMETERS;

	/* The record must not cross a 64-byte EEPROM page */
	if (eeprom_offset % EEPROM_PAGE_SIZE ||
	    sizeof(struct bmc_sensor_record) > EEPROM_PAGE_SIZE)
		return TEE_ERROR_BAD_PARAMETERS;

	if (period && (period < MIN_PERIOD_MS || period > MAX_PERIOD_MS))
		return TEE_ERROR_BAD_PARAMETERS;

	/* Map the RP1 window (shared with the SCMI fan controller). */
	res = rp1_periph_map(bar_pa);
	if (res)
		return res;

	res = rp1_i2c_init(&i2c, rp1_periph_base() + i2c_offset);
	if (res)
		return res;

	exceptions = cpu_spin_lock_xsave(&state.lock);
	state.i2c = i2c;
	state.bar_pa = bar_pa;
	state.i2c_offset = i2c_offset;
	state.slave_addr = slave;
	state.eeprom_offset = eeprom_offset;
	if (period)
		state.period_ms = period;
	state.i2c_ready = true;
	cpu_spin_unlock_xrestore(&state.lock, exceptions);

	IMSG(PTA_NAME ": RP1 I2C at %#"PRIxPA"+%#"PRIx32
	     ", slave %#"PRIx32", eeprom offset %#"PRIx32", period %u ms",
	     bar_pa, i2c_offset, slave, eeprom_offset, state.period_ms);

	return TEE_SUCCESS;
}

static void fill_sample_out(TEE_Param *temp_seq, TEE_Param *status_err,
			    TEE_Param *when)
{
	uint32_t exceptions = cpu_spin_lock_xsave(&state.lock);

	temp_seq->value.a = (uint32_t)state.rec.soc_temp_mc;
	temp_seq->value.b = state.rec.seq;
	status_err->value.a = state.rec.status;
	status_err->value.b = state.push_errors;
	if (when)
		when->value.a = state.rec.uptime_s;

	cpu_spin_unlock_xrestore(&state.lock, exceptions);
}

static TEE_Result cmd_get(uint32_t types, TEE_Param params[TEE_NUM_PARAMS])
{
	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	fill_sample_out(&params[0], &params[1], &params[2]);

	return TEE_SUCCESS;
}

static uint32_t current_seq(void)
{
	uint32_t exceptions = cpu_spin_lock_xsave(&state.lock);
	uint32_t seq = state.rec.seq;

	cpu_spin_unlock_xrestore(&state.lock, exceptions);

	return seq;
}

static TEE_Result cmd_wait(uint32_t types, TEE_Param params[TEE_NUM_PARAMS])
{
	TEE_Result res = TEE_SUCCESS;
	uint32_t last_seq = 0;
	uint32_t timeout_ms = 0;

	if (types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
				     TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	last_seq = params[0].value.a;
	timeout_ms = params[0].value.b;

	mutex_lock(&state.mu);
	while (current_seq() == last_seq) {
		if (timeout_ms) {
			res = condvar_wait_timeout(&state.cv, &state.mu,
						   timeout_ms);
			if (res)
				break;
		} else {
			condvar_wait(&state.cv, &state.mu);
		}
	}
	mutex_unlock(&state.mu);

	if (res)
		return res;

	fill_sample_out(&params[1], &params[2], NULL);

	return TEE_SUCCESS;
}

static TEE_Result invoke_command(void *sess_ctx __unused, uint32_t cmd,
				 uint32_t types,
				 TEE_Param params[TEE_NUM_PARAMS])
{
	switch (cmd) {
	case PTA_BMC_SENSOR_CMD_INIT:
		return cmd_init(types, params);
	case PTA_BMC_SENSOR_CMD_GET:
		return cmd_get(types, params);
	case PTA_BMC_SENSOR_CMD_WAIT:
		return cmd_wait(types, params);
	default:
		return TEE_ERROR_NOT_IMPLEMENTED;
	}
}

pseudo_ta_register(.uuid = PTA_BMC_SENSOR_UUID, .name = PTA_NAME,
		   .flags = PTA_DEFAULT_FLAGS | TA_FLAG_CONCURRENT,
		   .invoke_command_entry_point = invoke_command);

static TEE_Result bmc_sensor_init(void)
{
	notif_register_driver(&sensor_notif_driver);

	/*
	 * Sampling starts right away (the sensor is SoC-internal and
	 * always mappable); the I2C push activates on CMD_INIT.
	 */
	callout_add(&state.callout, sensor_callout_cb, state.period_ms);
	state.callout_added = true;

	IMSG(PTA_NAME ": sampling every %u ms", state.period_ms);

	return TEE_SUCCESS;
}

driver_init(bmc_sensor_init);
