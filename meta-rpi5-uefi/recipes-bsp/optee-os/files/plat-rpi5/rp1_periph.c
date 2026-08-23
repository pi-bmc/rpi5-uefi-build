// SPDX-License-Identifier: BSD-2-Clause
/*
 * Shared RP1 peripheral-window mapping. See rp1_periph.h.
 */

#include <kernel/spinlock.h>
#include <mm/core_mmu.h>
#include <trace.h>

#include "rp1_periph.h"

static unsigned int rp1_lock = SPINLOCK_UNLOCK;
static vaddr_t rp1_base;
static paddr_t rp1_pa;

TEE_Result rp1_periph_map(paddr_t bar_pa)
{
	uint32_t exceptions = 0;
	void *va = NULL;

	if (!bar_pa || (bar_pa & SMALL_PAGE_MASK))
		return TEE_ERROR_BAD_PARAMETERS;

	exceptions = cpu_spin_lock_xsave(&rp1_lock);
	if (rp1_base && rp1_pa == bar_pa) {
		cpu_spin_unlock_xrestore(&rp1_lock, exceptions);
		return TEE_SUCCESS;
	}
	cpu_spin_unlock_xrestore(&rp1_lock, exceptions);

	va = core_mmu_add_mapping(MEM_AREA_IO_SEC, bar_pa,
				 RP1_PERIPH_WINDOW_SIZE);
	if (!va) {
		EMSG("rp1_periph: cannot map RP1 window at %#"PRIxPA, bar_pa);
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	exceptions = cpu_spin_lock_xsave(&rp1_lock);
	rp1_base = (vaddr_t)va;
	rp1_pa = bar_pa;
	cpu_spin_unlock_xrestore(&rp1_lock, exceptions);

	IMSG("rp1_periph: RP1 window mapped, PA %#"PRIxPA, bar_pa);

	return TEE_SUCCESS;
}

vaddr_t rp1_periph_base(void)
{
	uint32_t exceptions = cpu_spin_lock_xsave(&rp1_lock);
	vaddr_t base = rp1_base;

	cpu_spin_unlock_xrestore(&rp1_lock, exceptions);

	return base;
}
