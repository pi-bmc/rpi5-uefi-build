/** @file

  FanPolicy - the persistent active-cooler policy variable, shared between
  ActiveCoolerDxe (consumer, re-read every poll tick so changes apply
  live), FanConfigDxe's Setup page (efivarstore producer) and any BMC-side
  writer that reaches UEFI variables.

  This header is included by VFR as well as C: keep it to #defines and the
  varstore struct only.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_FAN_POLICY_H_
#define RPI_FAN_POLICY_H_

//
// Vendor GUID of the FanPolicy variable AND the FanConfigDxe formset
// (one GUID for both, the ConfigDxe idiom).
//
#define RPI_FAN_CONFIG_FORMSET_GUID \
  { 0x6c2b9f4a, 0xe0d3, 0x4f8b, { 0x84, 0x59, 0xa2, 0x0f, 0x6d, 0x4e, 0x91, 0xc7 } }

#define RPI_FAN_POLICY_VARIABLE_NAME  L"FanPolicy"

#define RPI_FAN_MODE_AUTO    0    // built-in DTB cooling curve
#define RPI_FAN_MODE_FIXED   1    // constant FixedLevel
#define RPI_FAN_MODE_CUSTOM  2    // auto loop with Trip1C..Trip4C thresholds

#pragma pack (1)
typedef struct {
  UINT8    Mode;          // RPI_FAN_MODE_*
  UINT8    FixedLevel;    // 0..4, used in FIXED mode
  //
  // Custom trip points in whole degrees Celsius for cooling levels 1..4,
  // used in CUSTOM mode. Must be strictly ascending, each within 30..90;
  // ActiveCoolerDxe falls back to the built-in trips otherwise. The 5 C
  // step-down hysteresis is fixed.
  //
  UINT8    Trip1C;
  UINT8    Trip2C;
  UINT8    Trip3C;
  UINT8    Trip4C;
} RPI_FAN_POLICY;
#pragma pack ()

#endif // RPI_FAN_POLICY_H_
