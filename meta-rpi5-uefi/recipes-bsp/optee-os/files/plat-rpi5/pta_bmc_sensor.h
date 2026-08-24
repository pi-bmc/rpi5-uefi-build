/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * BMC sensor push service - pseudo-TA ABI.
 *
 * This header is the wire contract between the OP-TEE pTA
 * (bmc_sensor_pta.c), the EDK2 handshake driver (RpiOpteeSensorDxe,
 * which carries its own mirror of these constants) and the normal-world
 * daemon (docs/optee-sensor/). Keep the three in step.
 *
 * The service samples the BCM2712 die temperature on a secure timer and
 * pushes a BMC_SENSOR_RECORD to the BMC-emulated I2C EEPROM (24c256 at
 * 0x50 on RP1 I2C1, the pi-bmc wire contract) at EEPROM offset 0x7800 -
 * the spare region of the pi-bmc EEPROM map (0x0000 vars / 0x4000 env /
 * 0x6000 SMBIOS / 0x6800 blkinfo / 0x7800 spare).
 */

#ifndef PTA_BMC_SENSOR_H
#define PTA_BMC_SENSOR_H

#include <stdint.h>
#include <util.h>

#define PTA_BMC_SENSOR_UUID \
	{ 0x575d6607, 0x5a2b, 0x4384, \
		{ 0x82, 0x7e, 0xc1, 0x6a, 0x25, 0xac, 0x4f, 0xa5 } }

/*
 * PTA_BMC_SENSOR_CMD_INIT - Late-initialization handshake
 *
 * The RP1 is a PCIe endpoint: its BAR only exists after the bootloader
 * (EDK2) has enumerated PCIe. EDK2 invokes this command once the RP1
 * bus driver is up; the normal-world daemon may invoke it again at
 * boot (with the BAR address Linux sees) in case the OS moved the BAR.
 * Idempotent; each call remaps if the address changed.
 *
 * [in] params[0].value.a  RP1 peripheral BAR physical address, low 32 bits
 * [in] params[0].value.b  RP1 peripheral BAR physical address, high 32 bits
 * [in] params[1].value.a  I2C controller offset within the BAR (0x74000)
 * [in] params[1].value.b  7-bit I2C slave address of the BMC EEPROM (0x50)
 * [in] params[2].value.a  EEPROM byte offset for the record (0x7800)
 * [in] params[2].value.b  sample period in ms (0 = keep current/default)
 */
#define PTA_BMC_SENSOR_CMD_INIT		0

/*
 * PTA_BMC_SENSOR_CMD_GET - Read the latest cached sample
 *
 * [out] params[0].value.a  SoC temperature, milli-Celsius (as INT32)
 * [out] params[0].value.b  sample sequence number
 * [out] params[1].value.a  status flags (PTA_BMC_SENSOR_STATUS_*)
 * [out] params[1].value.b  I2C push error count
 * [out] params[2].value.a  seconds since OP-TEE boot at sample time
 */
#define PTA_BMC_SENSOR_CMD_GET		1

/*
 * PTA_BMC_SENSOR_CMD_WAIT - Block until a sample newer than @a arrives
 *
 * Blocks the calling (normal world) thread inside the TEE until the
 * sample sequence number differs from params[0].value.a, then returns
 * the same outputs as CMD_GET. Wake-up is driven by the async
 * notification bottom half, so this only makes progress once the
 * normal-world OP-TEE driver is up (Linux; not the UEFI phase).
 *
 * [in]  params[0].value.a  last sequence number seen by the caller
 * [in]  params[0].value.b  timeout in ms (0 = wait forever)
 * [out] params[1].value.a  SoC temperature, milli-Celsius (as INT32)
 * [out] params[1].value.b  sample sequence number
 * [out] params[2].value.a  status flags
 * [out] params[2].value.b  I2C push error count
 *
 * Returns TEE_ERROR_TIMEOUT if the timeout expires first.
 */
#define PTA_BMC_SENSOR_CMD_WAIT		2

/*
 * PTA_BMC_SENSOR_CMD_MBOX_HANDOFF - Normal world hands the VPU mailbox over
 *
 * Invoked by EDK2 at ExitBootServices: from here on OP-TEE is the only
 * mailbox user (the OS device tree disables the mailbox and firmware
 * nodes and consumes firmware services over SCMI). One-way; no params.
 */
#define PTA_BMC_SENSOR_CMD_MBOX_HANDOFF	3

/* Status flags */
#define PTA_BMC_SENSOR_STATUS_TEMP_VALID	BIT(0) /* AVS read valid */
#define PTA_BMC_SENSOR_STATUS_I2C_READY		BIT(1) /* INIT done */
#define PTA_BMC_SENSOR_STATUS_LAST_PUSH_OK	BIT(2) /* last I2C write ok */
#define PTA_BMC_SENSOR_STATUS_POWER_BUTTON	BIT(3) /* press latched (sticky) */

/*
 * Core-internal (pwr_button.c -> bmc_sensor_pta.c): latch the power-button
 * status bit into the record and push it to the BMC immediately, so the
 * BMC can act on the press without waiting out the sample period. Safe
 * from callout context.
 */
void bmc_sensor_flag_power_button(void);

/* fan_flags bits */
#define BMC_SENSOR_FAN_VALID		BIT(0)	/* fan block populated */

/*
 * The record written to the BMC EEPROM at the configured offset.
 * Little-endian, 48 bytes, one 64-byte EEPROM page. Every field is
 * naturally aligned, so the struct has no padding and sizeof() == 48.
 *
 * Version 2 appended the fan block (level/duty/rpm) and reserved words so
 * all telemetry the BMC reports rides one I2C write; the version-1 layout
 * ended at the first reserved word (32 bytes). Readers key the trailing
 * CRC off @length, so a v1 writer and a v2 reader interoperate: the reader
 * validates the v1 prefix and leaves the fan fields zero.
 */
#define BMC_SENSOR_RECORD_MAGIC		0x52534E53	/* "SNSR" */
#define BMC_SENSOR_RECORD_VERSION	2

struct bmc_sensor_record {
	uint32_t magic;		/* BMC_SENSOR_RECORD_MAGIC */
	uint16_t version;	/* BMC_SENSOR_RECORD_VERSION */
	uint16_t length;	/* sizeof(struct bmc_sensor_record) */
	uint32_t seq;		/* increments every sample */
	int32_t soc_temp_mc;	/* SoC die temperature, milli-Celsius */
	uint32_t uptime_s;	/* seconds since OP-TEE boot */
	uint32_t status;	/* PTA_BMC_SENSOR_STATUS_* */
	/* --- version 2: fan block --- */
	uint8_t fan_level;	/* commanded cooling level, 0..fan_max_level */
	uint8_t fan_max_level;	/* highest level (RP1_FAN_LEVEL_COUNT - 1) */
	uint8_t fan_duty_pct;	/* PWM duty of the commanded level, 0..100 */
	uint8_t fan_flags;	/* BMC_SENSOR_FAN_* */
	uint16_t fan_rpm;	/* measured tach RPM, 0 = not measured */
	uint16_t reserved0;	/* 0 */
	uint32_t reserved1;	/* 0 (reserved: rail voltage) */
	uint32_t reserved2;	/* 0 (reserved: rail current) */
	uint32_t reserved3;	/* 0 */
	uint32_t crc32;		/* IEEE CRC32 of bytes 0..(length-4) */
};

#endif /* PTA_BMC_SENSOR_H */
