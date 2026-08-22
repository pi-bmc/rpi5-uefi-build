/** @file
 *
 *  SecureBootToggleDxe's Setup formset GUID and varstore layout.
 *
 *  The varstore is NOT this GUID: the questions bind straight to
 *  SecurityPkg's SecureBootEnable variable (EFI_SECURE_BOOT_ENABLE_NAME
 *  under gEfiSecureBootEnableDisableGuid), which is what the variable
 *  driver's AuthVariableLib reads to decide whether to enforce Secure
 *  Boot. This GUID only identifies the formset and its vendor device path.
 *
 *  This header is included by VFR as well as C: keep it to #defines and the
 *  varstore struct only. gRpiSecureBootToggleFormSetGuid reaches C through
 *  AutoGen, from the INF's [Guids] entry.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef RPI_SECURE_BOOT_TOGGLE_H_
#define RPI_SECURE_BOOT_TOGGLE_H_

#define RPI_SECURE_BOOT_TOGGLE_FORMSET_GUID                            \
  { 0x2f9c7e18, 0x5a64, 0x4b3d, { 0xa7, 0x1e, 0xc8, 0x35, 0x0d, 0x62,  \
                                  0x84, 0x93 } }

//
// Vendor GUID of SecurityPkg's SecureBootEnable variable - the value of
// EFI_SECURE_BOOT_ENABLE_DISABLE from
// SecurityPkg/Include/Guid/AuthenticatedVariableFormat.h, repeated here
// because that header also declares extern EFI_GUIDs and so cannot be
// included from a .vfr. The C side includes the real header; keep the two
// in step.
//
#define RPI_SECURE_BOOT_ENABLE_VARIABLE_GUID                           \
  { 0xf0a30bc7, 0xaf08, 0x4556, { 0x99, 0xc4, 0x00, 0x10, 0x09, 0xc9,  \
                                  0x3a, 0x44 } }

//
// Overlays the one-byte SecureBootEnable variable. Values are SecurityPkg's
// SECURE_BOOT_ENABLE (1) / SECURE_BOOT_DISABLE (0), which is exactly what a
// HII checkbox stores, so the checkbox needs no translation.
//
#pragma pack (1)
typedef struct {
  UINT8    Enable;
} RPI_SECURE_BOOT_TOGGLE_VARSTORE_DATA;
#pragma pack ()

#endif // RPI_SECURE_BOOT_TOGGLE_H_
