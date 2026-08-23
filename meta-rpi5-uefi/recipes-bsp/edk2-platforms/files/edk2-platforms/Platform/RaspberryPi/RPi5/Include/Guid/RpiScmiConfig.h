/** @file

  PowerProfile - the persistent SCMI power-profile variable, the high-level
  control that bundles the SCMI-managed subsystems (fan curve today; CPU
  DVFS cap once a CPU performance domain exists) into named presets.

  RpiScmiConfigDxe owns the Setup page (efivarstore producer) and exposes
  PowerProfile as a Redfish BIOS attribute; ActiveCoolerScmiDxe consumes it
  to pick the fan curve. A "Manual" profile defers to the detailed Fan
  page's FanPolicy variable, so the two coexist.

  This header is included by VFR as well as C: keep it to #defines and the
  varstore struct only.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_SCMI_CONFIG_H_
#define RPI_SCMI_CONFIG_H_

//
// Vendor GUID of the PowerProfile variable AND the RpiScmiConfigDxe
// formset (one GUID for both, the ConfigDxe idiom).
//
#define RPI_SCMI_CONFIG_FORMSET_GUID \
  { 0x2d9a7f13, 0xc4e8, 0x4b06, { 0x9f, 0x31, 0x7a, 0x5c, 0x0e, 0x82, 0x6d, 0x44 } }

#define RPI_POWER_PROFILE_VARIABLE_NAME  L"PowerProfile"

//
// Profiles. Balanced is the shipped default and matches the fan behavior
// that existed before profiles (the AUTO/CUSTOM 50/60/68/75 trip curve).
// Quiet raises the trips (fan spins later, less noise); Cool lowers them
// (fan spins early, lower sustained temperature). Manual hands fan control
// back to the FanPolicy page.
//
#define RPI_POWER_PROFILE_BALANCED     0
#define RPI_POWER_PROFILE_QUIET        1
#define RPI_POWER_PROFILE_COOL         2
#define RPI_POWER_PROFILE_MANUAL       3

#pragma pack (1)
typedef struct {
  UINT8    Profile;    // RPI_POWER_PROFILE_*
} RPI_POWER_PROFILE;
#pragma pack ()

#endif // RPI_SCMI_CONFIG_H_
