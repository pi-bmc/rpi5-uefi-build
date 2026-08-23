/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Raspberry Pi 5 power-button monitor. The button goes through the PMIC,
 * whose press the VPU firmware forwards by driving SoC GIO pin 20 low
 * (brcmstb GIO block at 0x107d508500, falling edge latched in STAT).
 * OP-TEE owns that edge latch exclusively -- polling and clearing it from
 * two worlds loses presses -- and surfaces presses two ways: the SCMI
 * System Power protocol's STATE_GET (polled by EDK2's PowerButtonScmiDxe
 * during firmware) and the BMC sensor record's POWER_BUTTON status bit
 * (pushed immediately, so the BMC can orchestrate a graceful OS shutdown
 * at runtime, when nothing polls SCMI).
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
 * default). Delivered by the normal world over SCMI (PowerButtonScmiDxe
 * reads POWER_OFF_ON_HALT from the blconfig EEPROM region and sends the
 * matching vendor SYSTEM_POWER_STATE_SET at boot).
 */
void rpi5_pwr_button_set_policy(bool power_off);

/*
 * Execute a system power action THROUGH EL3: TF-A's SiP service exposes
 * secure-caller-only wrappers over psci_system_off()/psci_system_reset()
 * (plain PSCI is rejected from the secure world). Does not return on
 * success. Used by the button's grace-window fallback and by SCMI
 * SYSTEM_POWER_STATE_SET.
 */
void rpi5_power_act(bool power_off);

#endif /* PWR_BUTTON_H */
