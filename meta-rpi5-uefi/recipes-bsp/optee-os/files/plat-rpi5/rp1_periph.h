/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Shared RP1 peripheral-window mapping.
 *
 * The RP1 southbridge is a PCIe endpoint: its register window has no
 * fixed address until the bootloader enumerates PCIe and assigns BAR1.
 * EDK2 passes that base to OP-TEE once (the sensor pTA's CMD_INIT). Both
 * secure consumers -- the I2C sensor push (bmc_sensor_pta / rp1_i2c) and
 * the SCMI fan controller (scmi_server / rp1_pwm) -- derive their block
 * addresses from this single 4 MiB window, so it is mapped exactly once.
 *
 * Offsets within the window are the RP1_*_BASE values from the EDK2
 * Rp1.h (I2C1 0x74000, CLOCKS 0x18000, PWM1 0x9c000).
 */

#ifndef RP1_PERIPH_H
#define RP1_PERIPH_H

#include <tee_api_types.h>
#include <types_ext.h>

#define RP1_PERIPH_WINDOW_SIZE	0x400000

#define RP1_I2C1_OFFSET		0x74000
#define RP1_CLOCKS_OFFSET	0x18000
#define RP1_PWM1_OFFSET		0x9c000
#define RP1_IO_BANK0_OFFSET	0xd0000
#define RP1_PADS_BANK0_OFFSET	0xf0000

/*
 * Map the RP1 peripheral window at physical @bar_pa as secure device
 * memory. Idempotent: a repeat call with the same address is a no-op;
 * a different address (the OS moved the BAR) remaps. Not called from
 * interrupt context.
 */
TEE_Result rp1_periph_map(paddr_t bar_pa);

/* Virtual base of the mapped window, or 0 if rp1_periph_map() has not run. */
vaddr_t rp1_periph_base(void);

#endif /* RP1_PERIPH_H */
