/** @file

  CpuClockPolicy - the persistent CPU clock profile, shared between
  CpuConfigDxe's Setup page (efivarstore producer, and the ReadyToBoot
  sync that converges config.txt to it) and any BMC-side writer that
  reaches UEFI variables.

  The variable is the source of truth; the arm_freq/over_voltage_delta
  managed block in config.txt on the boot volume is derived state,
  reconverged every boot. The VPU bootloader reads config.txt at
  power-on, so a profile change takes effect on the next reset.

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

#define RPI_CPU_CLOCK_PROFILE_DEFAULT  0    // stock 2400 MHz, no managed block
#define RPI_CPU_CLOCK_PROFILE_OC_2800  1    // arm_freq=2800
#define RPI_CPU_CLOCK_PROFILE_OC_3000  2    // arm_freq=3000
#define RPI_CPU_CLOCK_PROFILE_CUSTOM   3    // arm_freq=CustomMhz

//
// The BCM2712's stock ceiling and the range the Custom profile accepts.
// 3000 is the customary Pi 5 overclock target; how far a given board
// gets is silicon lottery, and the VPU firmware still thermal-throttles
// regardless of what is configured here.
//
#define RPI_CPU_CLOCK_DEFAULT_MHZ  2400
#define RPI_CPU_CLOCK_MIN_MHZ      1500
#define RPI_CPU_CLOCK_MAX_MHZ      3000

//
// over_voltage_delta in microvolts, applied only when the effective
// frequency exceeds stock. 50000 (50 mV) is the customary companion to
// a 2.8-3.0 GHz overclock.
//
#define RPI_CPU_CLOCK_DELTA_DEFAULT_UV  50000
#define RPI_CPU_CLOCK_DELTA_MAX_UV      100000

//
// QuestionId of the interactive "apply" action (BlCfg uses 0x1000).
//
#define RPI_CPU_CLOCK_KEY_APPLY  0x1200

#pragma pack (1)
typedef struct {
  UINT8     Profile;               // RPI_CPU_CLOCK_PROFILE_*
  UINT16    CustomMhz;             // used in CUSTOM profile
  UINT32    OverVoltageDeltaUv;    // written when effective MHz > stock
} RPI_CPU_CLOCK_POLICY;
#pragma pack ()

#endif // RPI_CPU_CLOCK_POLICY_H_
