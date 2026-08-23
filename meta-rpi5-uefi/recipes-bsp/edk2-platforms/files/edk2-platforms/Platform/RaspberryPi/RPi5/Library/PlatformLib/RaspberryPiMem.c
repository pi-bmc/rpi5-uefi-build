/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *  Copyright (c) 2019, Pete Batard <pete@akeo.ie>
 *  Copyright (c) 2017-2018, Andrey Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) 2014, Linaro Limited. All rights reserved.
 *  Copyright (c) 2013-2018, ARM Limited. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/RPiMem.h>
#include <Library/BoardInfoLib.h>
#include <Library/BoardRevisionHelperLib.h>

UINT64 mSystemMemoryBase;
extern UINT64 mSystemMemoryEnd;

// The total number of descriptors, including the final "end-of-table" descriptor.
#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS 14

STATIC BOOLEAN                  VirtualMemoryInfoInitialized = FALSE;
STATIC RPI_MEMORY_REGION_INFO   VirtualMemoryInfo[MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS];

#define VariablesBase (FixedPcdGet32(PcdNvStorageVariableBase))

/**
  Return the Virtual Memory Map of your platform

  This Virtual Memory Map is used by MemoryInitPei Module to initialize the MMU
  on your platform.

  @param[out]   VirtualMemoryMap    Array of ARM_MEMORY_REGION_DESCRIPTOR
                                    describing a Physical-to-Virtual Memory
                                    mapping. This array must be ended by a
                                    zero-filled entry

**/
VOID
ArmPlatformGetVirtualMemoryMap (
  IN ARM_MEMORY_REGION_DESCRIPTOR** VirtualMemoryMap
  )
{
  EFI_STATUS                    Status;
  UINT32                        RevisionCode = 0;
  UINT64                        TotalMemorySize;
  UINT64                        MemorySizeBelow3GB;
  UINT64                        MemorySizeBelow4GB;
  UINTN                         Index = 0;
  ARM_MEMORY_REGION_DESCRIPTOR  *VirtualMemoryTable;

  Status = BoardInfoGetRevisionCode (&RevisionCode);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get board revision. Status=%r\n",
            __func__, Status));
    ASSERT (FALSE);
    CpuDeadLoop ();
  }

  // Early output of the info we got from VideoCore can prove valuable.
  DEBUG ((DEBUG_INFO, "Board Rev: 0x%lX\n", RevisionCode));
  DEBUG ((DEBUG_INFO, "RAM < 1GB: 0x%ll08X (Size 0x%ll08X)\n", mSystemMemoryBase, mSystemMemoryEnd + 1));

  ASSERT (mSystemMemoryBase == 0);
  ASSERT (VirtualMemoryMap != NULL);

  // Compute the total RAM size available on this platform
  TotalMemorySize = BoardRevisionGetMemorySize (RevisionCode);
  DEBUG ((DEBUG_INFO, "Total RAM: 0x%ll08X\n", TotalMemorySize));

  MemorySizeBelow3GB = MIN(TotalMemorySize, 3UL * SIZE_1GB);
  MemorySizeBelow4GB = MIN(TotalMemorySize, 4UL * SIZE_1GB);

  VirtualMemoryTable = (ARM_MEMORY_REGION_DESCRIPTOR*)AllocatePages
                       (EFI_SIZE_TO_PAGES (sizeof (ARM_MEMORY_REGION_DESCRIPTOR) *
                       MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS));
  if (VirtualMemoryTable == NULL) {
    return;
  }

  // Firmware Volume
  VirtualMemoryTable[Index].PhysicalBase    = FixedPcdGet64 (PcdFdBaseAddress);
  VirtualMemoryTable[Index].VirtualBase     = VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Length          = VariablesBase - VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Attributes      = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  VirtualMemoryInfo[Index].Type             = RPI_MEM_RESERVED_REGION;
  VirtualMemoryInfo[Index++].Name           = L"FD";

  // Variable Volume
  VirtualMemoryTable[Index].PhysicalBase    = VariablesBase;
  VirtualMemoryTable[Index].VirtualBase     = VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Length          = FixedPcdGet32 (PcdFdtBaseAddress) - VariablesBase;
  VirtualMemoryTable[Index].Attributes      = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  VirtualMemoryInfo[Index].Type             = RPI_MEM_RUNTIME_REGION;
  VirtualMemoryInfo[Index++].Name           = L"FD Variables";

  // DTB is expected to directly follow the FD.
  VirtualMemoryTable[Index].PhysicalBase    = FixedPcdGet32 (PcdFdtBaseAddress);
  VirtualMemoryTable[Index].VirtualBase     = VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Length          = FixedPcdGet32 (PcdFdtSize);
  VirtualMemoryTable[Index].Attributes      = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  VirtualMemoryInfo[Index].Type             = RPI_MEM_RESERVED_REGION;
  VirtualMemoryInfo[Index++].Name           = L"Flattened Device Tree";

  //
  // Base System RAM. With OP-TEE built in (PcdOpteeTzdramSize != 0), punch
  // its carve-out [TZDRAM | static SHM] out of this range: the TZDRAM half
  // gets the GPU-Reserved treatment (device-mapped, no HOB, invisible to
  // the OS), the SHM half stays mapped write-back with a reserved-memory
  // HOB so OpteeLib can talk to OP-TEE over it. The carve-out must lie
  // wholly inside this region or it is skipped (and OP-TEE, which TF-A
  // placed at compile time, would be left unprotected -- keep the PCDs,
  // the TF-A defines and plat-rpi5 conf.mk in step).
  //
  if ((FixedPcdGet32 (PcdOpteeTzdramSize) != 0) &&
      (FixedPcdGet64 (PcdOpteeTzdramBase) > FixedPcdGet64 (PcdSystemMemoryBase)) &&
      (FixedPcdGet64 (PcdOpteeTzdramBase) + FixedPcdGet32 (PcdOpteeTzdramSize) +
       FixedPcdGet32 (PcdOpteeShmSize) <= mSystemMemoryEnd + 1)) {
    VirtualMemoryTable[Index].PhysicalBase  = FixedPcdGet64 (PcdSystemMemoryBase);
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = FixedPcdGet64 (PcdOpteeTzdramBase) - FixedPcdGet64 (PcdSystemMemoryBase);
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_BASIC_REGION;
    VirtualMemoryInfo[Index++].Name         = L"System RAM < 1GB";

    VirtualMemoryTable[Index].PhysicalBase  = FixedPcdGet64 (PcdOpteeTzdramBase);
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = FixedPcdGet32 (PcdOpteeTzdramSize);
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_UNMAPPED_REGION;
    VirtualMemoryInfo[Index++].Name         = L"OP-TEE TZDRAM";

    VirtualMemoryTable[Index].PhysicalBase  = FixedPcdGet64 (PcdOpteeTzdramBase) + FixedPcdGet32 (PcdOpteeTzdramSize);
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = FixedPcdGet32 (PcdOpteeShmSize);
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_RESERVED_REGION;
    VirtualMemoryInfo[Index++].Name         = L"OP-TEE Shared Memory";

    VirtualMemoryTable[Index].PhysicalBase  = VirtualMemoryTable[Index - 1].PhysicalBase + VirtualMemoryTable[Index - 1].Length;
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = mSystemMemoryEnd + 1 - VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_BASIC_REGION;
    VirtualMemoryInfo[Index++].Name         = L"System RAM < 1GB (high)";
  } else {
    VirtualMemoryTable[Index].PhysicalBase  = FixedPcdGet64 (PcdSystemMemoryBase);
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = mSystemMemoryEnd + 1 - FixedPcdGet64 (PcdSystemMemoryBase);
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_BASIC_REGION;
    VirtualMemoryInfo[Index++].Name         = L"System RAM < 1GB";
  }

  // GPU Reserved
  VirtualMemoryTable[Index].PhysicalBase    = mSystemMemoryEnd + 1;
  VirtualMemoryTable[Index].VirtualBase     = VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Length          = SIZE_1GB - VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Attributes      = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  VirtualMemoryInfo[Index].Type             = RPI_MEM_UNMAPPED_REGION;
  VirtualMemoryInfo[Index++].Name           = L"GPU Reserved";

  // Memory in the 1GB - 3GB range is always available.
  if (MemorySizeBelow3GB > SIZE_1GB) {
    VirtualMemoryTable[Index].PhysicalBase  = SIZE_1GB;
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = MemorySizeBelow3GB - VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_BASIC_REGION;
    VirtualMemoryInfo[Index++].Name         = L"Extended System RAM < 3GB";
  }

  //
  // Memory in the 3GB - 4GB range may be reserved for 32-bit PCIe BAR space.
  // RpiPlatformDxe can reclaim part of it as system RAM depending on the needs.
  //
  if (MemorySizeBelow4GB > 3UL * SIZE_1GB) {
    VirtualMemoryTable[Index].PhysicalBase  = 3UL * SIZE_1GB;
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = MemorySizeBelow4GB - VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_UNMAPPED_REGION;
    VirtualMemoryInfo[Index++].Name         = L"Extended System RAM < 4GB";
  }

  if (TotalMemorySize > 4UL * SIZE_1GB) {
    VirtualMemoryTable[Index].PhysicalBase  = 4UL * SIZE_1GB;
    VirtualMemoryTable[Index].VirtualBase   = VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Length        = TotalMemorySize - VirtualMemoryTable[Index].PhysicalBase;
    VirtualMemoryTable[Index].Attributes    = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    VirtualMemoryInfo[Index].Type           = RPI_MEM_BASIC_REGION;
    VirtualMemoryInfo[Index++].Name         = L"Extended System RAM >= 4GB";
  }

  // SoC device registers
  VirtualMemoryTable[Index].PhysicalBase    = SIZE_64GB;
  VirtualMemoryTable[Index].VirtualBase     = VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Length          = SIZE_64GB;
  VirtualMemoryTable[Index].Attributes      = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  VirtualMemoryInfo[Index].Type             = RPI_MEM_UNMAPPED_REGION;
  VirtualMemoryInfo[Index++].Name           = L"MMIO";

  // End of Table
  VirtualMemoryTable[Index].PhysicalBase    = 0;
  VirtualMemoryTable[Index].VirtualBase     = 0;
  VirtualMemoryTable[Index].Length          = 0;
  VirtualMemoryTable[Index++].Attributes    = (ARM_MEMORY_REGION_ATTRIBUTES)0;

  ASSERT(Index <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);

  *VirtualMemoryMap = VirtualMemoryTable;
  VirtualMemoryInfoInitialized = TRUE;
}

/**
  Return additional memory info not populated by the above call.

  This call should follow the one to ArmPlatformGetVirtualMemoryMap ().

**/
VOID
RpiPlatformGetVirtualMemoryInfo (
  IN RPI_MEMORY_REGION_INFO** MemoryInfo
  )
{
  ASSERT (VirtualMemoryInfo != NULL);

  if (!VirtualMemoryInfoInitialized) {
    DEBUG ((DEBUG_ERROR,
      "ArmPlatformGetVirtualMemoryMap must be called before RpiPlatformGetVirtualMemoryInfo.\n"));
    return;
  }

  *MemoryInfo = VirtualMemoryInfo;
}
