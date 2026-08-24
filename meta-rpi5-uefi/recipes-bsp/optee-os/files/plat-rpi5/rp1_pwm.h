/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * RP1 PWM1 channel-3 fan controller (the Raspberry Pi 5 Active Cooler).
 *
 * Register sequence and values are the hardware-proven ones from this
 * project's EDK2 ActiveCoolerDxe: PWM1 at RP1 +0x9c000, channel 3 drives
 * GPIO45; clk_pwm1 (CLOCKS block +0x18000) is xosc 50 MHz / 1 = 20 ns per
 * tick, range 2078 ticks (~24 kHz, the DTB's 41566 ns period); polarity
 * inverted so duty 0 parks the line high (fan off). GPIO45's mux to pwm1
 * is left to firmware (ActiveCoolerDxe sets it and it persists) -- OP-TEE
 * only touches the clock, PWM and (indirectly) the fan speed.
 */

#ifndef RP1_PWM_H
#define RP1_PWM_H

#include <tee_api_types.h>

/* Number of discrete fan levels exposed as SCMI performance levels. */
#define RP1_FAN_LEVEL_COUNT	5

/*
 * Program clk_pwm1, channel 3 range/polarity and enable the channel.
 * Idempotent; relies on rp1_periph_map() having mapped the window.
 * Returns TEE_ERROR_BAD_STATE if the window is not mapped yet.
 */
TEE_Result rp1_fan_init(void);

/* Set the fan to discrete level 0..RP1_FAN_LEVEL_COUNT-1 (0 = off). */
TEE_Result rp1_fan_set_level(unsigned int level);

/* Current level, or 0 if never set / not initialized. */
unsigned int rp1_fan_get_level(void);

/* The 0..255 duty a level maps to (for SCMI descriptors). */
unsigned int rp1_fan_level_duty255(unsigned int level);

/*
 * True when clk_pwm1 is running (either brought up by the fan init above
 * or left running by the VPU). For the SCMI clock protocol's read-only
 * view; returns false while the RP1 window is unmapped.
 */
bool rp1_pwm1_clk_enabled(void);

/*
 * Autonomous thermal governor (OP-TEE-owned). The closed loop that drives
 * the fan from the SoC temperature -- the policy that used to run only in
 * the firmware phase (ActiveCoolerScmiDxe, over SCMI) -- transcribed to run
 * natively in the secure world so the fan is regulated under the OS too,
 * where nothing else commands it (the SCMI perf "fan" domain has no Linux
 * consumer). Profile indices match RPI_POWER_PROFILE_* on the EDK2 side.
 */
#define RP1_FAN_PROFILE_BALANCED  0
#define RP1_FAN_PROFILE_QUIET     1
#define RP1_FAN_PROFILE_COOL      2
#define RP1_FAN_PROFILE_COUNT     3

/*
 * Start the autonomous thermal loop (idempotent). Called at ExitBootServices
 * (the mailbox handoff); before that the firmware agent owns the fan over
 * SCMI perf and this stays idle, so exactly one loop ever runs.
 */
void rp1_fan_enable_auto(void);

/* Select the trip-table profile the autonomous loop follows. */
void rp1_fan_set_profile(unsigned int profile);

/*
 * Re-assert clk_pwm1 if something (Linux's clk_disable_unused) gated it once
 * rp1-clk claimed it. The autonomous loop calls this each tick so the fan
 * the secure world commands cannot be silently switched off from non-secure.
 */
void rp1_fan_kick_clock(void);

#endif /* RP1_PWM_H */
