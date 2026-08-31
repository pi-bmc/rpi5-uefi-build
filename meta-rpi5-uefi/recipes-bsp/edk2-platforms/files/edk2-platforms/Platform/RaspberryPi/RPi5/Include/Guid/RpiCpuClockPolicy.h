/** @file

  CpuClockPolicy - the persistent ARM clock policy, shared between
  CpuConfigDxe (efivarstore Setup page + config.txt reconvergence) and
  the Redfish Processor feature driver (RedfishProcessorDxe), which
  consumes standard /Systems/1/Processors/{id} PATCHes into these
  questions.

  The properties mirror the Processor schema (v1_10_0+):

    SpeedLimitMhz  the explicit frequency cap (arm_freq). 0 means "no
                   override": the managed config.txt block is removed
                   and the SoC runs the shipped stock configuration.
    SpeedLocked    TRUE pins the cores at the cap (the shipped
                   force_turbo=1 behavior); FALSE emits force_turbo=0
                   so DVFS may scale below the cap.

  OverVoltageDeltaUv has no Processor-schema counterpart and stays a
  BIOS attribute (CpuOverVoltageDeltaUv).

  This header is included by VFR as well as C: keep it to #defines and
  the varstore struct only.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_CPU_CLOCK_POLICY_H_
#define RPI_CPU_CLOCK_POLICY_H_

//
// Vendor GUID of the CpuClockPolicy variable AND the CpuConfigDxe
// formset (one GUID for both, the ConfigDxe idiom).
//
#define RPI_CPU_CONFIG_FORMSET_GUID \
  { 0xc5e48ba4, 0xc316, 0x4160, { 0x84, 0x13, 0xfe, 0x7d, 0xeb, 0x76, 0x02, 0xb5 } }

#define RPI_CPU_CLOCK_POLICY_VARIABLE_NAME  L"CpuClockPolicy"

//
// The supported cap range. 0 is the "no override" sentinel; a nonzero
// cap is clamped into [MIN, MAX] by the driver (the VFR bounds it too;
// belt and braces for a BMC-written variable). Stock is the shipped
// config.txt ceiling, used for the over-voltage threshold.
//
#define RPI_CPU_CLOCK_STOCK_MHZ  2400
#define RPI_CPU_CLOCK_MIN_MHZ    1500
#define RPI_CPU_CLOCK_MAX_MHZ    3000

//
// over_voltage_delta bounds, in microvolts.
//
#define RPI_CPU_CLOCK_DELTA_DEFAULT_UV  50000
#define RPI_CPU_CLOCK_DELTA_MAX_UV      100000

//
// Question id of the page's interactive apply action (BlCfg owns 0x1000).
//
#define RPI_CPU_CLOCK_KEY_APPLY  0x1200

#pragma pack (1)
typedef struct {
  UINT16    SpeedLimitMhz;         // 0 = no override; else clamped to 1500..3000
  UINT8     SpeedLocked;           // BOOLEAN: pin at the cap (force_turbo)
  UINT8     Reserved;              // keeps the layout distinguishable from the
                                   // retired 7-byte profile layout; write 0
  UINT32    OverVoltageDeltaUv;    // applied only when the cap exceeds stock
} RPI_CPU_CLOCK_POLICY;
#pragma pack ()

#endif // RPI_CPU_CLOCK_POLICY_H_
