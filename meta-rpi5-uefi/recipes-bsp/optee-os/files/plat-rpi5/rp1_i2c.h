/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Minimal polled-mode master driver for the RP1 southbridge's DesignWare
 * DW_apb_i2c controllers (the Raspberry Pi 5's header I2C buses).
 *
 * The controller lives behind the RP1's PCIe BAR1, so the register base
 * is only known once the bootloader has enumerated PCIe and told us the
 * BAR address (see bmc_sensor_pta.c). All transfers are polled and are
 * safe to run either in a yielding thread or, with interrupts already
 * masked, from an interrupt handler; the caller provides the locking.
 */

#ifndef RP1_I2C_H
#define RP1_I2C_H

#include <tee_api_types.h>
#include <types_ext.h>

struct rp1_i2c {
	vaddr_t base;		/* mapped DW_apb_i2c register block */
	bool configured;
};

/*
 * Configure the controller for 100 kHz standard mode as a master and
 * leave it disabled. @base is the virtual address of the 4 KiB register
 * block. Fails if the DW component ID does not answer (link down, BAR
 * moved, wrong offset).
 */
TEE_Result rp1_i2c_init(struct rp1_i2c *i2c, vaddr_t base);

/*
 * Master write of @len bytes to 7-bit @slave_addr: START, address, data,
 * STOP. Returns TEE_ERROR_COMMUNICATION on NACK/abort and
 * TEE_ERROR_BUSY on timeout.
 */
TEE_Result rp1_i2c_write(struct rp1_i2c *i2c, uint8_t slave_addr,
			 const uint8_t *buf, size_t len);

#endif /* RP1_I2C_H */
