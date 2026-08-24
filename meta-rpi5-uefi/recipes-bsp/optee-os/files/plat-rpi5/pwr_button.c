// SPDX-License-Identifier: BSD-2-Clause
/*
 * Raspberry Pi 5 power-button monitor. See pwr_button.h.
 *
 * Register interface per Linux gpio-brcmstb.c and the retired NS
 * PowerButtonDxe (which parsed the same block out of the VPU DTB:
 * gpio@7d508500, brcm,brcmstb-gpio, bank widths <32 4>, pwr_button on
 * pin 20 active-low). Falling-edge detection latches in STAT until
 * write-1-cleared, so the 100 ms callout below cannot miss even the
 * ~100-200 ms pulses KVM devices produce; a level check backs it up for
 * sustained holds.
 */

#include <initcall.h>
#include <io.h>
#include <kernel/callout.h>
#include <kernel/thread.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <trace.h>

#include "pta_bmc_sensor.h"
#include "pwr_button.h"

/*
 * CPU PA of the brcmstb GIO block carrying the button (SoC bus
 * 0x7d508500 behind the 0x10_00000000 window). Bank 0, bit 20.
 */
#define GIO_BASE	0x107d508500UL
#define GIO_SIZE	0x40

#define GIO_REG_DATA	0x04	/* pin level */
#define GIO_REG_EC	0x0c	/* edge config: 0 = falling */
#define GIO_REG_EI	0x10	/* edge-insensitive: set = both edges */
#define GIO_REG_MASK	0x14	/* must be set for STAT to latch */
#define GIO_REG_STAT	0x1c	/* latched edges, write-1-to-clear */

#define BUTTON_BIT	BIT(20)

#define POLL_PERIOD_MS	100

/*
 * Grace window between latching a firmware-phase press and acting on it:
 * 20 polls = ~2 s. It runs ONLY during the firmware phase, where OP-TEE is
 * effectively the sole running core and a raw EL3 power action is safe. At
 * ExitBootServices the button is released to the OS (rpi5_pwr_button_release),
 * so this fallback never fires at runtime -- a secure-world power-off cannot
 * quiesce the kernel's other cores and would wedge one in EL3. The BMC record
 * was flagged at the latch regardless.
 */
#define ACT_GRACE_POLLS	20

/* SiP FIDs, secure-caller-only; keep in step with TF-A rpi_scmi_svc.c */
#define RPI_SIP_SYSTEM_OFF	0x82000011
#define RPI_SIP_SYSTEM_RESET	0x82000012

register_phys_mem_pgdir(MEM_AREA_IO_SEC, GIO_BASE, GIO_SIZE);

static struct callout pwr_button_callout;
static bool pwr_button_latched;
static bool pwr_button_powers_off;
static bool pwr_button_released;
static unsigned int pwr_button_grace;

bool rpi5_pwr_button_pending(void)
{
	return pwr_button_latched;
}

void rpi5_pwr_button_set_policy(bool power_off)
{
	pwr_button_powers_off = power_off;
	IMSG("pwr_button: press policy = %s", power_off ? "power off" : "reset");
}

void rpi5_pwr_button_release(void)
{
	/*
	 * Read by the poll callout, which then clears the latch one last time
	 * and unregisters itself. A plain store is enough: the callout only
	 * ever transitions this false->true and reacts on its next tick.
	 */
	pwr_button_released = true;
	IMSG("pwr_button: released to the OS (kernel gpio-keys owns it now)");
}

void rpi5_power_act(bool power_off)
{
	struct thread_smc_args args = {
		.a0 = power_off ? RPI_SIP_SYSTEM_OFF : RPI_SIP_SYSTEM_RESET,
	};

	IMSG("pwr_button: executing system %s", power_off ? "off" : "reset");
	thread_smccc(&args);

	/* Only reached if EL3 refused the call */
	EMSG("pwr_button: EL3 power action failed (%#" PRIx64 ")", args.a0);
}

static vaddr_t gio_base(void)
{
	return (vaddr_t)phys_to_virt(GIO_BASE, MEM_AREA_IO_SEC, GIO_SIZE);
}

/* Callout context: interrupts masked, keep it to a few MMIOs. */
static bool pwr_button_callout_cb(struct callout *co __unused)
{
	vaddr_t base = gio_base();
	bool pressed = false;

	if (!base)
		return true;

	/*
	 * Released to the OS at ExitBootServices: clear any latched edge so the
	 * kernel does not see a stale press, then unregister (return false) so
	 * OP-TEE never touches the button GIO again and cannot race the kernel's
	 * gpio-keys irqchip.
	 */
	if (pwr_button_released) {
		io_write32(base + GIO_REG_STAT, BUTTON_BIT);
		return false;
	}

	/* Latched falling edge, or currently held low (active-low). */
	pressed = (io_read32(base + GIO_REG_STAT) & BUTTON_BIT) ||
		  !(io_read32(base + GIO_REG_DATA) & BUTTON_BIT);

	io_write32(base + GIO_REG_STAT, BUTTON_BIT);

	if (pressed && !pwr_button_latched) {
		pwr_button_latched = true;
		pwr_button_grace = ACT_GRACE_POLLS;
		IMSG("pwr_button: press latched");
		bmc_sensor_flag_power_button();
	}

	/*
	 * Grace-window fallback: if the press is still pending after the
	 * window (nothing in the normal world reacted), act ourselves.
	 */
	if (pwr_button_latched && pwr_button_grace && !--pwr_button_grace)
		rpi5_power_act(pwr_button_powers_off);

	return true;
}

static TEE_Result pwr_button_init(void)
{
	vaddr_t base = gio_base();

	if (!base) {
		EMSG("pwr_button: GIO block not mapped");
		return TEE_ERROR_GENERIC;
	}

	/* Falling-edge detection on pin 20, latch enabled, latch cleared. */
	io_clrbits32(base + GIO_REG_EC, BUTTON_BIT);
	io_clrbits32(base + GIO_REG_EI, BUTTON_BIT);
	io_setbits32(base + GIO_REG_MASK, BUTTON_BIT);
	io_write32(base + GIO_REG_STAT, BUTTON_BIT);

	callout_add(&pwr_button_callout, pwr_button_callout_cb, POLL_PERIOD_MS);

	return TEE_SUCCESS;
}

driver_init(pwr_button_init);
