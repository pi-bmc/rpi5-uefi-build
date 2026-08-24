/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Raspberry Pi 5 power-button monitor. The button goes through the PMIC,
 * whose press the VPU firmware forwards by driving SoC GIO pin 20 low
 * (brcmstb GIO block at 0x107d508500, falling edge latched in STAT).
 *
 * OP-TEE owns that edge latch during the FIRMWARE phase only. Polling and
 * clearing STAT from two worlds loses presses, so at ExitBootServices this
 * releases the GIO (rpi5_pwr_button_release) and the OS's own gpio-keys
 * driver -- which owns pin 20 in the device tree -- takes the button over.
 * That matters because the OS must run the shutdown: it quiesces every core
 * before PSCI SYSTEM_OFF, whereas a raw power-off from the secure world
 * while the OS is live cannot, and wedges the calling core in EL3.
 *
 * Presses are also surfaced on the BMC sensor record's POWER_BUTTON status
 * bit (pushed immediately at the latch).
 */

#ifndef PWR_BUTTON_H
#define PWR_BUTTON_H

#include <stdbool.h>

/*
 * True once a press has been latched. Never cleared by software: the
 * consumer acts by resetting or powering off the system (firmware phase),
 * or the BMC acts on the pushed record (runtime). Monotonic, so it is
 * safely readable from any context without the button lock.
 */
bool rpi5_pwr_button_pending(void);

/*
 * Press policy: powers off (true) or resets (false, the U-Boot-compatible
 * default) for the FIRMWARE-phase press action. Delivered by the normal
 * world over SCMI (RpiScmiConfigDxe reads POWER_OFF_ON_HALT from the
 * blconfig EEPROM region and sends the matching vendor SYSTEM_POWER_STATE_SET
 * at boot). At OS runtime the kernel decides the action, not this.
 */
void rpi5_pwr_button_set_policy(bool power_off);

/*
 * Release the button to the OS at ExitBootServices: stop polling and stop
 * clearing the GIO edge latch, so the kernel's gpio-keys irqchip receives
 * presses and runs an orderly shutdown. Idempotent. After this OP-TEE never
 * touches the button GIO again, so it cannot race the kernel or wedge a core
 * with a secure-world power-off while the OS is live.
 */
void rpi5_pwr_button_release(void);

/*
 * Execute a system power action THROUGH EL3: TF-A's SiP service exposes
 * secure-caller-only wrappers over psci_system_off()/psci_system_reset()
 * (plain PSCI is rejected from the secure world). Does not return on
 * success. Used by the button's grace-window fallback and by SCMI
 * SYSTEM_POWER_STATE_SET.
 */
void rpi5_power_act(bool power_off);

#endif /* PWR_BUTTON_H */
