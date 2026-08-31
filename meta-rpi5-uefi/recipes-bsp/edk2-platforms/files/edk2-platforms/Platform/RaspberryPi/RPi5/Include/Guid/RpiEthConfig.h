/** @file

  EthCfg - the persistent BMC-managed IPv4 policy for the onboard NIC,
  shared between EthConfigDxe (efivarstore Setup page + boot-time apply
  into Ip4Config2) and any BMC-side writer that reaches UEFI variables
  (the Redfish Bios feature driver PATCHing /Systems/1/Bios attributes).

  This header is included by VFR as well as C: keep it to #defines and the
  varstore struct only.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_ETH_CONFIG_H_
#define RPI_ETH_CONFIG_H_

//
// Vendor GUID of the EthCfg variable AND the EthConfigDxe formset
// (one GUID for both, the ConfigDxe idiom).
//
#define RPI_ETH_CONFIG_FORMSET_GUID \
  { 0x8c8b17ee, 0x61bd, 0x4efb, { 0x80, 0xd6, 0x6c, 0xbf, 0x34, 0x25, 0x6b, 0x12 } }

#define RPI_ETH_CONFIG_VARIABLE_NAME  L"EthCfg"

#define RPI_ETH_IP4_MODE_UNMANAGED  0    // leave Ip4Config2 (and its Setup form) alone
#define RPI_ETH_IP4_MODE_DHCP       1    // force DHCP policy every boot
#define RPI_ETH_IP4_MODE_STATIC     2    // force the static settings below every boot

//
// "255.255.255.255" plus the terminating NUL.
//
#define RPI_ETH_IP4_STR_SIZE  16

#pragma pack (1)
typedef struct {
  UINT8     Ip4Mode;                                // RPI_ETH_IP4_MODE_*
  //
  // Dotted-quad strings, NUL terminated, empty when unset. Address and
  // SubnetMask are required in STATIC mode (a parse failure keeps the
  // boot on the NIC's existing configuration); Gateway and the two DNS
  // servers are optional.
  //
  CHAR16    Ip4Address[RPI_ETH_IP4_STR_SIZE];
  CHAR16    Ip4SubnetMask[RPI_ETH_IP4_STR_SIZE];
  CHAR16    Ip4Gateway[RPI_ETH_IP4_STR_SIZE];
  CHAR16    Ip4Dns1[RPI_ETH_IP4_STR_SIZE];
  CHAR16    Ip4Dns2[RPI_ETH_IP4_STR_SIZE];
} RPI_ETH_CONFIG;
#pragma pack ()

#endif // RPI_ETH_CONFIG_H_
