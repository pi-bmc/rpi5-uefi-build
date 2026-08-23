// SPDX-License-Identifier: BSD-2-Clause
/*
 * BCM2712 VPU mailbox (property channel) driver. See vpu_mbox.h.
 *
 * Register interface per the BCM2835 mailbox the 2712 retains: MAIL0
 * (VC->ARM) read + status, MAIL1 (ARM->VC) write + status, channel in
 * the low 4 bits of each word. The property interface is channel 8; the
 * word exchanged is the physical address of a 16-byte-aligned buffer
 * (the SoC node carries no dma-ranges, so bus address == PA; OP-TEE
 * core .bss sits below 4 GB as the interface requires).
 *
 * The VPU reads and writes the buffer directly, so the cache is cleaned
 * before ringing and invalidated before parsing the response.
 */

#include <io.h>
#include <kernel/cache_helpers.h>
#include <kernel/delay.h>
#include <kernel/spinlock.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <string.h>
#include <trace.h>
#include <util.h>

#include "vpu_mbox.h"

#define MBOX_BASE		0x107c013880UL
#define MBOX_SIZE		0x40

#define MAIL0_RD		0x00	/* VC -> ARM */
#define MAIL0_STATUS		0x18
#define MAIL1_WRT		0x20	/* ARM -> VC */
#define MAIL1_STATUS		0x38

#define MBOX_STATUS_FULL	BIT(31)
#define MBOX_STATUS_EMPTY	BIT(30)

#define MBOX_CHAN_PROPERTY	8
#define MBOX_CHAN_MASK		0xf

/* Bounded poll: ~10 ms at 10 us steps. VPU property calls are sub-ms. */
#define MBOX_POLL_STEP_US	10
#define MBOX_POLL_MAX		1000

/* Property buffer codes */
#define PROP_CODE_REQUEST	0
#define PROP_CODE_RESP_OK	0x80000000
#define PROP_TAG_RESP		BIT(31)

/* Firmware clock property tags */
#define TAG_GET_CLOCK_RATE	0x00030002
#define TAG_GET_MAX_CLOCK_RATE	0x00030004
#define TAG_SET_CLOCK_RATE	0x00038002

register_phys_mem_pgdir(MEM_AREA_IO_SEC, MBOX_BASE, MBOX_SIZE);

/*
 * One in-flight property call at a time; cache-line aligned and sized
 * so the maintenance below never touches neighbouring data.
 */
#define MBOX_BUF_WORDS		64
static uint32_t mbox_buf[MBOX_BUF_WORDS] __aligned(64);

static unsigned int mbox_lock = SPINLOCK_UNLOCK;
static bool mbox_owned;

void vpu_mbox_set_owned(void)
{
	if (!mbox_owned)
		IMSG("vpu_mbox: normal world handed the mailbox over");
	mbox_owned = true;
}

bool vpu_mbox_owned(void)
{
	return mbox_owned;
}

static vaddr_t mbox_base(void)
{
	return (vaddr_t)phys_to_virt(MBOX_BASE, MEM_AREA_IO_SEC, MBOX_SIZE);
}

static bool mbox_poll(vaddr_t status_reg, uint32_t busy_bit)
{
	unsigned int n = 0;

	for (n = 0; n < MBOX_POLL_MAX; n++) {
		if (!(io_read32(status_reg) & busy_bit))
			return true;
		udelay(MBOX_POLL_STEP_US);
	}

	return false;
}

/* Caller holds mbox_lock; buffer prepared in mbox_buf. */
static TEE_Result mbox_xfer_locked(void)
{
	vaddr_t base = mbox_base();
	paddr_t pa = virt_to_phys(mbox_buf);
	uint32_t msg = 0;
	uint32_t reply = 0;

	/* The property channel carries a 32-bit bus address (channel in the
	 * low 4 bits), so the buffer must sit below 4 GB. */
	if (!base || !pa || (pa & MBOX_CHAN_MASK) || pa > UINT32_MAX)
		return TEE_ERROR_GENERIC;

	msg = (uint32_t)pa | MBOX_CHAN_PROPERTY;

	dcache_clean_range(mbox_buf, sizeof(mbox_buf));

	if (!mbox_poll(base + MAIL1_STATUS, MBOX_STATUS_FULL))
		return TEE_ERROR_BUSY;
	io_write32(base + MAIL1_WRT, msg);

	/*
	 * The VPU replies with the same buffer address on the property
	 * channel; drain anything else (there should be nothing - OP-TEE
	 * is the only owner by the time this driver may run).
	 */
	while (true) {
		if (!mbox_poll(base + MAIL0_STATUS, MBOX_STATUS_EMPTY))
			return TEE_ERROR_BUSY;
		reply = io_read32(base + MAIL0_RD);
		if ((reply & MBOX_CHAN_MASK) == MBOX_CHAN_PROPERTY &&
		    (reply & ~MBOX_CHAN_MASK) == (uint32_t)pa)
			break;
	}

	dcache_inv_range(mbox_buf, sizeof(mbox_buf));

	if (mbox_buf[1] != PROP_CODE_RESP_OK)
		return TEE_ERROR_COMMUNICATION;

	return TEE_SUCCESS;
}

TEE_Result vpu_mbox_prop_call(uint32_t tag, const uint32_t *req,
			      size_t req_len, uint32_t *resp, size_t resp_cap)
{
	size_t val_len = MAX(req_len, resp_cap);
	size_t val_words = ROUNDUP_DIV(val_len, sizeof(uint32_t));
	uint32_t exceptions = 0;
	TEE_Result res = TEE_SUCCESS;

	if (!mbox_owned)
		return TEE_ERROR_BAD_STATE;
	if (6 + val_words > MBOX_BUF_WORDS)
		return TEE_ERROR_EXCESS_DATA;

	exceptions = cpu_spin_lock_xsave(&mbox_lock);

	memset(mbox_buf, 0, sizeof(mbox_buf));
	mbox_buf[0] = (6 + val_words) * sizeof(uint32_t);  /* total size */
	mbox_buf[1] = PROP_CODE_REQUEST;
	mbox_buf[2] = tag;
	mbox_buf[3] = val_words * sizeof(uint32_t);        /* value buf size */
	mbox_buf[4] = 0;                                   /* request indicator */
	if (req && req_len)
		memcpy(&mbox_buf[5], req, req_len);
	/* mbox_buf[5 + val_words] = 0: end tag, via the memset */

	res = mbox_xfer_locked();
	if (!res) {
		if (!(mbox_buf[4] & PROP_TAG_RESP)) {
			res = TEE_ERROR_COMMUNICATION;
		} else if (resp && resp_cap) {
			memcpy(resp, &mbox_buf[5],
			       MIN(resp_cap, (size_t)(mbox_buf[4] &
						      ~PROP_TAG_RESP)));
		}
	}

	cpu_spin_unlock_xrestore(&mbox_lock, exceptions);

	return res;
}

uint32_t vpu_clock_get_rate(uint32_t clock_id)
{
	uint32_t req[2] = { clock_id, 0 };
	uint32_t resp[2] = { };

	if (vpu_mbox_prop_call(TAG_GET_CLOCK_RATE, req, sizeof(req),
			       resp, sizeof(resp)))
		return 0;

	return resp[1];
}

uint32_t vpu_clock_get_max_rate(uint32_t clock_id)
{
	uint32_t req[2] = { clock_id, 0 };
	uint32_t resp[2] = { };

	if (vpu_mbox_prop_call(TAG_GET_MAX_CLOCK_RATE, req, sizeof(req),
			       resp, sizeof(resp)))
		return 0;

	return resp[1];
}

TEE_Result vpu_clock_set_rate(uint32_t clock_id, uint32_t hz)
{
	uint32_t req[3] = { clock_id, hz, 0 /* no turbo skip */ };
	uint32_t resp[2] = { };
	TEE_Result res = TEE_SUCCESS;

	res = vpu_mbox_prop_call(TAG_SET_CLOCK_RATE, req, sizeof(req),
				 resp, sizeof(resp));
	if (res)
		return res;

	if (resp[0] != clock_id || !resp[1])
		return TEE_ERROR_COMMUNICATION;

	return TEE_SUCCESS;
}
