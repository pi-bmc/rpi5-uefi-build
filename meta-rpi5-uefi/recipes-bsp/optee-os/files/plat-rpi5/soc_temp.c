// SPDX-License-Identifier: BSD-2-Clause
/*
 * BCM2712 SoC die temperature (AVS monitor). See soc_temp.h.
 */

#include <io.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>

#include "soc_temp.h"

#define AVS_MONITOR_BASE	0x107d542000UL
#define AVS_MONITOR_SIZE	0x1000
#define AVS_RO_TEMP_STATUS	0x200
#define AVS_TEMP_VALID_MASK	0x10400
#define AVS_TEMP_RAW_MASK	0x3ff

register_phys_mem_pgdir(MEM_AREA_IO_SEC, AVS_MONITOR_BASE, AVS_MONITOR_SIZE);

bool soc_temp_read_mc(int32_t *milli_c)
{
	vaddr_t va = (vaddr_t)phys_to_virt(AVS_MONITOR_BASE, MEM_AREA_IO_SEC,
					   AVS_MONITOR_SIZE);
	uint32_t sts = 0;

	if (!va)
		return false;

	sts = io_read32(va + AVS_RO_TEMP_STATUS);
	if ((sts & AVS_TEMP_VALID_MASK) != AVS_TEMP_VALID_MASK)
		return false;

	*milli_c = 450000 - 550 * (int32_t)(sts & AVS_TEMP_RAW_MASK);

	return true;
}
