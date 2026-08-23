// SPDX-License-Identifier: BSD-2-Clause
/*
 * Polled-mode master driver for the RP1's DW_apb_i2c.
 *
 * Register-level behaviour and timing values match the pi-bmc project's
 * EDK2 Rp1DwI2cDxe bring-up (proven on hardware against the
 * BMC-emulated 24c256): clk_sys is a fixed 200 MHz (neither the VPU nor
 * Linux ever reparents it), SCL counts derived from the DW databook
 * with tf = 300 ns, 50 ns spike filter, 300 ns SDA hold.
 */

#include <io.h>
#include <kernel/delay.h>
#include <trace.h>

#include "rp1_i2c.h"

#define DW_IC_CON		0x00
#define DW_IC_CON_MASTER	BIT(0)
#define DW_IC_CON_SPEED_STD	BIT(1)
#define DW_IC_CON_RESTART_EN	BIT(5)
#define DW_IC_CON_SLAVE_DISABLE	BIT(6)
#define DW_IC_TAR		0x04
#define DW_IC_DATA_CMD		0x10
#define DW_IC_DATA_CMD_STOP	BIT(9)
#define DW_IC_SS_SCL_HCNT	0x14
#define DW_IC_SS_SCL_LCNT	0x18
#define DW_IC_INTR_MASK		0x30
#define DW_IC_RAW_INTR_STAT	0x34
#define DW_IC_RAW_TX_ABRT	BIT(6)
#define DW_IC_RAW_STOP_DET	BIT(9)
#define DW_IC_CLR_INTR		0x40
#define DW_IC_CLR_TX_ABRT	0x54
#define DW_IC_CLR_STOP_DET	0x60
#define DW_IC_ENABLE		0x6c
#define DW_IC_ENABLE_ENABLE	BIT(0)
#define DW_IC_STATUS		0x70
#define DW_IC_STATUS_TFNF	BIT(1)
#define DW_IC_STATUS_TFE	BIT(2)
#define DW_IC_STATUS_MST_ACT	BIT(5)
#define DW_IC_SDA_HOLD		0x7c
#define DW_IC_TX_ABRT_SOURCE	0x80
#define DW_IC_ENABLE_STATUS	0x9c
#define DW_IC_FS_SPKLEN		0xa0
#define DW_IC_COMP_TYPE		0xfc
#define DW_IC_COMP_TYPE_VALUE	0x44570140

/* clk_sys: fixed 200 MHz */
#define RP1_I2C_CLK_KHZ		200000

/*
 * DW databook: high phase covers tHIGH + tf minus 3 cycles of internal
 * latency, low phase covers tLOW + tf minus 1 cycle; tf = 300 ns.
 * Standard mode (100 kHz): tHIGH 4000 ns -> 857, tLOW 4700 ns -> 999.
 */
#define DW_SCL_HCNT(high_ns) \
	((RP1_I2C_CLK_KHZ * ((high_ns) + 300) + 500000) / 1000000 - 3)
#define DW_SCL_LCNT(low_ns) \
	((RP1_I2C_CLK_KHZ * ((low_ns) + 300) + 500000) / 1000000 - 1)
#define DW_SS_SCL_HCNT_VALUE	DW_SCL_HCNT(4000)	/* 857 */
#define DW_SS_SCL_LCNT_VALUE	DW_SCL_LCNT(4700)	/* 999 */
#define DW_FS_SPKLEN_VALUE	(RP1_I2C_CLK_KHZ * 50 / 1000000)   /* 10 */
#define DW_SDA_HOLD_VALUE	(RP1_I2C_CLK_KHZ * 300 / 1000000)  /* 60 */

#define DW_ENABLE_TIMEOUT_US	2500
#define DW_BYTE_TIMEOUT_US	50000
#define DW_XFER_TIMEOUT_US	500000

static uint32_t i2c_read(struct rp1_i2c *i2c, unsigned int reg)
{
	return io_read32(i2c->base + reg);
}

static void i2c_write(struct rp1_i2c *i2c, unsigned int reg, uint32_t val)
{
	io_write32(i2c->base + reg, val);
}

static TEE_Result i2c_set_enable(struct rp1_i2c *i2c, bool enable)
{
	uint64_t tref = timeout_init_us(DW_ENABLE_TIMEOUT_US);
	uint32_t want = enable ? DW_IC_ENABLE_ENABLE : 0;

	i2c_write(i2c, DW_IC_ENABLE, want);
	while ((i2c_read(i2c, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_ENABLE) !=
	       want) {
		if (timeout_elapsed(tref))
			return TEE_ERROR_BUSY;
		udelay(25);
	}

	return TEE_SUCCESS;
}

TEE_Result rp1_i2c_init(struct rp1_i2c *i2c, vaddr_t base)
{
	TEE_Result res = TEE_SUCCESS;
	uint32_t id = 0;

	i2c->base = base;
	i2c->configured = false;

	id = i2c_read(i2c, DW_IC_COMP_TYPE);
	if (id != DW_IC_COMP_TYPE_VALUE) {
		EMSG("rp1_i2c: bad DW component id %#"PRIx32
		     " (link down or BAR moved?)", id);
		return TEE_ERROR_ITEM_NOT_FOUND;
	}

	res = i2c_set_enable(i2c, false);
	if (res)
		return res;

	i2c_write(i2c, DW_IC_CON, DW_IC_CON_MASTER | DW_IC_CON_SPEED_STD |
				  DW_IC_CON_RESTART_EN |
				  DW_IC_CON_SLAVE_DISABLE);
	i2c_write(i2c, DW_IC_SS_SCL_HCNT, DW_SS_SCL_HCNT_VALUE);
	i2c_write(i2c, DW_IC_SS_SCL_LCNT, DW_SS_SCL_LCNT_VALUE);
	i2c_write(i2c, DW_IC_FS_SPKLEN, DW_FS_SPKLEN_VALUE);
	i2c_write(i2c, DW_IC_SDA_HOLD, DW_SDA_HOLD_VALUE);
	i2c_write(i2c, DW_IC_INTR_MASK, 0);	/* fully polled */

	i2c->configured = true;

	return TEE_SUCCESS;
}

TEE_Result rp1_i2c_write(struct rp1_i2c *i2c, uint8_t slave_addr,
			 const uint8_t *buf, size_t len)
{
	TEE_Result res = TEE_SUCCESS;
	uint64_t tref = 0;
	size_t n = 0;

	if (!i2c->configured)
		return TEE_ERROR_BAD_STATE;
	if (!len)
		return TEE_ERROR_BAD_PARAMETERS;

	/* IC_TAR may only change while the controller is disabled */
	res = i2c_set_enable(i2c, false);
	if (res)
		return res;
	i2c_write(i2c, DW_IC_TAR, slave_addr);
	res = i2c_set_enable(i2c, true);
	if (res)
		return res;
	i2c_read(i2c, DW_IC_CLR_INTR);

	for (n = 0; n < len; n++) {
		tref = timeout_init_us(DW_BYTE_TIMEOUT_US);
		while (!(i2c_read(i2c, DW_IC_STATUS) & DW_IC_STATUS_TFNF)) {
			if (i2c_read(i2c, DW_IC_RAW_INTR_STAT) &
			    DW_IC_RAW_TX_ABRT)
				goto abort;
			if (timeout_elapsed(tref)) {
				res = TEE_ERROR_BUSY;
				goto out;
			}
			udelay(10);
		}
		i2c_write(i2c, DW_IC_DATA_CMD,
			  buf[n] | (n == len - 1 ? DW_IC_DATA_CMD_STOP : 0));
	}

	/* Wait for the FIFO to drain and the STOP to go out */
	tref = timeout_init_us(DW_XFER_TIMEOUT_US);
	for (;;) {
		uint32_t stat = i2c_read(i2c, DW_IC_STATUS);

		if (i2c_read(i2c, DW_IC_RAW_INTR_STAT) & DW_IC_RAW_TX_ABRT)
			goto abort;
		if ((stat & DW_IC_STATUS_TFE) &&
		    !(stat & DW_IC_STATUS_MST_ACT))
			break;
		if (timeout_elapsed(tref)) {
			res = TEE_ERROR_BUSY;
			goto out;
		}
		udelay(10);
	}

	i2c_read(i2c, DW_IC_CLR_STOP_DET);
	res = TEE_SUCCESS;
	goto out;

abort:
	DMSG("rp1_i2c: TX abort, source %#"PRIx32,
	     i2c_read(i2c, DW_IC_TX_ABRT_SOURCE));
	i2c_read(i2c, DW_IC_CLR_TX_ABRT);
	res = TEE_ERROR_COMMUNICATION;
out:
	i2c_set_enable(i2c, false);
	return res;
}
