/** @file

  BlCfg - the bootloader EEPROM configuration varstore shared between
  BootloaderConfigDxe's Setup page (efivarstore) and its staging logic.

  The variable never configures anything by itself: every boot it is
  rewritten from the live blconfig region (the EEPROM config text the VPU
  hands over in the DTB), so it always mirrors what the running bootloader
  actually used. Saved edits only take effect through the page's explicit
  "stage EEPROM update" action, which writes pieeprom.upd/pieeprom.sig to
  the boot partition and reboots into the bootloader's self-update.

  This header is included by VFR as well as C: keep it to #defines and the
  varstore struct only.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_BLCFG_H_
#define RPI_BLCFG_H_

//
// Vendor GUID of the BlCfg / BlCfgStaged variables AND the
// BootloaderConfigDxe formset (one GUID for both, the ConfigDxe idiom).
//
#define RPI_BLCFG_FORMSET_GUID \
  { 0x8f3a5c12, 0x6b4d, 0x4e2a, { 0x9d, 0x07, 0x4b, 0xaa, 0x1c, 0x86, 0x33, 0xf5 } }

#define RPI_BLCFG_VARIABLE_NAME         L"BlCfg"

//
// Written when an EEPROM update has been staged to the boot partition;
// holds the staged values. Cleared on the next boot after checking
// whether the bootloader applied them (and cleaning up the staged files
// if it did).
//
#define RPI_BLCFG_STAGED_VARIABLE_NAME  L"BlCfgStaged"

//
// BOOT_ORDER as text ("0xf461"): up to 8 nibbles plus the 0x prefix.
//
#define RPI_BLCFG_BOOT_ORDER_MAXLEN  11

//
// QuestionId of the interactive "stage EEPROM update and reboot" action.
//
#define RPI_BLCFG_KEY_STAGE  0x1000

#pragma pack (1)
typedef struct {
  //
  // BOOT_ORDER hex string, e.g. "0xf461". Nibbles are tried right to
  // left; see the Raspberry Pi bootloader documentation for the codes.
  //
  CHAR16    BootOrder[RPI_BLCFG_BOOT_ORDER_MAXLEN + 1];
  UINT8     BootUart;         // BOOT_UART: 0/1, absent default 0
  UINT8     PowerOffOnHalt;   // POWER_OFF_ON_HALT: 0/1, absent default 0
  UINT8     WakeOnGpio;       // WAKE_ON_GPIO: 0/1, absent default 1
  UINT16    PsuMaxCurrent;    // PSU_MAX_CURRENT: 0 (auto/absent), 3000, 5000
} RPI_BLCFG_DATA;
#pragma pack ()

#endif // RPI_BLCFG_H_
