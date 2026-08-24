// SPDX-License-Identifier: BSD-2-Clause
/*
 * RP1 PWM1 channel-3 fan controller. See rp1_pwm.h. Ported from the
 * hardware-proven pi-bmc EDK2 ActiveCoolerDxe register sequence.
 */

#include <io.h>
#include <kernel/callout.h>
#include <kernel/spinlock.h>
#include <trace.h>
#include <util.h>

#include "rp1_periph.h"
#include "rp1_pwm.h"
#include "soc_temp.h"

/* PWM block, per-channel stride 16 bytes. */
#define PWM_GLOBAL_CTRL		0x000
#define PWM_GLOBAL_SET_UPDATE	BIT(31)		/* latch shadowed config */
#define PWM_GLOBAL_CHAN_EN(c)	BIT(c)
#define PWM_CHAN_CTRL(c)	(0x014 + ((c) * 16))
#define PWM_CHAN_RANGE(c)	(0x018 + ((c) * 16))
#define PWM_CHAN_DUTY(c)	(0x020 + ((c) * 16))

#define PWM_CHAN_CTRL_MODE_TE_MS	BIT(0)	/* trailing-edge mark/space */
#define PWM_CHAN_CTRL_INVERT		BIT(3)
#define PWM_CHAN_CTRL_FIFO_POP		BIT(8)

/* clk_pwm1 in the CLOCKS block; aux source index 2 is xosc (50 MHz). */
#define CLK_PWM1_CTRL		0x084
#define CLK_PWM1_DIV_INT	0x088
#define CLK_PWM1_DIV_FRAC	0x08c
#define CLK_CTRL_ENABLE		BIT(11)
#define CLK_CTRL_AUXSRC_XOSC	SHIFT_U32(2, 5)

#define FAN_PWM_CHANNEL		3
#define FAN_PWM_RANGE_TICKS	2078	/* 41566 ns at 20 ns/tick */

/*
 * Fan PWM line: GPIO45, funcsel 0 (= PWM1 channel 3), pull-down -- the
 * values the VPU DTB uses for Linux's pwm-fan. GPIO45 lives in bank 2
 * (pins 34..53, window offset +0x8000 within the IO and PADS blocks),
 * local index 11. OP-TEE owns this mux: with the fan delegated to SCMI,
 * no normal-world agent touches RP1 fan hardware at all.
 */
#define FAN_GPIO_BANK_OFFSET	0x8000
#define FAN_GPIO_LOCAL		11	/* GPIO45 - 34 */

#define IO_CTRL_REG		(RP1_IO_BANK0_OFFSET + FAN_GPIO_BANK_OFFSET + \
				 (FAN_GPIO_LOCAL) * 8 + 4)
#define IO_CTRL_FUNCSEL_MASK	GENMASK_32(4, 0)
#define IO_CTRL_OUTOVER_MASK	GENMASK_32(13, 12)	/* 0 = peripheral */
#define IO_CTRL_OEOVER_MASK	GENMASK_32(15, 14)	/* 0 = peripheral */
#define IO_CTRL_FUNCSEL_PWM	0			/* alt0 on GPIO45 */

#define PADS_PIN_REG		(RP1_PADS_BANK0_OFFSET + FAN_GPIO_BANK_OFFSET + \
				 0x4 + (FAN_GPIO_LOCAL) * 4)
#define PADS_PULL_DOWN		BIT(2)
#define PADS_PULL_UP		BIT(3)
#define PADS_IN_ENABLE		BIT(6)
#define PADS_OUT_DISABLE	BIT(7)

/* Trip/duty table mirrors ActiveCoolerDxe: level 0 off .. level 4 max. */
static const unsigned int fan_duty255[RP1_FAN_LEVEL_COUNT] = {
	0, 75, 125, 175, 250,
};

/*
 * A spinlock, not a mutex: rp1_fan_set_level is reached from the SCMI SMT
 * fastcall entry, which runs in atomic context (interrupts masked) where
 * a mutex is illegal. Every critical section here is a short run of MMIO.
 */
static unsigned int fan_lock = SPINLOCK_UNLOCK;
static bool fan_ready;
static unsigned int fan_level;

static vaddr_t pwm_base(void)
{
	vaddr_t base = rp1_periph_base();

	return base ? base + RP1_PWM1_OFFSET : 0;
}

static vaddr_t clk_base(void)
{
	vaddr_t base = rp1_periph_base();

	return base ? base + RP1_CLOCKS_OFFSET : 0;
}

/* Caller holds fan_lock. Program clk_pwm1 + channel 3, idempotent. */
static bool fan_hw_init_locked(void)
{
	vaddr_t pwm = pwm_base();
	vaddr_t clk = clk_base();

	if (!pwm || !clk)
		return false;
	if (fan_ready)
		return true;

	/* Enable clk_pwm1 off xosc if firmware left it disabled. */
	if (!(io_read32(clk + CLK_PWM1_CTRL) & CLK_CTRL_ENABLE)) {
		io_write32(clk + CLK_PWM1_DIV_INT, 1);
		io_write32(clk + CLK_PWM1_DIV_FRAC, 0);
		io_write32(clk + CLK_PWM1_CTRL,
			   CLK_CTRL_AUXSRC_XOSC | CLK_CTRL_ENABLE);
	}

	/*
	 * Mux GPIO45 to the PWM (idempotent if the VPU already did): pad
	 * input buffer on, output not disabled, pull-down; funcsel to PWM
	 * with the out/oe overrides back on peripheral control.
	 */
	io_clrsetbits32(rp1_periph_base() + PADS_PIN_REG,
			PADS_OUT_DISABLE | PADS_PULL_UP,
			PADS_IN_ENABLE | PADS_PULL_DOWN);
	io_clrsetbits32(rp1_periph_base() + IO_CTRL_REG,
			IO_CTRL_FUNCSEL_MASK | IO_CTRL_OUTOVER_MASK |
			IO_CTRL_OEOVER_MASK,
			IO_CTRL_FUNCSEL_PWM);

	io_write32(pwm + PWM_CHAN_CTRL(FAN_PWM_CHANNEL),
		   PWM_CHAN_CTRL_MODE_TE_MS | PWM_CHAN_CTRL_INVERT |
		   PWM_CHAN_CTRL_FIFO_POP);
	io_write32(pwm + PWM_CHAN_RANGE(FAN_PWM_CHANNEL), FAN_PWM_RANGE_TICKS);
	io_write32(pwm + PWM_CHAN_DUTY(FAN_PWM_CHANNEL), 0);
	io_write32(pwm + PWM_GLOBAL_CTRL,
		   io_read32(pwm + PWM_GLOBAL_CTRL) |
		   PWM_GLOBAL_CHAN_EN(FAN_PWM_CHANNEL) | PWM_GLOBAL_SET_UPDATE);

	fan_ready = true;
	fan_level = 0;

	return true;
}

TEE_Result rp1_fan_init(void)
{
	uint32_t exceptions = cpu_spin_lock_xsave(&fan_lock);
	bool ok = fan_hw_init_locked();

	cpu_spin_unlock_xrestore(&fan_lock, exceptions);

	return ok ? TEE_SUCCESS : TEE_ERROR_BAD_STATE;
}

TEE_Result rp1_fan_set_level(unsigned int level)
{
	vaddr_t pwm = pwm_base();
	uint32_t duty_ticks = 0;
	uint32_t exceptions = 0;

	if (level >= RP1_FAN_LEVEL_COUNT)
		return TEE_ERROR_BAD_PARAMETERS;
	if (!pwm)
		return TEE_ERROR_BAD_STATE;

	exceptions = cpu_spin_lock_xsave(&fan_lock);
	if (!fan_hw_init_locked()) {
		cpu_spin_unlock_xrestore(&fan_lock, exceptions);
		return TEE_ERROR_BAD_STATE;
	}

	duty_ticks = (fan_duty255[level] * FAN_PWM_RANGE_TICKS + 127) / 255;
	io_write32(pwm + PWM_CHAN_DUTY(FAN_PWM_CHANNEL), duty_ticks);
	io_write32(pwm + PWM_GLOBAL_CTRL,
		   io_read32(pwm + PWM_GLOBAL_CTRL) | PWM_GLOBAL_SET_UPDATE);
	fan_level = level;
	cpu_spin_unlock_xrestore(&fan_lock, exceptions);

	FMSG("rp1_fan: level %u -> %u ticks", level, duty_ticks);

	return TEE_SUCCESS;
}

unsigned int rp1_fan_get_level(void)
{
	return fan_level;
}

unsigned int rp1_fan_level_duty255(unsigned int level)
{
	if (level >= RP1_FAN_LEVEL_COUNT)
		return 0;

	return fan_duty255[level];
}

bool rp1_pwm1_clk_enabled(void)
{
	vaddr_t clk = clk_base();

	if (!clk)
		return false;

	return io_read32(clk + CLK_PWM1_CTRL) & CLK_CTRL_ENABLE;
}

/*
 * --- Autonomous thermal governor -------------------------------------
 *
 * The closed loop that regulates the fan from the SoC temperature. It used
 * to run only in the firmware phase (ActiveCoolerScmiDxe, over SCMI); this
 * is the same policy, native to the secure world so the fan is regulated
 * under the OS too -- where no agent commands it (the SCMI perf "fan"
 * domain has no Linux consumer). Native throughout: temperature is a direct
 * AVS read and the fan a direct PWM write, so the loop never round-trips its
 * own SCMI server. Enabled at ExitBootServices (rp1_fan_enable_auto, from
 * the mailbox handoff); before that the firmware agent owns the fan and this
 * stays idle, so exactly one loop ever runs.
 */

/*
 * Control cadence: brisk enough for the SoC's thermal mass, light on the
 * secure timer (a fraction of the firmware agent's 1 s poll), and it bounds
 * how long a Linux-gated clk_pwm1 can stall the fan before we re-assert it.
 */
#define FAN_CTRL_PERIOD_MS	5000
#define FAN_HYST_MC		5000	/* 5 C, matches the firmware agent */
#define FAN_LEVEL_SAFE		2	/* commanded when the sensor is invalid */

/*
 * Trip temperature (milli-C) to ENTER each level, per profile; index 0 is
 * unused (level 0 is off). Values mirror ActiveCoolerScmiDxe's ProfileTrips
 * so firmware- and OS-phase behaviour match. Balanced is the default until
 * the firmware delivers the operator's selected profile over SCMI perf.
 */
static const int32_t fan_trip_mc[RP1_FAN_PROFILE_COUNT][RP1_FAN_LEVEL_COUNT] = {
	[RP1_FAN_PROFILE_BALANCED] = { 0, 50000, 60000, 68000, 75000 },
	[RP1_FAN_PROFILE_QUIET]    = { 0, 58000, 68000, 76000, 84000 },
	[RP1_FAN_PROFILE_COOL]     = { 0, 42000, 50000, 58000, 66000 },
};

static unsigned int fan_profile = RP1_FAN_PROFILE_BALANCED;
static struct callout fan_auto_callout;
static bool fan_auto_added;

void rp1_fan_set_profile(unsigned int profile)
{
	uint32_t exceptions = 0;

	if (profile >= RP1_FAN_PROFILE_COUNT)
		return;

	exceptions = cpu_spin_lock_xsave(&fan_lock);
	fan_profile = profile;
	cpu_spin_unlock_xrestore(&fan_lock, exceptions);

	DMSG("rp1_fan: profile -> %u", profile);
}

void rp1_fan_kick_clock(void)
{
	uint32_t exceptions = cpu_spin_lock_xsave(&fan_lock);
	vaddr_t clk = clk_base();

	if (clk && !(io_read32(clk + CLK_PWM1_CTRL) & CLK_CTRL_ENABLE)) {
		io_write32(clk + CLK_PWM1_DIV_INT, 1);
		io_write32(clk + CLK_PWM1_DIV_FRAC, 0);
		io_write32(clk + CLK_PWM1_CTRL,
			   CLK_CTRL_AUXSRC_XOSC | CLK_CTRL_ENABLE);
	}

	cpu_spin_unlock_xrestore(&fan_lock, exceptions);
}

/*
 * Target level for a temperature under @prof: the highest level whose trip
 * is at or below the temperature, with the firmware agent's gentle ramp-down
 * (release one level per tick, but hold inside a 5 C hysteresis band below
 * the current level's trip so the fan does not hunt).
 */
static unsigned int fan_target_level(int32_t mc, unsigned int cur,
				     unsigned int prof)
{
	const int32_t *trip = fan_trip_mc[prof];
	unsigned int target = 0;
	unsigned int i = 0;

	for (i = RP1_FAN_LEVEL_COUNT; i-- > 1; ) {
		if (mc >= trip[i]) {
			target = i;
			break;
		}
	}

	if (cur && target < cur) {
		if (mc >= trip[cur] - FAN_HYST_MC)
			target = cur;		/* inside hysteresis: hold */
		else
			target = cur - 1;	/* step down one level */
	}

	return target;
}

static bool fan_auto_cb(struct callout *co)
{
	unsigned int prof = 0;
	unsigned int cur = 0;
	unsigned int target = 0;
	int32_t mc = 0;
	uint32_t exceptions = 0;

	/*
	 * Re-assert clk_pwm1 first: once rp1-clk claims it, Linux's
	 * clk_disable_unused can gate the fan clock behind our back.
	 */
	rp1_fan_kick_clock();

	exceptions = cpu_spin_lock_xsave(&fan_lock);
	prof = fan_profile;
	cur = fan_level;
	cpu_spin_unlock_xrestore(&fan_lock, exceptions);

	if (!soc_temp_read_mc(&mc)) {
		if (cur < FAN_LEVEL_SAFE)
			rp1_fan_set_level(FAN_LEVEL_SAFE);
		goto out;
	}

	target = fan_target_level(mc, cur, prof);
	if (target != cur)
		rp1_fan_set_level(target);

out:
	callout_set_next_timeout(co, FAN_CTRL_PERIOD_MS);
	return true;
}

void rp1_fan_enable_auto(void)
{
	uint32_t exceptions = cpu_spin_lock_xsave(&fan_lock);
	bool add = !fan_auto_added;

	fan_auto_added = true;
	cpu_spin_unlock_xrestore(&fan_lock, exceptions);

	if (add) {
		callout_add(&fan_auto_callout, fan_auto_cb, FAN_CTRL_PERIOD_MS);
		IMSG("rp1_fan: autonomous thermal loop enabled (%u ms)",
		     FAN_CTRL_PERIOD_MS);
	}
}
